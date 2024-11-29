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

#include <gvfsmountinfo.h>

#include <gudev/gudev.h>

#include "gvfsudisks2volumemonitor.h"
#include "gvfsudisks2mount.h"
#include "gvfsudisks2volume.h"
#include "gvfsudisks2drive.h"
#include "gvfsudisks2utils.h"

typedef struct _GVfsUDisks2MountClass GVfsUDisks2MountClass;
struct _GVfsUDisks2MountClass
{
  GObjectClass parent_class;
};

struct _GVfsUDisks2Mount
{
  GObject parent;

  GVfsUDisks2VolumeMonitor *monitor; /* owned by volume monitor */

  /* may be NULL */
  GVfsUDisks2Volume        *volume;  /* owned by volume monitor */

  /* may be NULL */
  GUnixMountEntry *mount_entry;

  /* the following members are set in update_mount() */
  GFile *root;
  GIcon *icon;
  GIcon *symbolic_icon;
  gchar *name;
  gchar *sort_key;
  gchar *uuid;
  gchar *device_file;
  gchar *mount_path;
  gboolean can_unmount;
  gchar *mount_entry_name;
  gchar *mount_entry_fs_type;

#ifdef HAVE_BURN
  gboolean is_burn_mount;
#endif

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

  g_clear_object (&mount->root);
  g_clear_object (&mount->icon);
  g_clear_object (&mount->symbolic_icon);
  g_free (mount->name);
  g_free (mount->sort_key);
  g_free (mount->uuid);
  g_free (mount->device_file);
  g_free (mount->mount_path);

  g_free (mount->mount_entry_name);
  g_free (mount->mount_entry_fs_type);

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
  GIcon *old_symbolic_icon;

  /* save old values */
  old_can_unmount = mount->can_unmount;
  old_name = g_strdup (mount->name);
  old_icon = mount->icon != NULL ? g_object_ref (mount->icon) : NULL;
  old_symbolic_icon = mount->symbolic_icon != NULL ? g_object_ref (mount->symbolic_icon) : NULL;

  /* reset */
  mount->can_unmount = FALSE;
  g_clear_object (&mount->icon);
  g_clear_object (&mount->symbolic_icon);
  g_free (mount->name); mount->name = NULL;

  /* in with the new */
  if (mount->volume != NULL)
    {
      mount->can_unmount = TRUE;

      /* icon order of preference: bdmv, xdg, autorun, probed */
      if (mount->bdmv_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->bdmv_volume_info_icon);
      else if (mount->xdg_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->xdg_volume_info_icon);
      else if (mount->autorun_icon != NULL)
        mount->icon = g_object_ref (mount->autorun_icon);
      else
        mount->icon = g_volume_get_icon (G_VOLUME (mount->volume));

      /* name order of preference : bdmv, xdg, probed */
      if (mount->bdmv_volume_info_name != NULL)
        mount->name = g_strdup (mount->bdmv_volume_info_name);
      else if (mount->xdg_volume_info_name != NULL)
        mount->name = g_strdup (mount->xdg_volume_info_name);
      else
        mount->name = g_volume_get_name (G_VOLUME (mount->volume));

      mount->symbolic_icon = g_volume_get_symbolic_icon (G_VOLUME (mount->volume));
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
        {
          mount->icon = gvfs_udisks2_utils_icon_from_fs_type (mount->mount_entry_fs_type);
        }

      g_free (mount->name);

      /* name order of preference: bdmv, xdg, probed */
      if (mount->bdmv_volume_info_name != NULL)
        mount->name = g_strdup (mount->bdmv_volume_info_name);
      else if (mount->xdg_volume_info_name != NULL)
        mount->name = g_strdup (mount->xdg_volume_info_name);
      else
        mount->name = g_strdup (mount->mount_entry_name);

      mount->symbolic_icon = gvfs_udisks2_utils_symbolic_icon_from_fs_type (mount->mount_entry_fs_type);
    }

  /* compute whether something changed */
  changed = !((old_can_unmount == mount->can_unmount) &&
              (g_strcmp0 (old_name, mount->name) == 0) &&
              g_icon_equal (old_icon, mount->icon) &&
              g_icon_equal (old_symbolic_icon, mount->symbolic_icon));

  /* free old values */
  g_free (old_name);
  g_clear_object (&old_icon);
  g_clear_object (&old_symbolic_icon);

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
                        GUnixMountEntry          *mount_entry, /* takes ownership */
                        GVfsUDisks2Volume        *volume)
{
  GVfsUDisks2Mount *mount = NULL;

  mount = g_object_new (GVFS_TYPE_UDISKS2_MOUNT, NULL);
  mount->monitor = monitor;
  mount->sort_key = g_strdup_printf ("gvfs.time_detected_usec.%" G_GINT64_FORMAT, g_get_real_time ());

  if (mount_entry != NULL)
    {
      mount->mount_entry = mount_entry; /* takes ownership */
      mount->mount_entry_name = g_unix_mount_entry_guess_name (mount_entry);
      mount->mount_entry_fs_type = g_strdup (g_unix_mount_entry_get_fs_type (mount_entry));
      mount->device_file = g_strdup (g_unix_mount_entry_get_device_path (mount_entry));
      mount->mount_path = g_strdup (g_unix_mount_entry_get_mount_path (mount_entry));
      mount->root = g_file_new_for_path (mount->mount_path);
    }
#ifdef HAVE_BURN
  else
    {
      /* burn:/// mount (the only mounts we support with mount_entry == NULL) */
      mount->device_file = NULL;
      mount->mount_path = NULL;
      mount->root = g_file_new_for_uri ("burn:///");
      mount->is_burn_mount = TRUE;
    }
#endif

  /* need to set the volume only when the mount is fully constructed */
  mount->volume = volume;
  if (mount->volume != NULL)
    {
      gvfs_udisks2_volume_set_mount (volume, mount);
      /* this is for piggy backing on the name and icon of the associated volume */
      g_signal_connect (mount->volume, "changed", G_CALLBACK (on_volume_changed), mount);
    }

  update_mount (mount);

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

void
gvfs_udisks2_mount_set_volume (GVfsUDisks2Mount   *mount,
                               GVfsUDisks2Volume  *volume)
{
  if (mount->volume != volume)
    {
      if (mount->volume != NULL)
        gvfs_udisks2_mount_unset_volume (mount, mount->volume);
      mount->volume = volume;
      if (mount->volume != NULL)
        {
          gvfs_udisks2_volume_set_mount (volume, mount);
          /* this is for piggy backing on the name and icon of the associated volume */
          g_signal_connect (mount->volume, "changed", G_CALLBACK (on_volume_changed), mount);
        }
      update_mount (mount);
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

static GIcon *
gvfs_udisks2_mount_get_symbolic_icon (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->symbolic_icon != NULL ? g_object_ref (mount->symbolic_icon) : NULL;
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
gvfs_udisks2_mount_get_mount_path (GVfsUDisks2Mount *mount)
{
  return mount->mount_path;
}

GUnixMountEntry *
gvfs_udisks2_mount_get_mount_entry (GVfsUDisks2Mount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->mount_entry;
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
gvfs_udisks2_mount_get_volume_ (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  GVolume *volume = NULL;

  if (mount->volume != NULL)
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

typedef struct
{
  gboolean in_progress;

  UDisksEncrypted *encrypted;
  UDisksFilesystem *filesystem;

  GMountOperation *mount_operation;
  GMountUnmountFlags flags;

  gulong mount_op_reply_handler_id;
  guint retry_unmount_timer_id;

  GMountOperationResult reply_result;
  gint reply_choice;
  gboolean reply_set;
} UnmountData;

static void
unmount_data_free (UnmountData *data)
{
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

  g_clear_object (&data->mount_operation);
  g_clear_object (&data->encrypted);
  g_clear_object (&data->filesystem);
}

static gboolean
unmount_operation_is_eject (GMountOperation *op)
{
  return GPOINTER_TO_INT (g_object_get_data (G_OBJECT (op), "x-udisks2-is-eject"));
}

static gboolean
unmount_operation_is_stop (GMountOperation *op)
{
  return GPOINTER_TO_INT (g_object_get_data (G_OBJECT (op), "x-udisks2-is-stop"));
}

static void
umount_completed (gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  UnmountData *data = g_task_get_task_data (task);

  if (data->mount_operation &&
      !unmount_operation_is_eject (data->mount_operation) &&
      !unmount_operation_is_stop (data->mount_operation))
    gvfs_udisks2_unmount_notify_stop (data->mount_operation, g_task_had_error (task));
}

static void unmount_do (GTask *task, gboolean force);

static gboolean
on_retry_timer_cb (gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  UnmountData *data = g_task_get_task_data (task);

  if (data->retry_unmount_timer_id == 0)
    goto out;

  /* we're removing the timeout */
  data->retry_unmount_timer_id = 0;

  if (g_task_get_completed (task) || data->in_progress)
    goto out;

  /* timeout expired => try again */
  unmount_do (task, FALSE);

 out:
  return FALSE; /* remove timeout */
}

static void
mount_op_reply_handle (GTask *task)
{
  UnmountData *data = g_task_get_task_data (task);
  data->reply_set = FALSE;

  if (data->reply_result == G_MOUNT_OPERATION_ABORTED ||
      (data->reply_result == G_MOUNT_OPERATION_HANDLED &&
       data->reply_choice == 1))
    {
      /* don't show an error dialog here */
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED,
                               "GMountOperation aborted");
      g_object_unref (task);
    }
  else if (data->reply_result == G_MOUNT_OPERATION_HANDLED)
    {
      /* user chose force unmount => try again with force_unmount==TRUE */
      unmount_do (task, TRUE);
    }
  else
    {
      /* result == G_MOUNT_OPERATION_UNHANDLED => GMountOperation instance doesn't
       * support :show-processes signal
       */
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_BUSY,
                               _("One or more programs are preventing the unmount operation."));
      g_object_unref (task);
    }
}

static void
on_mount_op_reply (GMountOperation       *mount_operation,
                   GMountOperationResult result,
                   gpointer              user_data)
{
  GTask *task = G_TASK (user_data);
  UnmountData *data = g_task_get_task_data (task);
  gint choice;

  /* disconnect the signal handler */
  g_warn_if_fail (data->mount_op_reply_handler_id != 0);
  g_signal_handler_disconnect (data->mount_operation,
                               data->mount_op_reply_handler_id);
  data->mount_op_reply_handler_id = 0;

  choice = g_mount_operation_get_choice (mount_operation);
  data->reply_result = result;
  data->reply_choice = choice;
  data->reply_set = TRUE;
  if (!g_task_get_completed (task) && !data->in_progress)
    mount_op_reply_handle (task);
}

static void
lsof_command_cb (GObject       *source_object,
                 GAsyncResult  *res,
                 gpointer       user_data)
{
  GTask *task = G_TASK (user_data);
  UnmountData *data = g_task_get_task_data (task);
  GError *error;
  gint exit_status;
  GArray *processes;
  const gchar *choices[3] = {NULL, NULL, NULL};
  const gchar *message;
  gchar *standard_output = NULL;
  const gchar *p;

  processes = g_array_new (FALSE, FALSE, sizeof (GPid));

  error = NULL;
  if (!gvfs_udisks2_utils_spawn_finish (res,
                                        &exit_status,
                                        &standard_output,
                                        NULL, /* gchar **out_standard_error */
                                        &error))
    {
      g_printerr ("Error launching lsof(1): %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      goto out;
    }

  if (!(WIFEXITED (exit_status) && WEXITSTATUS (exit_status) == 0))
    {
      g_printerr ("lsof(1) did not exit normally\n");
      goto out;
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

 out:
  if (!g_task_get_completed (task))
    {
      gboolean is_eject;
      gboolean is_stop;

      is_eject = unmount_operation_is_eject (data->mount_operation);
      is_stop = unmount_operation_is_stop (data->mount_operation);

      /* We want to emit the 'show-processes' signal even if launching
       * lsof(1) failed or if it didn't return any PIDs. This is because
       * it won't show e.g. root-owned processes operating on files
       * on the mount point.
       *
       * (unfortunately there's no way to convey that it failed)
       */
      if (data->mount_op_reply_handler_id == 0)
        {
          data->mount_op_reply_handler_id = g_signal_connect (data->mount_operation,
                                                              "reply",
                                                              G_CALLBACK (on_mount_op_reply),
                                                              task);
        }
      if (is_eject || is_stop)
        {
          /* Note that the GUI (Shell, Files) currently use the term
           * "Eject" for both GDrive.stop() and GDrive.eject().
           */
          choices[0] = _("Eject Anyway");
        }
      else
        {
          choices[0] = _("Unmount Anyway");
        }
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
                                                                task);
        }
      g_array_free (processes, TRUE);
      g_free (standard_output);
    }
  g_object_unref (task); /* return ref */
}


static void
unmount_show_busy (GTask        *task,
                   const gchar  *mount_point)
{
  UnmountData *data = g_task_get_task_data (task);
  gchar *escaped_mount_point;

  data->in_progress = FALSE;

  /* We received an reply during an unmount operation which could not complete.
   * Handle the reply now. */
  if (data->reply_set)
    {
      mount_op_reply_handle (task);
      return;
    }

  escaped_mount_point = g_strescape (mount_point, NULL);
  gvfs_udisks2_utils_spawn (10, /* timeout in seconds */
                            g_task_get_cancellable (task),
                            lsof_command_cb,
                            g_object_ref (task),
                            "lsof -t \"%s\"",
                            escaped_mount_point);
  g_free (escaped_mount_point);
}

static void
lock_cb (GObject       *source_object,
         GAsyncResult  *res,
         gpointer       user_data)
{
  UDisksEncrypted *encrypted = UDISKS_ENCRYPTED (source_object);
  GTask *task = G_TASK (user_data);
  GError *error;

  error = NULL;
  if (!udisks_encrypted_call_lock_finish (encrypted,
                                          res,
                                          &error))
    g_task_return_error (task, error);
  else
    {
      /* Call the unmount_completed function explicitly here as it is too
       * late to emit show-unmount-progress signal from the notify::completed
       * callback.
       */
      umount_completed (task);

      g_task_return_boolean (task, TRUE);
    }

  g_object_unref (task);
}

static void
unmount_cb (GObject       *source_object,
            GAsyncResult  *res,
            gpointer       user_data)
{
  UDisksFilesystem *filesystem = UDISKS_FILESYSTEM (source_object);
  GTask *task = G_TASK (user_data);
  UnmountData *data = g_task_get_task_data (task);
  GVfsUDisks2Mount *mount = g_task_get_source_object (task);
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
          unmount_show_busy (task, udisks_filesystem_get_mount_points (filesystem)[0]);
          return;
        }

      g_task_return_error (task, error);
      g_object_unref (task);
    }
  else
    {
      gvfs_udisks2_volume_monitor_update (mount->monitor);
      if (data->encrypted != NULL)
        {
          udisks_encrypted_call_lock (data->encrypted,
                                      g_variant_new ("a{sv}", NULL), /* options */
                                      g_task_get_cancellable (task),
                                      lock_cb,
                                      task);
          return;
        }

      /* Call the unmount_completed function explicitly here as it is too
       * late to emit show-unmount-progress signal from the notify::completed
       * callback.
       */
      umount_completed (task);

      g_task_return_boolean (task, TRUE);
      g_object_unref (task);
    }
}


/* ------------------------------ */

static void
umount_command_cb (GObject       *source_object,
                   GAsyncResult  *res,
                   gpointer       user_data)
{
  GTask *task = G_TASK (user_data);
  GVfsUDisks2Mount *mount = g_task_get_source_object (task);
  GError *error;
  gint exit_status;
  gchar *standard_error = NULL;

  error = NULL;
  if (!gvfs_udisks2_utils_spawn_finish (res,
                                        &exit_status,
                                        NULL, /* gchar **out_standard_output */
                                        &standard_error,
                                        &error))
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      goto out;
    }

  if (WIFEXITED (exit_status) && WEXITSTATUS (exit_status) == 0)
    {
      gvfs_udisks2_volume_monitor_update (mount->monitor);

      /* Call the unmount_completed function explicitly here as it is too
       * late to emit show-unmount-progress signal from the notify::completed
       * callback.
       */
      umount_completed (task);

      g_task_return_boolean (task, TRUE);
      g_object_unref (task);
      goto out;
    }

  if (standard_error != NULL &&
      (strstr (standard_error, "device is busy") != NULL ||
       strstr (standard_error, "target is busy") != NULL))
    {
      unmount_show_busy (task, mount->mount_path);
      goto out;
    }

  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "%s", standard_error);
  g_object_unref (task);

 out:
  g_free (standard_error);
}

static void
unmount_do (GTask       *task,
            gboolean     force)
{
  UnmountData *data = g_task_get_task_data (task);
  GVfsUDisks2Mount *mount = g_task_get_source_object (task);
  GVariantBuilder builder;

  data->in_progress = TRUE;

  if (data->mount_operation != NULL)
    {
      gvfs_udisks2_unmount_notify_start (data->mount_operation,
                                         G_MOUNT (g_task_get_source_object (task)), NULL);
      g_signal_connect_swapped (task, "notify::completed", G_CALLBACK (umount_completed), task);
    }

  /* Use the umount(8) command if there is no block device / filesystem */
  if (data->filesystem == NULL)
    {
      gchar *quoted_path;

      quoted_path = g_shell_quote (mount->mount_path);
      gvfs_udisks2_utils_spawn (10, /* timeout in seconds */
                                g_task_get_cancellable (task),
                                umount_command_cb,
                                task,
                                "umount %s %s",
                                force ? "-l " : "",
                                quoted_path);
      g_free (quoted_path);
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
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (data->filesystem), G_MAXINT);
  udisks_filesystem_call_unmount (data->filesystem,
                                  g_variant_builder_end (&builder),
                                  g_task_get_cancellable (task),
                                  unmount_cb,
                                  task);

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
  UDisksBlock *block;
  GTask *task;

  /* first emit the ::mount-pre-unmount signal */
  g_signal_emit_by_name (mount->monitor, "mount-pre-unmount", mount);

  task = g_task_new (_mount, cancellable, callback, user_data);
  g_task_set_source_tag (task, gvfs_udisks2_mount_unmount_with_operation);

  data = g_new0 (UnmountData, 1);
  data->mount_operation = mount_operation != NULL ? g_object_ref (mount_operation) : NULL;
  data->flags = flags;

  g_task_set_task_data (task, data, (GDestroyNotify)unmount_data_free);

#ifdef HAVE_BURN
  if (mount->is_burn_mount)
    {
      /* burn mounts are really never mounted so complete successfully immediately */
      g_task_return_boolean (task, TRUE);
      g_object_unref (task);
      return;
    }
#endif

  block = NULL;
  if (mount->volume != NULL)
    block = gvfs_udisks2_volume_get_block (mount->volume);
  if (block != NULL)
    {
      GDBusObject *object;
      object = g_dbus_interface_get_object (G_DBUS_INTERFACE (block));
      if (object == NULL)
        {
          g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "No object for D-Bus interface");
          g_object_unref (task);
          return;
        }
      data->filesystem = udisks_object_get_filesystem (UDISKS_OBJECT (object));
      if (data->filesystem == NULL)
        {
          UDisksBlock *cleartext_block;

          data->encrypted = udisks_object_get_encrypted (UDISKS_OBJECT (object));
          if (data->encrypted == NULL)
            {
              g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                       "No filesystem or encrypted interface on D-Bus object");
              g_object_unref (task);
              return;
            }

          cleartext_block = udisks_client_get_cleartext_block (gvfs_udisks2_volume_monitor_get_udisks_client (mount->monitor),
                                                               block);
          if (cleartext_block != NULL)
            {
              data->filesystem = udisks_object_get_filesystem (UDISKS_OBJECT (g_dbus_interface_get_object (G_DBUS_INTERFACE (cleartext_block))));
              g_object_unref (cleartext_block);
              if (data->filesystem == NULL)
                {
                  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                           "No filesystem interface on D-Bus object for cleartext device");
                  g_object_unref (task);
                  return;
                }
            }
        }
      g_assert (data->filesystem != NULL);
    }
  unmount_do (task, FALSE /* force */);
}

static gboolean
gvfs_udisks2_mount_unmount_with_operation_finish (GMount        *mount,
                                                  GAsyncResult  *result,
                                                  GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, mount), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, gvfs_udisks2_mount_unmount_with_operation), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
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

static void
eject_wrapper_callback (GObject       *source_object,
                        GAsyncResult  *res,
                        gpointer       user_data)
{
  GTask *task = G_TASK (user_data);
  GError *error = NULL;

  if (g_drive_eject_with_operation_finish (G_DRIVE (source_object), res, &error))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
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
  GTask *task;
  GDrive *drive;

  task = g_task_new (_mount, cancellable, callback, user_data);
  g_task_set_source_tag (task, gvfs_udisks2_mount_eject_with_operation);

  drive = NULL;
  if (mount->volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (mount->volume));

  if (drive != NULL)
    {
      g_drive_eject_with_operation (drive, flags, mount_operation, cancellable, eject_wrapper_callback, task);
    }
  else
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               _("Operation not supported by backend"));
      g_object_unref (task);
    }
}

static gboolean
gvfs_udisks2_mount_eject_with_operation_finish (GMount        *_mount,
                                                GAsyncResult  *result,
                                                GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, _mount), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, gvfs_udisks2_mount_eject_with_operation), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
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

#ifdef HAVE_BURN
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
#endif
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

  if (mount->device_file != NULL)
    {
      GUdevDevice *gudev_device;
      gudev_device = g_udev_client_query_by_device_file (gvfs_udisks2_volume_monitor_get_gudev_client (mount->monitor),
                                                         mount->device_file);
      if (gudev_device != NULL)
        {
          /* Check if its bootable */
          const gchar *boot_sys_id;

          boot_sys_id = g_udev_device_get_property (gudev_device,
                                                    "ID_FS_BOOT_SYSTEM_ID");
          if (boot_sys_id != NULL ||
              g_udev_device_get_property_as_boolean (gudev_device, "OSINFO_BOOTABLE"))
            g_ptr_array_add (p, g_strdup ("x-content/bootable-media"));

          /* Check for media player */
          if (g_udev_device_has_property (gudev_device, "ID_MEDIA_PLAYER"))
            g_ptr_array_add (p, g_strdup ("x-content/audio-player"));

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
  GTask *task;
  char **type;
  GError *error = NULL;

  task = g_task_new (mount, cancellable, callback, user_data);
  g_task_set_source_tag (task, gvfs_udisks2_mount_guess_content_type);

  type = gvfs_udisks2_mount_guess_content_type_sync (mount, FALSE, cancellable, &error);
  if (error != NULL)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, type, (GDestroyNotify)g_strfreev);

  g_object_unref (task);
}

static gchar **
gvfs_udisks2_mount_guess_content_type_finish (GMount        *mount,
                                              GAsyncResult  *result,
                                              GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, mount), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (result, gvfs_udisks2_mount_guess_content_type), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
/* ---------------------------------------------------------------------------------------------------- */

static const gchar *
gvfs_udisks2_mount_get_sort_key (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->sort_key;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
gvfs_udisks2_mount_mount_iface_init (GMountIface *iface)
{
  iface->get_root = gvfs_udisks2_mount_get_root;
  iface->get_name = gvfs_udisks2_mount_get_name;
  iface->get_icon = gvfs_udisks2_mount_get_icon;
  iface->get_symbolic_icon = gvfs_udisks2_mount_get_symbolic_icon;
  iface->get_uuid = gvfs_udisks2_mount_get_uuid;
  iface->get_drive = gvfs_udisks2_mount_get_drive;
  iface->get_volume = gvfs_udisks2_mount_get_volume_;
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
  iface->get_sort_key = gvfs_udisks2_mount_get_sort_key;
}

gboolean
gvfs_udisks2_mount_has_volume (GVfsUDisks2Mount   *mount,
                               GVfsUDisks2Volume  *volume)
{
  return mount->volume == volume;
}

GVfsUDisks2Volume *
gvfs_udisks2_mount_get_volume (GVfsUDisks2Mount *mount)
{
  return mount->volume;
}
