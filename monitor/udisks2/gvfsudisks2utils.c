/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2012 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "gvfsudisks2utils.h"

void
gvfs_udisks2_utils_udisks_error_to_gio_error (GError *error)
{
  g_return_if_fail (error != NULL);

  if (error->domain == UDISKS_ERROR)
    {
      switch (error->code)
        {
        case UDISKS_ERROR_DEVICE_BUSY:
          error->code = G_IO_ERROR_BUSY;
          break;
        case UDISKS_ERROR_NOT_AUTHORIZED_DISMISSED:
          error->code = G_IO_ERROR_FAILED_HANDLED;
          break;
        default:
          error->code = G_IO_ERROR_FAILED;
          break;
        }
    }
  else
    {
      error->code = G_IO_ERROR_FAILED;
    }

  error->domain = G_IO_ERROR;
  g_dbus_error_strip_remote_error (error);
}


GIcon *
gvfs_udisks2_utils_icon_from_fs_type (const gchar *fs_type)
{
  const gchar *icon_name;
  if (g_strcmp0 (fs_type, "nfs") == 0 ||
      g_strcmp0 (fs_type, "nfs4") == 0 ||
      g_strcmp0 (fs_type, "cifs") == 0)
    {
      icon_name = "folder-remote";
    }
  else
    {
      icon_name = "drive-removable-media";
    }
  return g_themed_icon_new_with_default_fallbacks (icon_name);
}

GIcon *
gvfs_udisks2_utils_symbolic_icon_from_fs_type (const gchar *fs_type)
{
  const gchar *icon_name;
  if (g_strcmp0 (fs_type, "nfs") == 0 ||
      g_strcmp0 (fs_type, "nfs4") == 0 ||
      g_strcmp0 (fs_type, "cifs") == 0)
    {
      icon_name = "folder-remote-symbolic";
    }
  else
    {
      icon_name = "drive-removable-media-symbolic";
    }
  return g_themed_icon_new_with_default_fallbacks (icon_name);
}

gchar *
gvfs_udisks2_utils_lookup_fstab_options_value (const gchar *fstab_options,
                                               const gchar *key)
{
  gchar *ret = NULL;

  if (fstab_options != NULL)
    {
      const gchar *start;
      guint n;

      start = strstr (fstab_options, key);
      if (start != NULL)
        {
          start += strlen (key);
          for (n = 0; start[n] != ',' && start[n] != '\0'; n++)
            ;
          if (n == 0)
            ret = g_strdup ("");
          else if (n >= 1)
            ret = g_uri_unescape_segment (start, start + n, NULL);
        }
    }
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  GSimpleAsyncResult *simple; /* borrowed reference */
  GMainContext *main_context; /* may be NULL */

  gchar *command_line;

  GCancellable *cancellable;  /* may be NULL */
  gulong cancellable_handler_id;

  GPid child_pid;
  gint child_stdout_fd;
  gint child_stderr_fd;

  GIOChannel *child_stdout_channel;
  GIOChannel *child_stderr_channel;

  GSource *child_watch_source;
  GSource *child_stdout_source;
  GSource *child_stderr_source;

  gboolean timed_out;
  GSource *timeout_source;

  GString *child_stdout;
  GString *child_stderr;

  gint exit_status;
} SpawnData;

static void
child_watch_from_release_cb (GPid     pid,
                             gint     status,
                             gpointer user_data)
{
}

static void
spawn_data_free (SpawnData *data)
{
  if (data->timeout_source != NULL)
    {
      g_source_destroy (data->timeout_source);
      data->timeout_source = NULL;
    }

  /* Nuke the child, if necessary */
  if (data->child_watch_source != NULL)
    {
      g_source_destroy (data->child_watch_source);
      data->child_watch_source = NULL;
    }

  if (data->child_pid != 0)
    {
      GSource *source;
      kill (data->child_pid, SIGTERM);
      /* OK, we need to reap for the child ourselves - we don't want
       * to use waitpid() because that might block the calling
       * thread (the child might handle SIGTERM and use several
       * seconds for cleanup/rollback).
       *
       * So we use GChildWatch instead.
       *
       * Avoid taking a references to ourselves. but note that we need
       * to pass the GSource so we can nuke it once handled.
       */
      source = g_child_watch_source_new (data->child_pid);
      g_source_set_callback (source,
                             (GSourceFunc) child_watch_from_release_cb,
                             source,
                             (GDestroyNotify) g_source_destroy);
      g_source_attach (source, data->main_context);
      g_source_unref (source);
      data->child_pid = 0;
    }

  if (data->child_stdout != NULL)
    {
      g_string_free (data->child_stdout, TRUE);
      data->child_stdout = NULL;
    }

  if (data->child_stderr != NULL)
    {
      g_string_free (data->child_stderr, TRUE);
      data->child_stderr = NULL;
    }

  if (data->child_stdout_channel != NULL)
    {
      g_io_channel_unref (data->child_stdout_channel);
      data->child_stdout_channel = NULL;
    }
  if (data->child_stderr_channel != NULL)
    {
      g_io_channel_unref (data->child_stderr_channel);
      data->child_stderr_channel = NULL;
    }

  if (data->child_stdout_source != NULL)
    {
      g_source_destroy (data->child_stdout_source);
      data->child_stdout_source = NULL;
    }
  if (data->child_stderr_source != NULL)
    {
      g_source_destroy (data->child_stderr_source);
      data->child_stderr_source = NULL;
    }

  if (data->child_stdout_fd != -1)
    {
      g_warn_if_fail (close (data->child_stdout_fd) == 0);
      data->child_stdout_fd = -1;
    }
  if (data->child_stderr_fd != -1)
    {
      g_warn_if_fail (close (data->child_stderr_fd) == 0);
      data->child_stderr_fd = -1;
    }

  if (data->cancellable_handler_id > 0)
    {
      g_cancellable_disconnect (data->cancellable, data->cancellable_handler_id);
      data->cancellable_handler_id = 0;
    }

  if (data->main_context != NULL)
    g_main_context_unref (data->main_context);

  if (data->cancellable != NULL)
    g_object_unref (data->cancellable);

  g_free (data->command_line);

  g_slice_free (SpawnData, data);
}

/* called in the thread where @cancellable was cancelled */
static void
on_cancelled (GCancellable *cancellable,
              gpointer      user_data)
{
  SpawnData *data = user_data;
  GError *error;

  error = NULL;
  g_warn_if_fail (g_cancellable_set_error_if_cancelled (cancellable, &error));
  g_simple_async_result_take_error (data->simple, error);
  g_simple_async_result_complete_in_idle (data->simple);
  g_object_unref (data->simple);
}

static gboolean
read_child_stderr (GIOChannel *channel,
                   GIOCondition condition,
                   gpointer user_data)
{
  SpawnData *data = user_data;
  gchar buf[1024];
  gsize bytes_read;

  g_io_channel_read_chars (channel, buf, sizeof buf, &bytes_read, NULL);
  g_string_append_len (data->child_stderr, buf, bytes_read);
  return TRUE;
}

static gboolean
read_child_stdout (GIOChannel *channel,
                   GIOCondition condition,
                   gpointer user_data)
{
  SpawnData *data = user_data;
  gchar buf[1024];
  gsize bytes_read;

  g_io_channel_read_chars (channel, buf, sizeof buf, &bytes_read, NULL);
  g_string_append_len (data->child_stdout, buf, bytes_read);
  return TRUE;
}

static void
child_watch_cb (GPid     pid,
                gint     status,
                gpointer user_data)
{
  SpawnData *data = user_data;
  gchar *buf;
  gsize buf_size;

  if (g_io_channel_read_to_end (data->child_stdout_channel, &buf, &buf_size, NULL) == G_IO_STATUS_NORMAL)
    {
      g_string_append_len (data->child_stdout, buf, buf_size);
      g_free (buf);
    }
  if (g_io_channel_read_to_end (data->child_stderr_channel, &buf, &buf_size, NULL) == G_IO_STATUS_NORMAL)
    {
      g_string_append_len (data->child_stderr, buf, buf_size);
      g_free (buf);
    }

  data->exit_status = status;

  /* ok, child watch is history, make sure we don't free it in spawn_data_free() */
  data->child_pid = 0;
  data->child_watch_source = NULL;

  /* we're done */
  g_simple_async_result_complete_in_idle (data->simple);
  g_object_unref (data->simple);
}

static gboolean
timeout_cb (gpointer user_data)
{
  SpawnData *data = user_data;

  data->timed_out = TRUE;

  /* ok, timeout is history, make sure we don't free it in spawn_data_free() */
  data->timeout_source = NULL;

  /* we're done */
  g_simple_async_result_complete_in_idle (data->simple);
  g_object_unref (data->simple);

  return FALSE; /* remove source */
}

void
gvfs_udisks2_utils_spawn (guint                timeout_seconds,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data,
                          const gchar         *command_line_format,
                          ...)
{
  va_list var_args;
  SpawnData *data;
  GError *error;
  gint child_argc;
  gchar **child_argv = NULL;

  data = g_slice_new0 (SpawnData);
  data->simple = g_simple_async_result_new (NULL,
                                            callback,
                                            user_data,
                                            gvfs_udisks2_utils_spawn);
  data->main_context = g_main_context_get_thread_default ();
  if (data->main_context != NULL)
    g_main_context_ref (data->main_context);

  data->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;

  va_start (var_args, command_line_format);
  data->command_line = g_strdup_vprintf (command_line_format, var_args);
  va_end (var_args);

  data->child_stdout = g_string_new (NULL);
  data->child_stderr = g_string_new (NULL);
  data->child_stdout_fd = -1;
  data->child_stderr_fd = -1;

  /* the life-cycle of SpawnData is tied to its GSimpleAsyncResult */
  g_simple_async_result_set_op_res_gpointer (data->simple, data, (GDestroyNotify) spawn_data_free);

  error = NULL;
  if (data->cancellable != NULL)
    {
      /* could already be cancelled */
      error = NULL;
      if (g_cancellable_set_error_if_cancelled (data->cancellable, &error))
        {
          g_simple_async_result_take_error (data->simple, error);
          g_simple_async_result_complete_in_idle (data->simple);
          g_object_unref (data->simple);
          goto out;
        }

      data->cancellable_handler_id = g_cancellable_connect (data->cancellable,
                                                            G_CALLBACK (on_cancelled),
                                                            data,
                                                            NULL);
    }

  error = NULL;
  if (!g_shell_parse_argv (data->command_line,
                           &child_argc,
                           &child_argv,
                           &error))
    {
      g_prefix_error (&error,
                      "Error parsing command-line `%s': ",
                      data->command_line);
      g_simple_async_result_take_error (data->simple, error);
      g_simple_async_result_complete_in_idle (data->simple);
      g_object_unref (data->simple);
      goto out;
    }

  error = NULL;
  if (!g_spawn_async_with_pipes (NULL, /* working directory */
                                 child_argv,
                                 NULL, /* envp */
                                 G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                 NULL, /* child_setup */
                                 NULL, /* child_setup's user_data */
                                 &(data->child_pid),
                                 NULL, /* gint *stdin_fd */
                                 &(data->child_stdout_fd),
                                 &(data->child_stderr_fd),
                                 &error))
    {
      g_prefix_error (&error,
                      "Error spawning command-line `%s': ",
                      data->command_line);
      g_simple_async_result_take_error (data->simple, error);
      g_simple_async_result_complete_in_idle (data->simple);
      g_object_unref (data->simple);
      goto out;
    }

  if (timeout_seconds > 0)
    {
      data->timeout_source = g_timeout_source_new_seconds (timeout_seconds);
      g_source_set_priority (data->timeout_source, G_PRIORITY_DEFAULT);
      g_source_set_callback (data->timeout_source, timeout_cb, data, NULL);
      g_source_attach (data->timeout_source, data->main_context);
      g_source_unref (data->timeout_source);
    }

  data->child_watch_source = g_child_watch_source_new (data->child_pid);
  g_source_set_callback (data->child_watch_source, (GSourceFunc) child_watch_cb, data, NULL);
  g_source_attach (data->child_watch_source, data->main_context);
  g_source_unref (data->child_watch_source);

  data->child_stdout_channel = g_io_channel_unix_new (data->child_stdout_fd);
  g_io_channel_set_flags (data->child_stdout_channel, G_IO_FLAG_NONBLOCK, NULL);
  data->child_stdout_source = g_io_create_watch (data->child_stdout_channel, G_IO_IN);
  g_source_set_callback (data->child_stdout_source, (GSourceFunc) read_child_stdout, data, NULL);
  g_source_attach (data->child_stdout_source, data->main_context);
  g_source_unref (data->child_stdout_source);

  data->child_stderr_channel = g_io_channel_unix_new (data->child_stderr_fd);
  g_io_channel_set_flags (data->child_stderr_channel, G_IO_FLAG_NONBLOCK, NULL);
  data->child_stderr_source = g_io_create_watch (data->child_stderr_channel, G_IO_IN);
  g_source_set_callback (data->child_stderr_source, (GSourceFunc) read_child_stderr, data, NULL);
  g_source_attach (data->child_stderr_source, data->main_context);
  g_source_unref (data->child_stderr_source);

 out:
  g_strfreev (child_argv);
}

gboolean
gvfs_udisks2_utils_spawn_finish (GAsyncResult   *res,
                                 gint           *out_exit_status,
                                 gchar         **out_standard_output,
                                 gchar         **out_standard_error,
                                 GError        **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  SpawnData *data;
  gboolean ret = FALSE;

  g_return_val_if_fail (G_IS_ASYNC_RESULT (res), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == gvfs_udisks2_utils_spawn);

  if (g_simple_async_result_propagate_error (simple, error))
    goto out;

  data = g_simple_async_result_get_op_res_gpointer (simple);

  if (data->timed_out)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_TIMED_OUT,
                   _("Timed out running command-line `%s'"),
                   data->command_line);
      goto out;
    }

  if (out_exit_status != NULL)
    *out_exit_status = data->exit_status;

  if (out_standard_output != NULL)
    *out_standard_output = g_strdup (data->child_stdout->str);

  if (out_standard_error != NULL)
    *out_standard_error = g_strdup (data->child_stderr->str);

  ret = TRUE;

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

#if defined(HAVE_LIBSYSTEMD_LOGIN)
#include <systemd/sd-login.h>

static const gchar *
get_seat (void)
{
  static gsize once = 0;
  static char *seat = NULL;

  if (g_once_init_enter (&once))
    {
      char *session = NULL;
      if (sd_pid_get_session (getpid (), &session) == 0)
        {
          sd_session_get_seat (session, &seat);
          free (session);
          /* we intentionally leak seat here... */
        }
      g_once_init_leave (&once, (gsize) 1);
    }
  return seat;
}

#else

static const gchar *
get_seat (void)
{
  return NULL;
}

#endif

gboolean
gvfs_udisks2_utils_is_drive_on_our_seat (UDisksDrive *drive)
{
  gboolean ret = FALSE;
  const gchar *seat;
  const gchar *drive_seat = NULL;

  /* assume our own seat if we don't have seat-support or it doesn't work */
  seat = get_seat ();
  if (seat == NULL)
    {
      ret = TRUE;
      goto out;
    }

  /* If the device is not tagged, assume that udisks does not have
   * working seat-support... so just assume it's available at our
   * seat.
   *
   * Note that seat support was added in udisks 1.95.0 (and so was the
   * UDISKS_CHECK_VERSION macro).
   */
  drive_seat = udisks_drive_get_seat (drive);

  if (drive_seat == NULL || strlen (drive_seat) == 0)
    {
      ret = TRUE;
      goto out;
    }

  /* Otherwise, check if it's on our seat */
  if (g_strcmp0 (seat, drive_seat) == 0)
    ret = TRUE;

 out:
  return ret;
}

/* unmount progress notification utilities */
typedef struct {
  GMount *mount;
  GDrive *drive;

  GMountOperation *op;
  gboolean op_aborted;
  gboolean generic_text;
  gboolean show_processes_up;

  guint unmount_timer_id;
  gboolean unmount_fired;
} UnmountNotifyData;

static gboolean
unmount_notify_should_show (UnmountNotifyData *data)
{
  GVolume *volume;
  gchar *identifier = NULL;
  gboolean retval = TRUE;

  if (data->mount)
    {
      volume = g_mount_get_volume (data->mount);

      if (volume)
        {
          identifier = g_volume_get_identifier (volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
          g_object_unref (volume);
        }
    }
  else if (data->drive)
    {
      identifier = g_drive_get_identifier (data->drive, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
    }

  if (identifier && g_str_has_prefix (identifier, "/dev/sr"))
    retval = FALSE;

  g_free (identifier);

  return retval;
}

static gchar *
unmount_notify_get_name (UnmountNotifyData *data)
{
  if (data->mount)
    return g_mount_get_name (data->mount);
  else
    return g_drive_get_name (data->drive);
}

static gboolean
unmount_notify_timer_cb (gpointer user_data)
{
  UnmountNotifyData *data = user_data;
  gchar *message, *name;
  const gchar *format;

  data->unmount_timer_id = 0;

  if (data->unmount_fired)
    goto out;

  /* TODO: it would be nice to include and update the time left and
   * bytes left fields.
   */
  data->unmount_fired = TRUE;

  name = unmount_notify_get_name (data);
  format = data->generic_text ?
    _("Unmounting %s\nPlease wait") :
    _("Writing data to %s\nDon't unplug until finished");

  message = g_strdup_printf (format, name);
  g_signal_emit_by_name (data->op, "show-unmount-progress",
                         message, -1, -1);
  g_free (message);
  g_free (name);

 out:
  return FALSE;
}

static void
unmount_notify_ensure_timer (UnmountNotifyData *data)
{
  if (data->unmount_timer_id > 0)
    return;

  if (!unmount_notify_should_show (data))
    return;

  data->unmount_timer_id = 
    g_timeout_add (1500, unmount_notify_timer_cb, data);
}

static void
unmount_notify_stop_timer (UnmountNotifyData *data)
{
  if (data->unmount_timer_id > 0)
    {
      g_source_remove (data->unmount_timer_id);
      data->unmount_timer_id = 0;
    }
}

static void
unmount_notify_op_show_processes (UnmountNotifyData *data)
{
  unmount_notify_stop_timer (data);
  data->show_processes_up = TRUE;
}

static void
unmount_notify_op_aborted (UnmountNotifyData *data)
{
  unmount_notify_stop_timer (data);
  data->op_aborted = TRUE;
}

static void
unmount_notify_op_reply (UnmountNotifyData *data,
                         GMountOperationResult result)
{
  gint choice;

  choice = g_mount_operation_get_choice (data->op);

  if ((result == G_MOUNT_OPERATION_HANDLED && data->show_processes_up && choice == 1) ||
      result == G_MOUNT_OPERATION_ABORTED)
    unmount_notify_op_aborted (data);
  else if (result == G_MOUNT_OPERATION_HANDLED)
    unmount_notify_ensure_timer (data);

  data->show_processes_up = FALSE;
}

static void
unmount_notify_data_free (gpointer user_data)
{
  UnmountNotifyData *data = user_data;

  unmount_notify_stop_timer (data);

  g_clear_object (&data->mount);
  g_clear_object (&data->drive);

  g_slice_free (UnmountNotifyData, data);
}

static UnmountNotifyData *
unmount_notify_data_for_operation (GMountOperation *op,
                                   GMount          *mount,
                                   GDrive          *drive,
                                   gboolean         generic_text)
{
  UnmountNotifyData *data;

  data = g_object_get_data (G_OBJECT (op), "x-udisks2-notify-data");
  if (data != NULL)
    return data;

  data = g_slice_new0 (UnmountNotifyData);
  data->op = op;
  data->generic_text = generic_text;

  if (mount)
    data->mount = g_object_ref (mount);
  if (drive)
    data->drive = g_object_ref (drive);

  g_object_set_data_full (G_OBJECT (data->op),
                          "x-udisks2-notify-data", data, 
                          unmount_notify_data_free);

  g_signal_connect_swapped (data->op, "aborted",
                            G_CALLBACK (unmount_notify_op_aborted), data);
  g_signal_connect_swapped (data->op, "show-processes",
                            G_CALLBACK (unmount_notify_op_show_processes), data);
  g_signal_connect_swapped (data->op, "reply",
                            G_CALLBACK (unmount_notify_op_reply), data);

  return data;
}

void
gvfs_udisks2_unmount_notify_start (GMountOperation *op,
                                   GMount          *mount,
                                   GDrive          *drive,
                                   gboolean         generic_text)
{
  UnmountNotifyData *data;

  data = unmount_notify_data_for_operation (op, mount, drive, generic_text);
  unmount_notify_ensure_timer (data);
}

void
gvfs_udisks2_unmount_notify_stop (GMountOperation *op)
{
  gchar *message, *name;
  const gchar *format;
  UnmountNotifyData *data = g_object_get_data (G_OBJECT (op), "x-udisks2-notify-data");

  if (data == NULL)
    return;

  unmount_notify_stop_timer (data);

  if (data->op_aborted)
    return;
  if (!data->unmount_fired)
    return;

  name = unmount_notify_get_name (data);
  format = data->generic_text ?
    _("%s has been unmounted\n") : _("You can now unplug %s\n");

  message = g_strdup_printf (format, name);
  g_signal_emit_by_name (data->op, "show-unmount-progress",
                         message, 0, 0);

  g_free (message);
  g_free (name);
}
