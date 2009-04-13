/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2009 Red Hat, Inc.
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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include <gvfsmountinfo.h>

#include "ggduvolumemonitor.h"
#include "ggdumount.h"
#include "ggduvolume.h"

struct _GGduMount
{
  GObject parent;

  GVolumeMonitor *volume_monitor; /* owned by volume monitor */
  GGduVolume *volume;             /* owned by volume monitor */

  /* the following members need to be set upon construction */
  GFile *root;
  GIcon *icon;
  gchar *name;
  gchar *uuid;
  gchar *device_file;
  gchar *mount_path;
  gboolean can_unmount;

  gchar *mount_entry_name;
  GIcon *mount_entry_icon;

  gboolean is_burn_mount;

  GIcon *autorun_icon;
  gboolean searched_for_autorun;

  gchar *xdg_volume_info_name;
  GIcon *xdg_volume_info_icon;
  gboolean searched_for_xdg_volume_info;
};

static gboolean update_mount (GGduMount *mount);

static void g_gdu_mount_mount_iface_init (GMountIface *iface);

G_DEFINE_TYPE_EXTENDED (GGduMount, g_gdu_mount, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT,
                                               g_gdu_mount_mount_iface_init))

static void
volume_changed (GVolume    *volume,
                gpointer    user_data);

static void
g_gdu_mount_finalize (GObject *object)
{
  GGduMount *mount;

  mount = G_GDU_MOUNT (object);

  if (mount->volume != NULL)
    {
      g_signal_handlers_disconnect_by_func (mount->volume, volume_changed, mount);
      g_gdu_volume_unset_mount (mount->volume, mount);
    }

  if (mount->root != NULL)
    g_object_unref (mount->root);
  if (mount->icon != NULL)
    g_object_unref (mount->icon);
  g_free (mount->name);
  g_free (mount->uuid);
  g_free (mount->device_file);
  g_free (mount->mount_path);

  g_free (mount->mount_entry_name);
  if (mount->mount_entry_icon != NULL)
    g_object_unref (mount->mount_entry_icon);

  if (mount->autorun_icon != NULL)
    g_object_unref (mount->autorun_icon);

  g_free (mount->xdg_volume_info_name);
  if (mount->xdg_volume_info_icon != NULL)
    g_object_unref (mount->xdg_volume_info_icon);

  if (G_OBJECT_CLASS (g_gdu_mount_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_gdu_mount_parent_class)->finalize) (object);
}

static void
g_gdu_mount_class_init (GGduMountClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_gdu_mount_finalize;
}

static void
g_gdu_mount_init (GGduMount *mount)
{
}

static void
emit_changed (GGduMount *mount)
{
  g_signal_emit_by_name (mount, "changed");
  g_signal_emit_by_name (mount->volume_monitor, "mount_changed", mount);
}

static void
got_autorun_info_cb (GObject      *source_object,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  GGduMount *mount = G_GDU_MOUNT (user_data);

  mount->autorun_icon = g_vfs_mount_info_query_autorun_info_finish (G_FILE (source_object),
                                                                    res,
                                                                    NULL);

  if (update_mount (mount))
    emit_changed (mount);

  g_object_unref (mount);
}

static void
got_xdg_volume_info_cb (GObject      *source_object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  GGduMount *mount = G_GDU_MOUNT (user_data);

  mount->xdg_volume_info_icon = g_vfs_mount_info_query_xdg_volume_info_finish (G_FILE (source_object),
                                                                               res,
                                                                               &(mount->xdg_volume_info_name),
                                                                               NULL);
  if (update_mount (mount))
    emit_changed (mount);

  g_object_unref (mount);
}

static gboolean
update_mount (GGduMount *mount)
{
  gboolean changed;
  gboolean old_can_unmount;
  gchar *old_name;
  GIcon *old_icon;

  /* save old values */
  old_can_unmount = mount->can_unmount;
  old_name = g_strdup (mount->name);
  old_icon = mount->icon != NULL ? g_object_ref (mount->icon) : NULL;

  /* in with the new */
  if (mount->volume != NULL)
    {
      mount->can_unmount = TRUE;

      if (mount->icon != NULL)
        g_object_unref (mount->icon);

      /* order of preference: xdg, autorun, probed */
      if (mount->xdg_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->xdg_volume_info_icon);
      else if (mount->autorun_icon != NULL)
        mount->icon = g_object_ref (mount->autorun_icon);
      else
        mount->icon = g_volume_get_icon (G_VOLUME (mount->volume));

      g_free (mount->name);

      /* order of preference : xdg, probed */
      if (mount->xdg_volume_info_name != NULL)
        mount->name = g_strdup (mount->xdg_volume_info_name);
      else
        mount->name = g_volume_get_name (G_VOLUME (mount->volume));
    }
  else
    {
      mount->can_unmount = TRUE;

      if (mount->icon != NULL)
        g_object_unref (mount->icon);

      /* order of preference: xdg, autorun, probed */
      if (mount->xdg_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->xdg_volume_info_icon);
      else if (mount->autorun_icon != NULL)
        mount->icon = g_object_ref (mount->autorun_icon);
      else
        mount->icon = mount->mount_entry_icon != NULL ? g_object_ref (mount->mount_entry_icon) : NULL;

      g_free (mount->name);

      /* order of preference : xdg, probed */
      if (mount->xdg_volume_info_name != NULL)
        mount->name = g_strdup (mount->xdg_volume_info_name);
      else
        mount->name = g_strdup (mount->mount_entry_name);
    }

  /* compute whether something changed */
  changed = !((old_can_unmount == mount->can_unmount) &&
              (g_strcmp0 (old_name, mount->name) == 0) &&
              g_icon_equal (old_icon, mount->icon)
              );

  /* free old values */
  g_free (old_name);
  if (old_icon != NULL)
    g_object_unref (old_icon);

  /*g_debug ("in update_mount(), changed=%d", changed);*/

  /* search for .xdg-volume-info */
  if (!mount->searched_for_xdg_volume_info)
    {
      mount->searched_for_xdg_volume_info = TRUE;
      g_vfs_mount_info_query_xdg_volume_info (mount->root,
                                              NULL,
                                              got_xdg_volume_info_cb,
                                              g_object_ref (mount));
    }

  /* search for autorun.inf */
  if (!mount->searched_for_autorun)
    {
      mount->searched_for_autorun = TRUE;
      g_vfs_mount_info_query_autorun_info (mount->root,
                                           NULL,
                                           got_autorun_info_cb,
                                           g_object_ref (mount));
    }

  return changed;
}

static void
volume_changed (GVolume  *volume,
                gpointer  user_data)
{
  GGduMount *mount = G_GDU_MOUNT (user_data);

  if (update_mount (mount))
    emit_changed (mount);
}

GGduMount *
g_gdu_mount_new (GVolumeMonitor    *volume_monitor,
                 GUnixMountEntry   *mount_entry,
                 GGduVolume        *volume)
{
  GGduMount *mount;

  mount = NULL;

  /* Ignore internal mounts unless there's a volume */
  if (volume == NULL && (mount_entry != NULL && !g_unix_mount_guess_should_display (mount_entry)))
    goto out;

  mount = g_object_new (G_TYPE_GDU_MOUNT, NULL);
  mount->volume_monitor = volume_monitor;
  g_object_add_weak_pointer (G_OBJECT (volume_monitor), (gpointer) &(mount->volume_monitor));

  if (mount_entry != NULL)
    {
      /* No ref on GUnixMountEntry so save values for later use */
      mount->mount_entry_name = g_unix_mount_guess_name (mount_entry);
      mount->mount_entry_icon = g_unix_mount_guess_icon (mount_entry);
      mount->device_file = g_strdup (g_unix_mount_get_device_path (mount_entry));
      mount->mount_path = g_strdup (g_unix_mount_get_mount_path (mount_entry));
      mount->root = g_file_new_for_path (mount->mount_path);
    }
  else
    {
      /* burn:/// mount (the only mounts we support with mount_entry == NULL) */
      mount->device_file = NULL;
      mount->mount_path = NULL;
      mount->root = g_file_new_for_uri ("burn:///");
      mount->is_burn_mount = TRUE;
    }

  /* need to set the volume only when the mount is fully constructed */
  mount->volume = volume;
  if (mount->volume != NULL)
    {
      g_gdu_volume_set_mount (volume, mount);
      /* this is for piggy backing on the name and icon of the associated volume */
      g_signal_connect (mount->volume, "changed", G_CALLBACK (volume_changed), mount);
    }

  update_mount (mount);

 out:

  return mount;
}

void
g_gdu_mount_unmounted (GGduMount *mount)
{
  if (mount->volume != NULL)
    {
      g_gdu_volume_unset_mount (mount->volume, mount);
      g_signal_handlers_disconnect_by_func (mount->volume, volume_changed, mount);
      mount->volume = NULL;
      emit_changed (mount);
    }
}

void
g_gdu_mount_unset_volume (GGduMount *mount,
                                       GGduVolume  *volume)
{
  if (mount->volume == volume)
    {
      g_signal_handlers_disconnect_by_func (mount->volume, volume_changed, mount);
      mount->volume = NULL;
      emit_changed (mount);
    }
}

static GFile *
g_gdu_mount_get_root (GMount *_mount)
{
  GGduMount *mount = G_GDU_MOUNT (_mount);
  return mount->root != NULL ? g_object_ref (mount->root) : NULL;
}

static GIcon *
g_gdu_mount_get_icon (GMount *_mount)
{
  GGduMount *mount = G_GDU_MOUNT (_mount);
  return mount->icon != NULL ? g_object_ref (mount->icon) : NULL;
}

static gchar *
g_gdu_mount_get_uuid (GMount *_mount)
{
  GGduMount *mount = G_GDU_MOUNT (_mount);
  return g_strdup (mount->uuid);
}

static gchar *
g_gdu_mount_get_name (GMount *_mount)
{
  GGduMount *mount = G_GDU_MOUNT (_mount);
  return g_strdup (mount->name);
}

gboolean
g_gdu_mount_has_uuid (GGduMount         *_mount,
                      const gchar       *uuid)
{
  GGduMount *mount = G_GDU_MOUNT (_mount);
  return g_strcmp0 (mount->uuid, uuid) == 0;
}

gboolean
g_gdu_mount_has_mount_path (GGduMount    *_mount,
                            const gchar  *mount_path)
{
  GGduMount *mount = G_GDU_MOUNT (_mount);
  return g_strcmp0 (mount->mount_path, mount_path) == 0;
}

static GDrive *
g_gdu_mount_get_drive (GMount *_mount)
{
  GGduMount *mount = G_GDU_MOUNT (_mount);
  GDrive *drive;

  drive = NULL;
  if (mount->volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (mount->volume));

  return drive;
}

static GVolume *
g_gdu_mount_get_volume (GMount *_mount)
{
  GGduMount *mount = G_GDU_MOUNT (_mount);
  GVolume *volume;

  volume = NULL;
  if (mount->volume)
    volume = G_VOLUME (g_object_ref (mount->volume));

  return volume;
}

static gboolean
g_gdu_mount_can_unmount (GMount *_mount)
{
  GGduMount *mount = G_GDU_MOUNT (_mount);
  return mount->can_unmount;
}

static gboolean
g_gdu_mount_can_eject (GMount *_mount)
{
  GGduMount *mount = G_GDU_MOUNT (_mount);
  GDrive *drive;
  gboolean can_eject;

  can_eject = FALSE;
  if (mount->volume != NULL)
    {
      drive = g_volume_get_drive (G_VOLUME (mount->volume));
      if (drive != NULL)
        can_eject = g_drive_can_eject (drive);
    }

  return can_eject;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct {
  GMount *mount;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  int error_fd;
  GIOChannel *error_channel;
  guint error_channel_source_id;
  GString *error_string;
} UnmountEjectOp;

static void
eject_unmount_cb (GPid pid, gint status, gpointer user_data)
{
  UnmountEjectOp *data = user_data;
  GSimpleAsyncResult *simple;

  if (WEXITSTATUS (status) != 0)
    {
      GError *error;
      error = g_error_new_literal (G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   data->error_string->str);
      simple = g_simple_async_result_new_from_error (G_OBJECT (data->mount),
                                                     data->callback,
                                                     data->user_data,
                                                     error);
      g_error_free (error);
    }
  else
    {
      simple = g_simple_async_result_new (G_OBJECT (data->mount),
                                          data->callback,
                                          data->user_data,
                                          NULL);
    }

  g_simple_async_result_complete (simple);
  g_object_unref (simple);

  g_source_remove (data->error_channel_source_id);
  g_io_channel_unref (data->error_channel);
  g_string_free (data->error_string, TRUE);
  close (data->error_fd);
  g_spawn_close_pid (pid);
  g_free (data);
}

static gboolean
eject_unmount_read_error (GIOChannel *channel,
                          GIOCondition condition,
                          gpointer user_data)
{
  UnmountEjectOp *data = user_data;
  gchar buf[BUFSIZ];
  gsize bytes_read;
  GError *error;
  GIOStatus status;

  error = NULL;
read:
  status = g_io_channel_read_chars (channel, buf, sizeof (buf), &bytes_read, &error);
  if (status == G_IO_STATUS_NORMAL)
   {
     g_string_append_len (data->error_string, buf, bytes_read);
     if (bytes_read == sizeof (buf))
        goto read;
   }
  else if (status == G_IO_STATUS_EOF)
    g_string_append_len (data->error_string, buf, bytes_read);
  else if (status == G_IO_STATUS_ERROR)
    {
      if (data->error_string->len > 0)
        g_string_append (data->error_string, "\n");

      g_string_append (data->error_string, error->message);
      g_error_free (error);
      return FALSE;
    }

  return TRUE;
}

static void
eject_unmount_do (GMount              *mount,
                  GCancellable        *cancellable,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data,
                  char               **argv)
{
  UnmountEjectOp *data;
  GPid child_pid;
  GError *error;

  data = g_new0 (UnmountEjectOp, 1);
  data->mount = mount;
  data->callback = callback;
  data->user_data = user_data;
  data->cancellable = cancellable;

  error = NULL;
  if (!g_spawn_async_with_pipes (NULL,         /* working dir */
                                 argv,
                                 NULL,         /* envp */
                                 G_SPAWN_DO_NOT_REAP_CHILD|G_SPAWN_SEARCH_PATH,
                                 NULL,         /* child_setup */
                                 NULL,         /* user_data for child_setup */
                                 &child_pid,
                                 NULL,           /* standard_input */
                                 NULL,           /* standard_output */
                                 &(data->error_fd),
                                 &error)) {
    g_assert (error != NULL);
    goto handle_error;
  }

  data->error_string = g_string_new ("");

  data->error_channel = g_io_channel_unix_new (data->error_fd);
  g_io_channel_set_flags (data->error_channel, G_IO_FLAG_NONBLOCK, &error);
  if (error != NULL)
    goto handle_error;

  data->error_channel_source_id = g_io_add_watch (data->error_channel, G_IO_IN, eject_unmount_read_error, data);
  g_child_watch_add (child_pid, eject_unmount_cb, data);

handle_error:

  if (error != NULL)
    {
      GSimpleAsyncResult *simple;
      simple = g_simple_async_result_new_from_error (G_OBJECT (data->mount),
                                                     data->callback,
                                                     data->user_data,
                                                     error);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);

      if (data->error_string != NULL)
        g_string_free (data->error_string, TRUE);

      if (data->error_channel != NULL)
        g_io_channel_unref (data->error_channel);

      g_error_free (error);
      g_free (data);
    }
}

/* ---------------------------------------------------------------------------------------------------- */
static void
luks_lock_cb (GduDevice *device,
              GError    *error,
              gpointer   user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  if (error != NULL)
    {
      /* We could handle PolicyKit integration here but this action is allowed by default
       * and this won't be needed when porting to PolicyKit 1.0 anyway
       */
      g_simple_async_result_set_from_error (simple, error);
      g_error_free (error);
    }

  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
unmount_cb (GduDevice *device,
            GError    *error,
            gpointer   user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  if (error != NULL)
    {
      /* We could handle PolicyKit integration here but this action is allowed by default
       * and this won't be needed when porting to PolicyKit 1.0 anyway
       */
      g_simple_async_result_set_from_error (simple, error);
      g_error_free (error);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      goto out;
    }

  /* if volume is a cleartext LUKS block device, then also lock this one */
  if (gdu_device_is_luks_cleartext (device))
    {
      const gchar *luks_cleartext_slave_object_path;
      GduDevice *luks_cleartext_slave;
      GduPool *pool;

      luks_cleartext_slave_object_path = gdu_device_luks_cleartext_get_slave (device);
      if (luks_cleartext_slave_object_path == NULL)
        {
          g_simple_async_result_set_error (simple,
                                           G_IO_ERROR,
                                           G_IO_ERROR_FAILED,
                                           "Cannot get LUKS cleartext slave");
          g_simple_async_result_complete (simple);
          g_object_unref (simple);
          goto out;
        }

      pool = gdu_device_get_pool (device);
      luks_cleartext_slave = gdu_pool_get_by_object_path (pool, luks_cleartext_slave_object_path);
      g_object_unref (pool);

      if (luks_cleartext_slave == NULL)
        {
          g_simple_async_result_set_error (simple,
                                           G_IO_ERROR,
                                           G_IO_ERROR_FAILED,
                                           "Cannot get LUKS cleartext slave");
          g_simple_async_result_complete (simple);
          g_object_unref (simple);
          goto out;
        }

      gdu_device_op_luks_lock (luks_cleartext_slave,
                               luks_lock_cb,
                               simple);

      g_object_unref (luks_cleartext_slave);
      goto out;
    }

  g_simple_async_result_complete (simple);
  g_object_unref (simple);

 out:
  ;
}

static void
g_gdu_mount_unmount (GMount              *_mount,
                     GMountUnmountFlags   flags,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  GGduMount *mount = G_GDU_MOUNT (_mount);
  GSimpleAsyncResult *simple;
  GduPresentable *gdu_volume;

  gdu_volume = NULL;
  if (mount->volume != NULL)
    gdu_volume = g_gdu_volume_get_presentable_with_cleartext (mount->volume);

  if (mount->volume == NULL || gdu_volume == NULL)
    {
      gchar *argv[] = {"umount", NULL, NULL};

      /* TODO: honor flags */

      if (mount->mount_path != NULL)
        argv[1] = mount->mount_path;
      else
        argv[1] = mount->device_file;

      eject_unmount_do (_mount, cancellable, callback, user_data, argv);
    }
  else if (gdu_volume != NULL)
    {
      simple = g_simple_async_result_new (G_OBJECT (mount),
                                          callback,
                                          user_data,
                                          NULL);

      if (mount->is_burn_mount)
        {
          /* burn mounts are really never mounted... */
          g_simple_async_result_complete (simple);
          g_object_unref (simple);
        }
      else
        {
          GduDevice *device;

          /* TODO: honor flags */

          device = gdu_presentable_get_device (gdu_volume);
          gdu_device_op_filesystem_unmount (device, unmount_cb, simple);
          g_object_unref (device);
        }
    }
  else
    {
      simple = g_simple_async_result_new_error (G_OBJECT (mount),
                                                callback,
                                                user_data,
                                                G_IO_ERROR,
                                                G_IO_ERROR_FAILED,
                                                _("Operation not supported by backend"));
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
    }
}

static gboolean
g_gdu_mount_unmount_finish (GMount       *mount,
                            GAsyncResult  *result,
                            GError       **error)
{
  return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
}

typedef struct {
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
} EjectWrapperOp;

static void
eject_wrapper_callback (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data)
{
  EjectWrapperOp *data  = user_data;
  data->callback (data->object, res, data->user_data);
  g_object_unref (data->object);
  g_free (data);
}

static void
g_gdu_mount_eject (GMount              *mount,
                   GMountUnmountFlags   flags,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  GGduMount *gdu_mount = G_GDU_MOUNT (mount);
  GDrive *drive;

  drive = NULL;
  if (gdu_mount->volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (gdu_mount->volume));

  if (drive != NULL)
    {
      EjectWrapperOp *data;
      data = g_new0 (EjectWrapperOp, 1);
      data->object = g_object_ref (mount);
      data->callback = callback;
      data->user_data = user_data;
      g_drive_eject (drive, flags, cancellable, eject_wrapper_callback, data);
      g_object_unref (drive);
    }
  else
    {
      GSimpleAsyncResult *simple;
      simple = g_simple_async_result_new_error (G_OBJECT (mount),
                                                callback,
                                                user_data,
                                                G_IO_ERROR,
                                                G_IO_ERROR_FAILED,
                                                _("Operation not supported by backend"));
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
    }

}

static gboolean
g_gdu_mount_eject_finish (GMount        *_mount,
                          GAsyncResult  *result,
                          GError       **error)
{
  GGduMount *mount = G_GDU_MOUNT (_mount);
  GDrive *drive;
  gboolean res;

  res = TRUE;

  drive = NULL;
  if (mount->volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (mount->volume));

  if (drive != NULL)
    {
      res = g_drive_eject_finish (drive, result, error);
      g_object_unref (drive);
    }
  else
    {
      g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
      res = FALSE;
    }

  return res;
}

/* TODO: handle force_rescan */
static gchar **
g_gdu_mount_guess_content_type_sync (GMount              *_mount,
                                     gboolean             force_rescan,
                                     GCancellable        *cancellable,
                                     GError             **error)
{
  GGduMount *mount = G_GDU_MOUNT (_mount);
  const gchar *disc_type;
  char **x_content_types;
  GPtrArray *p;
  gchar **result;
  GduDevice *device;
  guint n;

  p = g_ptr_array_new ();

  device = NULL;
  if (mount->volume != NULL)
    {
      GduPresentable *presentable;
      presentable = g_gdu_volume_get_presentable_with_cleartext (mount->volume);
      if (presentable != NULL)
        device = gdu_presentable_get_device (presentable);
    }

  /* doesn't make sense to probe blank discs - look at the disc type instead */
  if (device != NULL && gdu_device_optical_disc_get_is_blank (device))
    {
      disc_type = gdu_device_drive_get_media (device);
      if (disc_type != NULL)
        {
          if (g_str_has_prefix (disc_type, "optical_dvd"))
            g_ptr_array_add (p, g_strdup ("x-content/blank-dvd"));
          else if (g_str_has_prefix (disc_type, "optical_hddvd"))
            g_ptr_array_add (p, g_strdup ("x-content/blank-hddvd"));
          else if (g_str_has_prefix (disc_type, "optical_bd"))
            g_ptr_array_add (p, g_strdup ("x-content/blank-bd"));
          else
            g_ptr_array_add (p, g_strdup ("x-content/blank-cd")); /* assume CD */
        }
    }
  else
    {
      /* sniff content type */
      x_content_types = g_content_type_guess_for_tree (mount->root);
      if (x_content_types != NULL)
        {
          for (n = 0; x_content_types[n] != NULL; n++)
            g_ptr_array_add (p, g_strdup (x_content_types[n]));
          g_strfreev (x_content_types);
        }
    }

  if (p->len == 0)
    {
      result = NULL;
      g_ptr_array_free (p, TRUE);
    }
  else
    {
      g_ptr_array_add (p, NULL);
      result = (char **) g_ptr_array_free (p, FALSE);
    }

  if (device != NULL)
    g_object_unref (device);

  return result;
}

/* since we're an out-of-process volume monitor we'll just do this sync */
static void
g_gdu_mount_guess_content_type (GMount              *mount,
                                gboolean             force_rescan,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  GSimpleAsyncResult *simple;

  /* TODO: handle force_rescan */
  simple = g_simple_async_result_new (G_OBJECT (mount),
                                      callback,
                                      user_data,
                                      NULL);
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static gchar **
g_gdu_mount_guess_content_type_finish (GMount              *mount,
                                       GAsyncResult        *result,
                                       GError             **error)
{
  return g_gdu_mount_guess_content_type_sync (mount, FALSE, NULL, error);
}

static void
g_gdu_mount_mount_iface_init (GMountIface *iface)
{
  iface->get_root = g_gdu_mount_get_root;
  iface->get_name = g_gdu_mount_get_name;
  iface->get_icon = g_gdu_mount_get_icon;
  iface->get_uuid = g_gdu_mount_get_uuid;
  iface->get_drive = g_gdu_mount_get_drive;
  iface->get_volume = g_gdu_mount_get_volume;
  iface->can_unmount = g_gdu_mount_can_unmount;
  iface->can_eject = g_gdu_mount_can_eject;
  iface->unmount = g_gdu_mount_unmount;
  iface->unmount_finish = g_gdu_mount_unmount_finish;
  iface->eject = g_gdu_mount_eject;
  iface->eject_finish = g_gdu_mount_eject_finish;
  iface->guess_content_type = g_gdu_mount_guess_content_type;
  iface->guess_content_type_finish = g_gdu_mount_guess_content_type_finish;
  iface->guess_content_type_sync = g_gdu_mount_guess_content_type_sync;
}

gboolean
g_gdu_mount_has_volume (GGduMount         *mount,
                        GGduVolume        *volume)
{
  return mount->volume == volume;
}
