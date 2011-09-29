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
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include <gvfsmountinfo.h>

#include <gudev/gudev.h>

#include "gvfsudisks2volumemonitor.h"
#include "gvfsudisks2mount.h"
#include "gvfsudisks2volume.h"
#include "gvfsudisks2drive.h"
#include "gvfsudisks2utils.h"

#define BUSY_UNMOUNT_NUM_ATTEMPTS              5
#define BUSY_UNMOUNT_MS_DELAY_BETWEEN_ATTEMPTS 100

typedef struct _GVfsUDisks2MountClass GVfsUDisks2MountClass;
struct _GVfsUDisks2MountClass
{
  GObjectClass parent_class;
};

struct _GVfsUDisks2Mount
{
  GObject parent;

  GVfsUDisks2VolumeMonitor *monitor; /* owned by volume monitor */
  GVfsUDisks2Volume        *volume;  /* owned by volume monitor */

  /* the following members are set in update_mount() */
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

  gchar *bdmv_volume_info_name;
  GIcon *bdmv_volume_info_icon;
  gboolean searched_for_bdmv_volume_info;
};

static gboolean update_mount (GVfsUDisks2Mount *mount);

static void gvfs_udisks2_mount_mount_iface_init (GMountIface *iface);

G_DEFINE_TYPE_EXTENDED (GVfsUDisks2Mount, gvfs_udisks2_mount, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT,
                                               gvfs_udisks2_mount_mount_iface_init))

static void on_volume_changed (GVolume *volume, gpointer user_data);

static void
gvfs_udisks2_mount_finalize (GObject *object)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (object);

  if (mount->volume != NULL)
    {
      g_signal_handlers_disconnect_by_func (mount->volume, on_volume_changed, mount);
      gvfs_udisks2_volume_unset_mount (mount->volume, mount);
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

  G_OBJECT_CLASS (gvfs_udisks2_mount_parent_class)->finalize (object);
}

static void
gvfs_udisks2_mount_class_init (GVfsUDisks2MountClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = gvfs_udisks2_mount_finalize;
}

static void
gvfs_udisks2_mount_init (GVfsUDisks2Mount *mount)
{
}

static void
emit_changed (GVfsUDisks2Mount *mount)
{
  g_signal_emit_by_name (mount, "changed");
  g_signal_emit_by_name (mount->monitor, "mount-changed", mount);
}

static void
got_autorun_info_cb (GObject      *source_object,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (user_data);
  mount->autorun_icon = g_vfs_mount_info_query_autorun_info_finish (G_FILE (source_object), res, NULL);
  if (update_mount (mount))
    emit_changed (mount);
  g_object_unref (mount);
}

static void
got_xdg_volume_info_cb (GObject      *source_object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (user_data);
  mount->xdg_volume_info_icon = g_vfs_mount_info_query_xdg_volume_info_finish (G_FILE (source_object),
                                                                               res,
                                                                               &(mount->xdg_volume_info_name),
                                                                               NULL);
  if (update_mount (mount))
    emit_changed (mount);
  g_object_unref (mount);
}

static void
got_bdmv_volume_info_cb (GObject      *source_object,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (user_data);
  mount->bdmv_volume_info_icon = g_vfs_mount_info_query_bdmv_volume_info_finish (G_FILE (source_object),
                                                                                 res,
                                                                                 &(mount->bdmv_volume_info_name),
                                                                                 NULL);
  if (update_mount (mount))
    emit_changed (mount);
  g_object_unref (mount);
}

static gboolean
update_mount (GVfsUDisks2Mount *mount)
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

      /* icon order of preference: bdmv, xdg, autorun, probed */
      if (mount->bdmv_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->bdmv_volume_info_icon);
      else if (mount->xdg_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->xdg_volume_info_icon);
      else if (mount->autorun_icon != NULL)
        mount->icon = g_object_ref (mount->autorun_icon);
      else
        mount->icon = g_volume_get_icon (G_VOLUME (mount->volume));

      g_free (mount->name);

      /* name order of preference : bdmv, xdg, probed */
      if (mount->bdmv_volume_info_name != NULL)
        mount->name = g_strdup (mount->bdmv_volume_info_name);
      else if (mount->xdg_volume_info_name != NULL)
        mount->name = g_strdup (mount->xdg_volume_info_name);
      else
        mount->name = g_volume_get_name (G_VOLUME (mount->volume));
    }
  else
    {
      mount->can_unmount = TRUE;

      if (mount->icon != NULL)
        g_object_unref (mount->icon);

      /* icon order of preference: bdmv, xdg, autorun, probed */
      if (mount->bdmv_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->bdmv_volume_info_icon);
      else if (mount->xdg_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->xdg_volume_info_icon);
      else if (mount->autorun_icon != NULL)
        mount->icon = g_object_ref (mount->autorun_icon);
      else
        mount->icon = mount->mount_entry_icon != NULL ? g_object_ref (mount->mount_entry_icon) : NULL;

      g_free (mount->name);

      /* name order of preference: bdmv, xdg, probed */
      if (mount->bdmv_volume_info_name != NULL)
        mount->name = g_strdup (mount->bdmv_volume_info_name);
      else if (mount->xdg_volume_info_name != NULL)
        mount->name = g_strdup (mount->xdg_volume_info_name);
      else
        mount->name = g_strdup (mount->mount_entry_name);
    }

  /* compute whether something changed */
  changed = !((old_can_unmount == mount->can_unmount) &&
              (g_strcmp0 (old_name, mount->name) == 0) &&
              g_icon_equal (old_icon, mount->icon));

  /* free old values */
  g_free (old_name);
  if (old_icon != NULL)
    g_object_unref (old_icon);

  /*g_debug ("in update_mount(), changed=%d", changed);*/

  /* search for BDMV */
  if (!mount->searched_for_bdmv_volume_info)
    {
      mount->searched_for_bdmv_volume_info = TRUE;
      g_vfs_mount_info_query_bdmv_volume_info (mount->root,
      					       NULL,
      					       got_bdmv_volume_info_cb,
      					       g_object_ref (mount));
    }

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
on_volume_changed (GVolume  *volume,
                   gpointer  user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (user_data);
  if (update_mount (mount))
    emit_changed (mount);
}

GVfsUDisks2Mount *
gvfs_udisks2_mount_new (GVfsUDisks2VolumeMonitor *monitor,
                        GUnixMountEntry          *mount_entry,
                        GVfsUDisks2Volume        *volume)
{
  GVfsUDisks2Mount *mount = NULL;

  /* Ignore internal mounts unless there's a volume */
  if (volume == NULL && (mount_entry != NULL && !g_unix_mount_guess_should_display (mount_entry)))
    goto out;

  mount = g_object_new (GVFS_TYPE_UDISKS2_MOUNT, NULL);
  mount->monitor = monitor;

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
      gvfs_udisks2_volume_set_mount (volume, mount);
      /* this is for piggy backing on the name and icon of the associated volume */
      g_signal_connect (mount->volume, "changed", G_CALLBACK (on_volume_changed), mount);
    }

  update_mount (mount);

 out:

  return mount;
}

void
gvfs_udisks2_mount_unmounted (GVfsUDisks2Mount *mount)
{
  if (mount->volume != NULL)
    {
      gvfs_udisks2_volume_unset_mount (mount->volume, mount);
      g_signal_handlers_disconnect_by_func (mount->volume, on_volume_changed, mount);
      mount->volume = NULL;
      emit_changed (mount);
    }
}

void
gvfs_udisks2_mount_unset_volume (GVfsUDisks2Mount   *mount,
                                 GVfsUDisks2Volume  *volume)
{
  if (mount->volume == volume)
    {
      g_signal_handlers_disconnect_by_func (mount->volume, on_volume_changed, mount);
      mount->volume = NULL;
      emit_changed (mount);
    }
}

static GFile *
gvfs_udisks2_mount_get_root (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->root != NULL ? g_object_ref (mount->root) : NULL;
}

static GIcon *
gvfs_udisks2_mount_get_icon (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->icon != NULL ? g_object_ref (mount->icon) : NULL;
}

static gchar *
gvfs_udisks2_mount_get_uuid (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return g_strdup (mount->uuid);
}

static gchar *
gvfs_udisks2_mount_get_name (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return g_strdup (mount->name);
}

gboolean
gvfs_udisks2_mount_has_uuid (GVfsUDisks2Mount *_mount,
                             const gchar      *uuid)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return g_strcmp0 (mount->uuid, uuid) == 0;
}

const gchar *
gvfs_udisks2_mount_get_mount_path (GVfsUDisks2Mount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->mount_path;
}

static GDrive *
gvfs_udisks2_mount_get_drive (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  GDrive *drive = NULL;

  if (mount->volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (mount->volume));
  return drive;
}

static GVolume *
gvfs_udisks2_mount_get_volume (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  GVolume *volume = NULL;

  if (mount->volume)
    volume = G_VOLUME (g_object_ref (mount->volume));
  return volume;
}

static gboolean
gvfs_udisks2_mount_can_unmount (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->can_unmount;
}

static gboolean
gvfs_udisks2_mount_can_eject (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
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

static GArray *
get_busy_processes (const gchar* const *mount_points)
{
  GArray *processes;
  guint n;

  processes = g_array_new (FALSE, FALSE, sizeof (GPid));

  for (n = 0; mount_points != NULL && mount_points[n] != NULL; n++)
    {
      const gchar *mount_point = mount_points[n];
      const gchar *lsof_argv[] = {"lsof", "-t", NULL, NULL};
      gchar *standard_output = NULL;
      const gchar *p;
      gint exit_status;
      GError *error;

      lsof_argv[2] = mount_point;

      error = NULL;
      if (!g_spawn_sync (NULL,       /* working_directory */
                         (gchar **) lsof_argv,
                         NULL,       /* envp */
                         G_SPAWN_SEARCH_PATH,
                         NULL,       /* child_setup */
                         NULL,       /* user_data */
                         &standard_output,
                         NULL,       /* standard_error */
                         &exit_status,
                         &error))
        {
          g_warning ("Error launching lsof(1): %s (%s, %d)",
                     error->message, g_quark_to_string (error->domain), error->code);
          goto cont_loop;
        }
      if (!(WIFEXITED (exit_status) && WEXITSTATUS (exit_status) == 0))
        {
          g_warning ("lsof(1) didn't exit normally");
          goto cont_loop;
        }

      p = standard_output;
      while (TRUE)
        {
          GPid pid;
          gchar *endp;

          if (*p == '\0')
            break;

          pid = strtol (p, &endp, 10);

          if (pid == 0 && p == endp)
            break;

          g_array_append_val (processes, pid);

          p = endp;
        }
    cont_loop:
      g_free (standard_output);
    }
  return processes;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  GSimpleAsyncResult *simple;

  GVfsUDisks2Mount *mount;

  GCancellable *cancellable;
  gulong cancelled_handler_id;
  gboolean is_cancelled;

  GMountOperation *mount_operation;
  GMountUnmountFlags flags;

  gulong mount_op_reply_handler_id;
  guint retry_unmount_timer_id;

} UnmountData;

static void
unmount_data_free (UnmountData *data)
{
  g_object_unref (data->simple);

  if (data->mount_op_reply_handler_id > 0)
    {
      /* make the operation dialog go away */
      g_signal_emit_by_name (data->mount_operation, "aborted");
      g_signal_handler_disconnect (data->mount_operation, data->mount_op_reply_handler_id);
    }
  if (data->retry_unmount_timer_id > 0)
    {
      g_source_remove (data->retry_unmount_timer_id);
      data->retry_unmount_timer_id = 0;
    }

  g_clear_object (&data->mount);
  if (data->cancelled_handler_id != 0)
    g_signal_handler_disconnect (data->cancellable, data->cancelled_handler_id);
  g_clear_object (&data->cancellable);
  g_clear_object (&data->mount_operation);

  g_free (data);
}

static void unmount_do (UnmountData *data, gboolean force);

static gboolean
on_retry_timer_cb (gpointer user_data)
{
  UnmountData *data = user_data;

  if (data->retry_unmount_timer_id == 0)
    goto out;

  /* we're removing the timeout */
  data->retry_unmount_timer_id = 0;

  /* timeout expired => try again */
  unmount_do (data, FALSE);

 out:
  return FALSE; /* remove timeout */
}

static void
on_mount_op_reply (GMountOperation       *mount_operation,
                   GMountOperationResult result,
                   gpointer              user_data)
{
  UnmountData *data = user_data;
  gint choice;

  /* disconnect the signal handler */
  g_warn_if_fail (data->mount_op_reply_handler_id != 0);
  g_signal_handler_disconnect (data->mount_operation,
                               data->mount_op_reply_handler_id);
  data->mount_op_reply_handler_id = 0;

  choice = g_mount_operation_get_choice (mount_operation);

  if (result == G_MOUNT_OPERATION_ABORTED ||
      (result == G_MOUNT_OPERATION_HANDLED && choice == 1))
    {
      /* don't show an error dialog here */
      g_simple_async_result_set_error (data->simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_FAILED_HANDLED,
                                       "GMountOperation aborted (user should never see this "
                                       "error since it is G_IO_ERROR_FAILED_HANDLED)");
      g_simple_async_result_complete_in_idle (data->simple);
      unmount_data_free (data);
    }
  else if (result == G_MOUNT_OPERATION_HANDLED)
    {
      /* user chose force unmount => try again with force_unmount==TRUE */
      unmount_do (data, TRUE);
    }
  else
    {
      /* result == G_MOUNT_OPERATION_UNHANDLED => GMountOperation instance doesn't
       * support :show-processes signal
       */
      g_simple_async_result_set_error (data->simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_BUSY,
                                       _("One or more programs are preventing the unmount operation."));
      g_simple_async_result_complete_in_idle (data->simple);
      unmount_data_free (data);
    }
}

static void
unmount_cb (GObject       *source_object,
            GAsyncResult  *res,
            gpointer       user_data)
{
  UDisksFilesystem *filesystem = UDISKS_FILESYSTEM (source_object);
  UnmountData *data = user_data;
  GError *error;

  error = NULL;
  if (!udisks_filesystem_call_unmount_finish (filesystem,
                                              res,
                                              &error))
    {
      gvfs_udisks2_utils_udisks_error_to_gio_error (error);

      /* if the user passed in a GMountOperation, then do the GMountOperation::show-processes dance ... */
      if (error->code == G_IO_ERROR_BUSY && data->mount_operation != NULL)
        {
          GArray *processes;
          const gchar *choices[3] = {NULL, NULL, NULL};
          const gchar *message;

          /* TODO: get processes */
          processes = get_busy_processes (udisks_filesystem_get_mount_points (filesystem));

          if (data->mount_op_reply_handler_id == 0)
            {
              data->mount_op_reply_handler_id = g_signal_connect (data->mount_operation,
                                                                  "reply",
                                                                  G_CALLBACK (on_mount_op_reply),
                                                                  data);
            }
          choices[0] = _("Unmount Anyway");
          choices[1] = _("Cancel");
          message = _("Volume is busy\n"
                      "One or more applications are keeping the volume busy.");
          g_signal_emit_by_name (data->mount_operation,
                                 "show-processes",
                                 message,
                                 processes,
                                 choices);
          /* set up a timer to try unmounting every two seconds - this will also
           * update the list of busy processes
           */
          if (data->retry_unmount_timer_id == 0)
            {
              data->retry_unmount_timer_id = g_timeout_add_seconds (2,
                                                                    on_retry_timer_cb,
                                                                    data);
            }
          goto out;
        }
      else
        {
          g_simple_async_result_take_error (data->simple, error);
          g_simple_async_result_complete (data->simple);
        }
    }
  else
    {
      gvfs_udisks2_volume_monitor_update (data->mount->monitor);
      g_simple_async_result_complete (data->simple);
    }

  unmount_data_free (data);
 out:
  ;
}

static void
unmount_do (UnmountData *data,
            gboolean     force)
{
  UDisksBlock *block;
  UDisksFilesystem *filesystem;
  GVariantBuilder builder;

  if (data->mount->volume != NULL)
    {
      block = gvfs_udisks2_volume_get_block (data->mount->volume);
    }
  else
    {
      g_simple_async_result_set_error (data->simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_FAILED,
                                       "Don't know how to unmount non-udisks object yet (TODO)");
      g_simple_async_result_complete (data->simple);
      unmount_data_free (data);
      goto out;
    }

  filesystem = udisks_object_peek_filesystem (UDISKS_OBJECT (g_dbus_interface_get_object (G_DBUS_INTERFACE (block))));
  if (filesystem == NULL)
    {
      g_simple_async_result_set_error (data->simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_FAILED,
                                       "No filesystem interface on D-Bus object");
      g_simple_async_result_complete (data->simple);
      unmount_data_free (data);
      goto out;
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (data->mount_operation == NULL)
    {
      g_variant_builder_add (&builder,
                             "{sv}",
                             "auth.no_user_interaction", g_variant_new_boolean (TRUE));
    }
  if (force || data->flags & G_MOUNT_UNMOUNT_FORCE)
    {
      g_variant_builder_add (&builder,
                             "{sv}",
                             "force", g_variant_new_boolean (TRUE));
    }
  udisks_filesystem_call_unmount (filesystem,
                                  g_variant_builder_end (&builder),
                                  data->cancellable,
                                  unmount_cb,
                                  data);

 out:
  ;
}

static void
gvfs_udisks2_mount_unmount_with_operation (GMount              *_mount,
                                           GMountUnmountFlags   flags,
                                           GMountOperation     *mount_operation,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  UnmountData *data;

  /* first emit the ::mount-pre-unmount signal */
  g_signal_emit_by_name (mount->monitor, "mount-pre-unmount", mount);

  data = g_new0 (UnmountData, 1);
  data->simple = g_simple_async_result_new (G_OBJECT (mount),
                                            callback,
                                            user_data,
                                            gvfs_udisks2_mount_unmount_with_operation);
  data->mount = g_object_ref (mount);
  data->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;
  data->mount_operation = mount_operation != NULL ? g_object_ref (mount_operation) : NULL;
  data->flags = flags;

  if (mount->is_burn_mount)
    {
      /* burn mounts are really never mounted so complete successfully immediately */
      g_simple_async_result_complete_in_idle (data->simple);
      unmount_data_free (data);
    }
  else
    {
      unmount_do (data, FALSE);
    }
}

static gboolean
gvfs_udisks2_mount_unmount_with_operation_finish (GMount        *mount,
                                                  GAsyncResult  *result,
                                                  GError       **error)
{
  return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
gvfs_udisks2_mount_unmount (GMount              *mount,
                            GMountUnmountFlags   flags,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  gvfs_udisks2_mount_unmount_with_operation (mount, flags, NULL, cancellable, callback, user_data);
}

static gboolean
gvfs_udisks2_mount_unmount_finish (GMount        *mount,
                                   GAsyncResult  *result,
                                   GError       **error)
{
  return gvfs_udisks2_mount_unmount_with_operation_finish (mount, result, error);
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
} EjectWrapperOp;

static void
eject_wrapper_callback (GObject       *source_object,
                        GAsyncResult  *res,
                        gpointer       user_data)
{
  EjectWrapperOp *data  = user_data;
  data->callback (data->object, res, data->user_data);
  g_object_unref (data->object);
  g_free (data);
}

static void
gvfs_udisks2_mount_eject_with_operation (GMount              *_mount,
                                         GMountUnmountFlags   flags,
                                         GMountOperation     *mount_operation,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  GDrive *drive;

  drive = NULL;
  if (mount->volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (mount->volume));

  if (drive != NULL)
    {
      EjectWrapperOp *data;
      data = g_new0 (EjectWrapperOp, 1);
      data->object = g_object_ref (mount);
      data->callback = callback;
      data->user_data = user_data;
      g_drive_eject_with_operation (drive, flags, mount_operation, cancellable, eject_wrapper_callback, data);
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
gvfs_udisks2_mount_eject_with_operation_finish (GMount        *_mount,
                                                GAsyncResult  *result,
                                                GError       **error)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  gboolean ret = TRUE;
  GDrive *drive;

  drive = NULL;
  if (mount->volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (mount->volume));

  if (drive != NULL)
    {
      ret = g_drive_eject_with_operation_finish (drive, result, error);
      g_object_unref (drive);
    }
  else
    {
      g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
      ret = FALSE;
    }
  return ret;
}

static void
gvfs_udisks2_mount_eject (GMount              *mount,
                          GMountUnmountFlags   flags,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
  gvfs_udisks2_mount_eject_with_operation (mount, flags, NULL, cancellable, callback, user_data);
}

static gboolean
gvfs_udisks2_mount_eject_finish (GMount        *mount,
                                 GAsyncResult  *result,
                                 GError       **error)
{
  return gvfs_udisks2_mount_eject_with_operation_finish (mount, result, error);
}

/* ---------------------------------------------------------------------------------------------------- */

/* TODO: handle force_rescan */
static gchar **
gvfs_udisks2_mount_guess_content_type_sync (GMount        *_mount,
                                            gboolean       force_rescan,
                                            GCancellable  *cancellable,
                                            GError       **error)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  gchar **x_content_types;
  GPtrArray *p;
  gchar **ret;
  guint n;

  p = g_ptr_array_new ();

  /* doesn't make sense to probe blank discs - look at the disc type instead */
  if (mount->is_burn_mount)
    {
      GDrive *drive;
      drive = gvfs_udisks2_mount_get_drive (_mount);
      if (drive != NULL)
        {
          UDisksDrive *udisks_drive = gvfs_udisks2_drive_get_udisks_drive (GVFS_UDISKS2_DRIVE (drive));;
          const gchar *media = udisks_drive_get_media (udisks_drive);
          if (media != NULL)
            {
              if (g_str_has_prefix (media, "optical_dvd"))
                g_ptr_array_add (p, g_strdup ("x-content/blank-dvd"));
              else if (g_str_has_prefix (media, "optical_hddvd"))
                g_ptr_array_add (p, g_strdup ("x-content/blank-hddvd"));
              else if (g_str_has_prefix (media, "optical_bd"))
                g_ptr_array_add (p, g_strdup ("x-content/blank-bd"));
              else
                g_ptr_array_add (p, g_strdup ("x-content/blank-cd")); /* assume CD */
            }
          g_object_unref (drive);
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

  /* Check if its bootable */
  if (mount->device_file != NULL)
    {
      GUdevDevice *gudev_device;
      gudev_device = g_udev_client_query_by_device_file (gvfs_udisks2_volume_monitor_get_gudev_client (mount->monitor),
                                                         mount->device_file);
      if (gudev_device != NULL)
        {
          if (g_udev_device_get_property_as_boolean (gudev_device, "OSINFO_BOOTABLE"))
            g_ptr_array_add (p, g_strdup ("x-content/bootable-media"));
          g_object_unref (gudev_device);
        }
    }

  if (p->len == 0)
    {
      ret = NULL;
      g_ptr_array_free (p, TRUE);
    }
  else
    {
      g_ptr_array_add (p, NULL);
      ret = (char **) g_ptr_array_free (p, FALSE);
    }
  return ret;
}

/* since we're an out-of-process volume monitor we'll just do this sync */
static void
gvfs_udisks2_mount_guess_content_type (GMount              *mount,
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
gvfs_udisks2_mount_guess_content_type_finish (GMount        *mount,
                                              GAsyncResult  *result,
                                              GError       **error)
{
  return gvfs_udisks2_mount_guess_content_type_sync (mount, FALSE, NULL, error);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
gvfs_udisks2_mount_mount_iface_init (GMountIface *iface)
{
  iface->get_root = gvfs_udisks2_mount_get_root;
  iface->get_name = gvfs_udisks2_mount_get_name;
  iface->get_icon = gvfs_udisks2_mount_get_icon;
  iface->get_uuid = gvfs_udisks2_mount_get_uuid;
  iface->get_drive = gvfs_udisks2_mount_get_drive;
  iface->get_volume = gvfs_udisks2_mount_get_volume;
  iface->can_unmount = gvfs_udisks2_mount_can_unmount;
  iface->can_eject = gvfs_udisks2_mount_can_eject;
  iface->unmount = gvfs_udisks2_mount_unmount;
  iface->unmount_finish = gvfs_udisks2_mount_unmount_finish;
  iface->unmount_with_operation = gvfs_udisks2_mount_unmount_with_operation;
  iface->unmount_with_operation_finish = gvfs_udisks2_mount_unmount_with_operation_finish;
  iface->eject = gvfs_udisks2_mount_eject;
  iface->eject_finish = gvfs_udisks2_mount_eject_finish;
  iface->eject_with_operation = gvfs_udisks2_mount_eject_with_operation;
  iface->eject_with_operation_finish = gvfs_udisks2_mount_eject_with_operation_finish;
  iface->guess_content_type = gvfs_udisks2_mount_guess_content_type;
  iface->guess_content_type_finish = gvfs_udisks2_mount_guess_content_type_finish;
  iface->guess_content_type_sync = gvfs_udisks2_mount_guess_content_type_sync;
}

gboolean
gvfs_udisks2_mount_has_volume (GVfsUDisks2Mount   *mount,
                               GVfsUDisks2Volume  *volume)
{
  return mount->volume == volume;
}
