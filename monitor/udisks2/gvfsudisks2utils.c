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

      /* The code doesn't care about prefix, which may cause problems for
       * options like "auto" and "noauto". However, this function is only used
       * with our "x-gvfs-*" options, where mentioned problems are unlikely.
       * Be careful, that some people rely on this bug and use "comment=x-gvfs-*"
       * as workaround, see: https://gitlab.gnome.org/GNOME/gvfs/issues/348
       */
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
  GMainContext *main_context; /* may be NULL */

  gchar *command_line;

  gulong cancellable_handler_id;

  GPid child_pid;
  gint child_stdout_fd;
  gint child_stderr_fd;

  GIOChannel *child_stdout_channel;
  GIOChannel *child_stderr_channel;

  GSource *child_watch_source;
  GSource *child_stdout_source;
  GSource *child_stderr_source;

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

  if (data->main_context != NULL)
    g_main_context_unref (data->main_context);

  g_free (data->command_line);

  g_slice_free (SpawnData, data);
}

/* called in the thread where @cancellable was cancelled */
static void
on_cancelled (GCancellable *cancellable,
              gpointer      user_data)
{
  GTask *task = G_TASK (user_data);

  g_assert (g_task_return_error_if_cancelled (task));
  g_object_unref (task);
}

static gboolean
read_child_stderr (GIOChannel *channel,
                   GIOCondition condition,
                   gpointer user_data)
{
  SpawnData *data = g_task_get_task_data (G_TASK (user_data));
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
  SpawnData *data = g_task_get_task_data (G_TASK (user_data));
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
  GTask *task = G_TASK (user_data);
  SpawnData *data = g_task_get_task_data (task);
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
  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

static gboolean
timeout_cb (gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  SpawnData *data = g_task_get_task_data (task);

  /* ok, timeout is history, make sure we don't free it in spawn_data_free() */
  data->timeout_source = NULL;

  /* we're done */
  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                           _("Timed out running command-line “%s”"),
                           data->command_line);
  g_object_unref (task);

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
  GTask *task;

  task = g_task_new (NULL, cancellable, callback, user_data);
  g_task_set_source_tag (task, gvfs_udisks2_utils_spawn);

  data = g_slice_new0 (SpawnData);
  data->main_context = g_main_context_get_thread_default ();
  if (data->main_context != NULL)
    g_main_context_ref (data->main_context);

  va_start (var_args, command_line_format);
  data->command_line = g_strdup_vprintf (command_line_format, var_args);
  va_end (var_args);

  data->child_stdout = g_string_new (NULL);
  data->child_stderr = g_string_new (NULL);
  data->child_stdout_fd = -1;
  data->child_stderr_fd = -1;

  g_task_set_task_data (task, data, (GDestroyNotify)spawn_data_free);

  error = NULL;
  if (cancellable != NULL)
    {
      /* could already be cancelled */
      if (g_task_return_error_if_cancelled (task))
        {
          g_object_unref (task);
          goto out;
        }

      data->cancellable_handler_id = g_cancellable_connect (cancellable,
                                                            G_CALLBACK (on_cancelled),
                                                            task,
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
      g_task_return_error (task, error);
      g_object_unref (task);
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
      g_task_return_error (task, error);
      g_object_unref (task);
      goto out;
    }

  if (timeout_seconds > 0)
    {
      data->timeout_source = g_timeout_source_new_seconds (timeout_seconds);
      g_source_set_priority (data->timeout_source, G_PRIORITY_DEFAULT);
      g_source_set_callback (data->timeout_source, timeout_cb, task, NULL);
      g_source_attach (data->timeout_source, data->main_context);
      g_source_unref (data->timeout_source);
    }

  data->child_watch_source = g_child_watch_source_new (data->child_pid);
  g_source_set_callback (data->child_watch_source, (GSourceFunc) child_watch_cb, task, NULL);
  g_source_attach (data->child_watch_source, data->main_context);
  g_source_unref (data->child_watch_source);

  data->child_stdout_channel = g_io_channel_unix_new (data->child_stdout_fd);
  g_io_channel_set_flags (data->child_stdout_channel, G_IO_FLAG_NONBLOCK, NULL);
  data->child_stdout_source = g_io_create_watch (data->child_stdout_channel, G_IO_IN);
  g_source_set_callback (data->child_stdout_source, (GSourceFunc) read_child_stdout, task, NULL);
  g_source_attach (data->child_stdout_source, data->main_context);
  g_source_unref (data->child_stdout_source);

  data->child_stderr_channel = g_io_channel_unix_new (data->child_stderr_fd);
  g_io_channel_set_flags (data->child_stderr_channel, G_IO_FLAG_NONBLOCK, NULL);
  data->child_stderr_source = g_io_create_watch (data->child_stderr_channel, G_IO_IN);
  g_source_set_callback (data->child_stderr_source, (GSourceFunc) read_child_stderr, task, NULL);
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
  SpawnData *data;

  g_return_val_if_fail (g_task_is_valid (res, NULL), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (res, gvfs_udisks2_utils_spawn), FALSE);

  data = g_task_get_task_data (G_TASK (res));
  if (data->cancellable_handler_id > 0)
    {
      g_cancellable_disconnect (g_task_get_cancellable (G_TASK (res)), data->cancellable_handler_id);
      data->cancellable_handler_id = 0;
    }

  if (g_task_had_error (G_TASK (res)))
    goto out;

  if (out_exit_status != NULL)
    *out_exit_status = data->exit_status;

  if (out_standard_output != NULL)
    *out_standard_output = g_strdup (data->child_stdout->str);

  if (out_standard_error != NULL)
    *out_standard_error = g_strdup (data->child_stderr->str);

 out:
  return g_task_propagate_boolean (G_TASK (res), error);
}

/* ---------------------------------------------------------------------------------------------------- */

#if defined(HAVE_LOGIND)
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

  data->unmount_timer_id = 0;

  if (data->unmount_fired)
    goto out;

  /* TODO: it would be nice to include and update the time left and
   * bytes left fields.
   */
  data->unmount_fired = TRUE;

  name = unmount_notify_get_name (data);
  if (data->mount)
    message = g_strdup_printf (_("Unmounting %s\nDisconnecting from filesystem."), name);
  else
    message = g_strdup_printf (_("Writing data to %s\nDevice should not be unplugged."), name);

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
unmount_notify_op_reply (UnmountNotifyData *data,
                         GMountOperationResult result)
{
  gint choice;

  choice = g_mount_operation_get_choice (data->op);

  if ((result == G_MOUNT_OPERATION_HANDLED && data->show_processes_up && choice == 1) ||
      result == G_MOUNT_OPERATION_ABORTED)
    unmount_notify_stop_timer (data);
  else if (result == G_MOUNT_OPERATION_HANDLED)
    unmount_notify_ensure_timer (data);

  data->show_processes_up = FALSE;
}

static void
unmount_notify_data_free (gpointer user_data)
{
  UnmountNotifyData *data = user_data;

  unmount_notify_stop_timer (data);
  g_signal_handlers_disconnect_by_data (data->op, data);

  g_clear_object (&data->mount);
  g_clear_object (&data->drive);

  g_slice_free (UnmountNotifyData, data);
}

static UnmountNotifyData *
unmount_notify_data_for_operation (GMountOperation *op,
                                   GMount          *mount,
                                   GDrive          *drive)
{
  UnmountNotifyData *data;

  data = g_object_get_data (G_OBJECT (op), "x-udisks2-notify-data");
  if (data != NULL)
    return data;

  data = g_slice_new0 (UnmountNotifyData);
  data->op = op;

  if (mount)
    data->mount = g_object_ref (mount);
  if (drive)
    data->drive = g_object_ref (drive);

  g_object_set_data_full (G_OBJECT (data->op),
                          "x-udisks2-notify-data", data, 
                          unmount_notify_data_free);

  g_signal_connect_swapped (data->op, "aborted",
                            G_CALLBACK (unmount_notify_stop_timer), data);
  g_signal_connect_swapped (data->op, "show-processes",
                            G_CALLBACK (unmount_notify_op_show_processes), data);
  g_signal_connect_swapped (data->op, "reply",
                            G_CALLBACK (unmount_notify_op_reply), data);

  return data;
}

void
gvfs_udisks2_unmount_notify_start (GMountOperation *op,
                                   GMount          *mount,
                                   GDrive          *drive)
{
  UnmountNotifyData *data;

  data = unmount_notify_data_for_operation (op, mount, drive);
  unmount_notify_ensure_timer (data);
}

void
gvfs_udisks2_unmount_notify_stop (GMountOperation *op,
                                  gboolean         unmount_failed)
{
  gchar *message, *name;
  UnmountNotifyData *data = g_object_steal_data (G_OBJECT (op), "x-udisks2-notify-data");

  if (data == NULL)
    return;

  unmount_notify_stop_timer (data);

  if (unmount_failed)
    {
      unmount_notify_data_free (data);
      return;
    }

  name = unmount_notify_get_name (data);
  if (data->mount)
    message = g_strdup_printf (_("%s unmounted\nFilesystem has been disconnected."), name);
  else
    message = g_strdup_printf (_("%s can be safely unplugged\nDevice can be removed."), name);

  g_signal_emit_by_name (data->op, "show-unmount-progress",
                         message, 0, 0);

  unmount_notify_data_free (data);
  g_free (message);
  g_free (name);
}
