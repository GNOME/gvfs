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

#include <glib.h>
#include <glib/gi18n-lib.h>

#include "ggduvolumemonitor.h"
#include "ggdudrive.h"
#include "ggduvolume.h"

struct _GGduDrive {
  GObject parent;

  GVolumeMonitor  *volume_monitor; /* owned by volume monitor */
  GList           *volumes;        /* entries in list are owned by volume_monitor */

  GduPresentable *presentable;

  /* the following members need to be set upon construction */
  GIcon *icon;
  gchar *name;
  gchar *device_file;
  dev_t dev;
  gboolean is_media_removable;
  gboolean has_media;
  gboolean can_eject;
  gboolean can_poll_for_media;
  gboolean is_media_check_automatic;

  GDriveStartStopType start_stop_type;
  gboolean can_start;
  gboolean can_start_degraded;
  gboolean can_stop;
};

static void g_gdu_drive_drive_iface_init (GDriveIface *iface);

G_DEFINE_TYPE_EXTENDED (GGduDrive, g_gdu_drive, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_DRIVE,
                                               g_gdu_drive_drive_iface_init))

static void presentable_changed (GduPresentable *presentable,
                                 GGduDrive      *drive);

static void presentable_job_changed (GduPresentable *presentable,
                                     GGduDrive      *drive);

static void
g_gdu_drive_finalize (GObject *object)
{
  GList *l;
  GGduDrive *drive;

  drive = G_GDU_DRIVE (object);

  for (l = drive->volumes; l != NULL; l = l->next)
    {
      GGduVolume *volume = l->data;
      g_gdu_volume_unset_drive (volume, drive);
    }

  if (drive->presentable != NULL)
    {
      g_signal_handlers_disconnect_by_func (drive->presentable, presentable_changed, drive);
      g_signal_handlers_disconnect_by_func (drive->presentable, presentable_job_changed, drive);
      g_object_unref (drive->presentable);
    }

  if (drive->icon != NULL)
    g_object_unref (drive->icon);
  g_free (drive->name);
  g_free (drive->device_file);

  if (G_OBJECT_CLASS (g_gdu_drive_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_gdu_drive_parent_class)->finalize) (object);
}

static void
g_gdu_drive_class_init (GGduDriveClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_gdu_drive_finalize;
}

static void
g_gdu_drive_init (GGduDrive *gdu_drive)
{
}

static void
emit_changed (GGduDrive *drive)
{
  g_signal_emit_by_name (drive, "changed");
  g_signal_emit_by_name (drive->volume_monitor, "drive_changed", drive);
}

static gboolean
update_drive (GGduDrive *drive)
{
  GduDevice *device;
  gboolean changed;
  GIcon *old_icon;
  gchar *old_name;
  gchar *old_device_file;
  dev_t old_dev;
  gboolean old_is_media_removable;
  gboolean old_has_media;
  gboolean old_can_eject;
  gboolean old_can_start;
  gboolean old_can_start_degraded;
  gboolean old_can_stop;
  gboolean old_start_stop_type;
  gboolean old_is_media_check_automatic;
  gboolean old_can_poll_for_media;

  /* save old values */
  old_is_media_removable = drive->is_media_removable;
  old_has_media = drive->has_media;
  old_can_eject = drive->can_eject;
  old_can_start = drive->can_start;
  old_can_start_degraded = drive->can_start_degraded;
  old_can_stop = drive->can_stop;
  old_start_stop_type = drive->start_stop_type;
  old_can_poll_for_media = drive->can_poll_for_media;
  old_is_media_check_automatic = drive->is_media_check_automatic;

  old_name = g_strdup (drive->name);
  old_device_file = g_strdup (drive->device_file);
  old_dev = drive->dev;
  old_icon = drive->icon != NULL ? g_object_ref (drive->icon) : NULL;

  /* in with the new */
  device = gdu_presentable_get_device (drive->presentable);

  if (drive->icon != NULL)
    g_object_unref (drive->icon);
  drive->icon = gdu_presentable_get_icon (drive->presentable);

  g_free (drive->name);
  if (_is_pc_floppy_drive (device))
    drive->name = g_strdup (_("Floppy Drive"));
  else
    drive->name = gdu_presentable_get_name (drive->presentable);

  /* It's perfectly fine to not have a GduDevice - for example, this is the case for non-running
   * MD RAID arrays as well as LVM2 Volume Group "drives"
   */
  if (device == NULL)
    {
      g_free (drive->device_file);
      drive->dev = 0;
      drive->device_file = NULL;
      drive->is_media_removable = FALSE;
      drive->has_media = TRUE;
      drive->can_eject = FALSE;
      drive->can_poll_for_media = FALSE;
    }
  else
    {
      g_free (drive->device_file);
      drive->dev = gdu_device_get_dev (device);
      drive->device_file = g_strdup (gdu_device_get_device_file (device));
      drive->is_media_removable = gdu_device_is_removable (device);
      drive->has_media = gdu_device_is_media_available (device);
      /* All drives with removable media are ejectable
       *
       * See http://bugzilla.gnome.org/show_bug.cgi?id=576587 for why we want this.
       *
       * See also below where we e.g. set can_eject to TRUE for non-removable drives.
       */
      drive->can_eject = ((gdu_device_drive_get_is_media_ejectable (device) || gdu_device_is_removable (device)) &&
                          gdu_device_is_media_available (device) && ! _is_pc_floppy_drive (device)) ||
                         gdu_device_drive_get_requires_eject (device);
      drive->is_media_check_automatic = gdu_device_is_media_change_detected (device);
      drive->can_poll_for_media = gdu_device_is_removable (device);
    }

  /* determine start/stop type */
  drive->can_stop = FALSE;
  drive->can_start = FALSE;
  drive->can_start_degraded = FALSE;
  drive->start_stop_type = G_DRIVE_START_STOP_TYPE_UNKNOWN;
  if (gdu_drive_is_activatable (GDU_DRIVE (drive->presentable)))
    {
      gboolean can_activate;
      gboolean degraded;

      can_activate = gdu_drive_can_activate (GDU_DRIVE (drive->presentable), &degraded);

      drive->can_stop  = gdu_drive_can_deactivate (GDU_DRIVE (drive->presentable));
      drive->can_start = can_activate && !degraded;
      drive->can_start_degraded = can_activate && degraded;
      drive->start_stop_type = G_DRIVE_START_STOP_TYPE_MULTIDISK;
    }
  else if (device != NULL && gdu_device_drive_get_can_detach (device))
    {
      /* Ideally, for non-ejectable devices (e.g. non-cdrom, non-zip)
       * such as USB sticks we'd display "Eject" instead of "Shutdown"
       * since it is more familiar and the common case. The way this
       * should work is that after the Eject() method returns we call
       * Detach() - see eject_cb() below.
       *
       * (Note that it's not enough to just call Detach() since some
       * devices, such as the Kindle, only works with Eject(). So we
       * call them both in order).
       *
       * We actually used to do this (and that's why eject_cb() still
       * has this code) but some systems use internal USB devices for
       * e.g. SD card readers. If we were to detach these then the
       * user would have to power-cycle the system to get the device
       * back. See http://bugs.freedesktop.org/show_bug.cgi?id=24343
       * for more details.
       *
       * In the future, if we know for sure that a port is external
       * (like, from DMI data) we can go back to doing this. For now
       * the user will get all the options...
       */
      drive->can_stop = TRUE;
      drive->can_start = FALSE;
      drive->can_start_degraded = FALSE;
      drive->start_stop_type = G_DRIVE_START_STOP_TYPE_SHUTDOWN;
    }

  if (device != NULL)
    g_object_unref (device);

  /* Never use empty/blank names (#582772) */
  if (drive->name == NULL || strlen (drive->name) == 0)
    {
      if (drive->device_file != NULL)
        drive->name = g_strdup_printf (_("Unnamed Drive (%s)"), drive->device_file);
      else
        drive->name = g_strdup (_("Unnamed Drive"));
    }

  /* compute whether something changed */
  changed = !((old_is_media_removable == drive->is_media_removable) &&
              (old_has_media == drive->has_media) &&
              (old_can_eject == drive->can_eject) &&
              (old_can_start == drive->can_start) &&
              (old_can_start_degraded == drive->can_start_degraded) &&
              (old_can_stop == drive->can_stop) &&
              (old_start_stop_type == drive->start_stop_type) &&
              (old_is_media_check_automatic == drive->is_media_check_automatic) &&
              (old_can_poll_for_media == drive->can_poll_for_media) &&
              (g_strcmp0 (old_name, drive->name) == 0) &&
              (g_strcmp0 (old_device_file, drive->device_file) == 0) &&
              (old_dev == drive->dev) &&
              g_icon_equal (old_icon, drive->icon)
              );

  /* free old values */
  g_free (old_name);
  g_free (old_device_file);
  if (old_icon != NULL)
    g_object_unref (old_icon);

  /*g_debug ("in update_drive(); has_media=%d changed=%d", drive->has_media, changed);*/

  return changed;
}

static void
presentable_changed (GduPresentable *presentable,
                     GGduDrive      *drive)
{
  /*g_debug ("drive: presentable_changed: %p: %s", drive, gdu_presentable_get_id (GDU_PRESENTABLE (presentable)));*/
  if (update_drive (drive))
    emit_changed (drive);
}

static void
presentable_job_changed (GduPresentable *presentable,
                         GGduDrive      *drive)
{
  /*g_debug ("drive: presentable_job_changed: %p: %s", drive, gdu_presentable_get_id (GDU_PRESENTABLE (presentable)));*/
  if (update_drive (drive))
    emit_changed (drive);
}

GGduDrive *
g_gdu_drive_new (GVolumeMonitor       *volume_monitor,
                 GduPresentable       *presentable)
{
  GGduDrive *drive;

  drive = g_object_new (G_TYPE_GDU_DRIVE, NULL);
  drive->volume_monitor = volume_monitor;
  g_object_add_weak_pointer (G_OBJECT (volume_monitor), (gpointer) &(drive->volume_monitor));

  drive->presentable = g_object_ref (presentable);

  g_signal_connect (drive->presentable, "changed", G_CALLBACK (presentable_changed), drive);
  g_signal_connect (drive->presentable, "job-changed", G_CALLBACK (presentable_job_changed), drive);

  update_drive (drive);

  return drive;
}

void
g_gdu_drive_disconnected (GGduDrive *drive)
{
  GList *l, *volumes;

  volumes = drive->volumes;
  drive->volumes = NULL;

  for (l = volumes; l != NULL; l = l->next)
    {
      GGduVolume *volume = l->data;
      g_gdu_volume_unset_drive (volume, drive);
    }

  g_list_free (volumes);
}

void
g_gdu_drive_set_volume (GGduDrive *drive,
                        GGduVolume *volume)
{
  if (g_list_find (drive->volumes, volume) == NULL)
    {
      drive->volumes = g_list_prepend (drive->volumes, volume);
      emit_changed (drive);
    }
}

void
g_gdu_drive_unset_volume (GGduDrive *drive,
                          GGduVolume *volume)
{
  GList *l;

  l = g_list_find (drive->volumes, volume);
  if (l != NULL)
    {
      drive->volumes = g_list_delete_link (drive->volumes, l);
      emit_changed (drive);
    }
}

static GIcon *
g_gdu_drive_get_icon (GDrive *_drive)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  return drive->icon != NULL ? g_object_ref (drive->icon) : NULL;
}

static char *
g_gdu_drive_get_name (GDrive *_drive)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  return g_strdup (drive->name);
}

static GList *
g_gdu_drive_get_volumes (GDrive *_drive)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  GList *l;

  l = g_list_copy (drive->volumes);
  g_list_foreach (l, (GFunc) g_object_ref, NULL);

  return l;
}

static gboolean
g_gdu_drive_has_volumes (GDrive *_drive)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  gboolean res;

  res = drive->volumes != NULL;

  return res;
}

static gboolean
g_gdu_drive_is_media_removable (GDrive *_drive)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  return drive->is_media_removable;
}

static gboolean
g_gdu_drive_has_media (GDrive *_drive)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  return drive->has_media;
}

static gboolean
g_gdu_drive_is_media_check_automatic (GDrive *_drive)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  return drive->is_media_check_automatic;
}

static gboolean
g_gdu_drive_can_eject (GDrive *_drive)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  return drive->can_eject;
}

static gboolean
g_gdu_drive_can_poll_for_media (GDrive *_drive)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  return drive->can_poll_for_media;
}

static gboolean
g_gdu_drive_can_start (GDrive *_drive)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  return drive->can_start;
}

static gboolean
g_gdu_drive_can_start_degraded (GDrive *_drive)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  return drive->can_start_degraded;
}

static gboolean
g_gdu_drive_can_stop (GDrive *_drive)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  return drive->can_stop;
}

static GDriveStartStopType
g_gdu_drive_get_start_stop_type (GDrive *_drive)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  return drive->start_stop_type;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef void (*UnmountsMountsFunc)  (GDrive              *drive,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data,
                                     gpointer             on_all_unmounted_data);

typedef struct {
  GDrive *drive;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GMountOperation *mount_operation;
  GCancellable *cancellable;
  GMountUnmountFlags flags;

  GList *pending_mounts;

  UnmountsMountsFunc on_all_unmounted;
  gpointer on_all_unmounted_data;
} UnmountMountsOp;

static void
free_unmount_mounts_op (UnmountMountsOp *data)
{
  GList *l;

  for (l = data->pending_mounts; l != NULL; l = l->next)
    {
      GMount *mount = l->data;
      g_object_unref (mount);
    }
  g_list_free (data->pending_mounts);
}

static void
unmount_mounts_cb (GObject       *source_object,
                   GAsyncResult  *res,
                   gpointer       user_data);

static void
unmount_mounts_do (UnmountMountsOp *data)
{
  if (data->pending_mounts == NULL)
    {

      /*g_warning ("all pending mounts done");*/
      data->on_all_unmounted (data->drive,
                              data->cancellable,
                              data->callback,
                              data->user_data,
                              data->on_all_unmounted_data);

      g_object_unref (data->drive);
      g_free (data);
    }
  else
    {
      GMount *mount;

      mount = data->pending_mounts->data;
      data->pending_mounts = g_list_remove (data->pending_mounts, mount);

      /*g_warning ("unmounting %p", mount);*/

      g_mount_unmount_with_operation (mount,
                                      data->flags,
                                      data->mount_operation,
                                      data->cancellable,
                                      unmount_mounts_cb,
                                      data);
    }
}

static void
unmount_mounts_cb (GObject *source_object,
                   GAsyncResult *res,
                   gpointer user_data)
{
  UnmountMountsOp *data = user_data;
  GMount *mount = G_MOUNT (source_object);
  GSimpleAsyncResult *simple;
  GError *error = NULL;

  if (!g_mount_unmount_with_operation_finish (mount, res, &error))
    {
      /* make the error dialog more targeted to the drive.. unless the user has already seen a dialog */
      if (error->code != G_IO_ERROR_FAILED_HANDLED)
        {
          g_error_free (error);
          error = g_error_new (G_IO_ERROR, G_IO_ERROR_BUSY,
                               _("Failed to eject media; one or more volumes on the media are busy."));
        }

      /* unmount failed; need to fail the whole eject operation */
      simple = g_simple_async_result_new_from_error (G_OBJECT (data->drive),
                                                     data->callback,
                                                     data->user_data,
                                                     error);
      g_error_free (error);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);

      free_unmount_mounts_op (data);
    }
  else
    {

      /*g_warning ("successfully unmounted %p", mount);*/

      /* move on to the next mount.. */
      unmount_mounts_do (data);
    }

  g_object_unref (mount);
}

static void
unmount_mounts (GGduDrive           *drive,
                GMountUnmountFlags   flags,
                GMountOperation     *mount_operation,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             user_data,
                UnmountsMountsFunc   on_all_unmounted,
                gpointer             on_all_unmounted_data)
{
  GMount *mount;
  UnmountMountsOp *data;
  GList *l;

  data = g_new0 (UnmountMountsOp, 1);
  data->drive = g_object_ref (drive);
  data->mount_operation = mount_operation;
  data->cancellable = cancellable;
  data->callback = callback;
  data->user_data = user_data;
  data->flags = flags;
  data->on_all_unmounted = on_all_unmounted;
  data->on_all_unmounted_data = on_all_unmounted_data;

  for (l = drive->volumes; l != NULL; l = l->next)
    {
      GGduVolume *volume = l->data;
      mount = g_volume_get_mount (G_VOLUME (volume));
      if (mount != NULL && g_mount_can_unmount (mount))
        data->pending_mounts = g_list_prepend (data->pending_mounts, g_object_ref (mount));
    }

  unmount_mounts_do (data);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
detach_after_eject_cb (GduDevice *device,
                       GError    *error,
                       gpointer   user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  /* Don't return an error here - this is because some devices, such as
   * the Kindle, can do Eject() but not Detach() e.g. the STOP UNIT
   * command or any other part of Detach() may fail.
   */
  if (error != NULL)
    {
      g_warning ("Detach() after Eject() failed with: %s", error->message);
      g_error_free (error);
    }

  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
eject_cb (GduDevice *device,
          GError    *error,
          gpointer   user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  GGduDrive *drive;
  gboolean drive_detachable;

  drive = G_GDU_DRIVE (g_async_result_get_source_object (G_ASYNC_RESULT (simple)));
  drive_detachable = drive->can_stop == FALSE && drive->start_stop_type == G_DRIVE_START_STOP_TYPE_SHUTDOWN;

  if (error != NULL && error->code == G_IO_ERROR_FAILED &&
      drive_detachable && ! drive->has_media && drive->is_media_removable)
    {
      /* Silently drop the error if there's no media in drive and we're still trying to detach it (see below) */
      g_error_free (error);
      error = NULL;
    }

  if (error != NULL)
    {
      g_simple_async_result_set_from_error (simple, error);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      g_error_free (error);
      goto out;
    }

  if (drive_detachable)
    {
      /* If device is not ejectable but it is detachable and we don't support stop(),
       * then also run Detach() after Eject() - see update_drive() for details for why...
       */
      gdu_device_op_drive_detach (device, detach_after_eject_cb, simple);
    }
  else
    {
      /* otherwise we are done */
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
    }
  g_object_unref (drive);

 out:
  ;
}

static void
g_gdu_drive_eject_on_all_unmounted (GDrive              *_drive,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data,
                                    gpointer             on_all_unmounted_data)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  GSimpleAsyncResult *simple;
  GduDevice *device;

  device = gdu_presentable_get_device (drive->presentable);
  if (device == NULL)
    {
      simple = g_simple_async_result_new_error (G_OBJECT (drive),
                                                callback,
                                                user_data,
                                                G_IO_ERROR,
                                                G_IO_ERROR_FAILED,
                                                "Drive is activatable and not running");
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
    }
  else
    {
      simple = g_simple_async_result_new (G_OBJECT (drive),
                                          callback,
                                          user_data,
                                          NULL);

      gdu_device_op_drive_eject (device, eject_cb, simple);
    }
}

static void
g_gdu_drive_eject_with_operation (GDrive              *_drive,
                                  GMountUnmountFlags   flags,
                                  GMountOperation     *mount_operation,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);

  /* first we need to go through all the volumes and unmount their assoicated mounts (if any) */
  unmount_mounts (drive,
                  flags,
                  mount_operation,
                  cancellable,
                  callback,
                  user_data,
                  g_gdu_drive_eject_on_all_unmounted,
                  NULL);
}

static gboolean
g_gdu_drive_eject_with_operation_finish (GDrive        *drive,
                                         GAsyncResult  *result,
                                         GError       **error)
{
  return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
}

static void
g_gdu_drive_eject (GDrive              *drive,
                   GMountUnmountFlags   flags,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  g_gdu_drive_eject_with_operation (drive, flags, NULL, cancellable, callback, user_data);
}

static gboolean
g_gdu_drive_eject_finish (GDrive        *drive,
                          GAsyncResult  *result,
                          GError       **error)
{
  return g_gdu_drive_eject_with_operation_finish (drive, result, error);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
stop_cb (GduDevice *device,
         GError    *error,
         gpointer   user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  if (error != NULL)
    {
      g_simple_async_result_set_from_error (simple, error);
      g_error_free (error);
    }

  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
drive_deactivate_cb (GduDrive  *drive,
                     GError    *error,
                     gpointer   user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  if (error != NULL)
    {
      g_simple_async_result_set_from_error (simple, error);
      g_error_free (error);
    }

  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static void
g_gdu_drive_stop_on_all_unmounted (GDrive              *_drive,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data,
                                   gpointer             on_all_unmounted_data)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  GSimpleAsyncResult *simple;
  GduDevice *device;

  simple = g_simple_async_result_new (G_OBJECT (drive),
                                      callback,
                                      user_data,
                                      NULL);

  switch (drive->start_stop_type)
    {
    case G_DRIVE_START_STOP_TYPE_SHUTDOWN:
      device = gdu_presentable_get_device (drive->presentable);
      if (device == NULL)
        {
          g_simple_async_result_set_error (simple,
                                           G_IO_ERROR,
                                           G_IO_ERROR_FAILED,
                                           "Cannot detach: drive has no GduDevice object");
          g_simple_async_result_complete_in_idle (simple);
          g_object_unref (simple);
        }
      else
        {
          gdu_device_op_drive_detach (device, stop_cb, simple);
          g_object_unref (device);
        }
      break;

    case G_DRIVE_START_STOP_TYPE_MULTIDISK:
      gdu_drive_deactivate (GDU_DRIVE (drive->presentable), drive_deactivate_cb, simple);
      break;

    default:
      g_simple_async_result_set_error (simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_NOT_SUPPORTED,
                                       "start_stop_type %d not supported",
                                       drive->start_stop_type);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      break;
    }
}

static void
g_gdu_drive_stop (GDrive              *_drive,
                  GMountUnmountFlags   flags,
                  GMountOperation     *mount_operation,
                  GCancellable        *cancellable,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);

  /* first we need to go through all the volumes and unmount their assoicated mounts (if any) */
  unmount_mounts (drive,
                  flags,
                  mount_operation,
                  cancellable,
                  callback,
                  user_data,
                  g_gdu_drive_stop_on_all_unmounted,
                  NULL);
}

static gboolean
g_gdu_drive_stop_finish (GDrive        *drive,
                         GAsyncResult  *result,
                         GError       **error)
{
  return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
start_cb (GduDrive   *drive,
          gchar      *assembled_drive_object_path,
          GError     *error,
          gpointer    user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);

  if (error != NULL)
    {
      g_simple_async_result_set_error (simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_FAILED,
                                       "Failed activating drive: %s",
                                       error->message);
      g_error_free (error);
    }
  else
    {
      g_free (assembled_drive_object_path);
    }
  g_simple_async_result_complete (simple);
}

typedef struct
{
  GGduDrive *drive;
  GSimpleAsyncResult *simple;

  GMountOperation *start_operation;
  gulong start_operation_reply_handler_id;
} StartOpData;

static void
start_operation_reply (GMountOperation      *op,
                       GMountOperationResult result,
                       gpointer              user_data)
{
  StartOpData *data = user_data;
  gint choice;

  /* we got what we wanted; don't listen to any other signals from the start operation */
  if (data->start_operation_reply_handler_id != 0)
    {
      g_signal_handler_disconnect (data->start_operation, data->start_operation_reply_handler_id);
      data->start_operation_reply_handler_id = 0;
    }

  if (result != G_MOUNT_OPERATION_HANDLED)
    {
      if (result == G_MOUNT_OPERATION_ABORTED)
        {
          /* The user aborted the operation so consider it "handled" */
          g_simple_async_result_set_error (data->simple,
                                           G_IO_ERROR,
                                           G_IO_ERROR_FAILED_HANDLED,
                                           "Start operation dialog aborted (user should never see this error since "
                                           "it is G_IO_ERROR_FAILED_HANDLED)");
        }
      else
        {
          g_simple_async_result_set_error (data->simple,
                                           G_IO_ERROR,
                                           G_IO_ERROR_FAILED,
                                           "Expected G_MOUNT_OPERATION_HANDLED but got %d", result);
        }
      g_simple_async_result_complete (data->simple);
      goto out;
    }

  /* handle the user pressing cancel */
  choice = g_mount_operation_get_choice (data->start_operation);
  if (choice == 1)
    {
      g_simple_async_result_set_error (data->simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_FAILED_HANDLED,
                                       "User refused to start degraded array (user should never see this error since "
                                       "it is G_IO_ERROR_FAILED_HANDLED)");
      g_simple_async_result_complete (data->simple);
      goto out;
    }

  gdu_drive_activate (GDU_DRIVE (data->drive->presentable), start_cb, data->simple);

 out:
  g_object_unref (data->drive);
  g_object_unref (data->start_operation);
  g_free (data);
}

static void
g_gdu_drive_start (GDrive              *_drive,
                   GDriveStartFlags     flags,
                   GMountOperation     *start_operation,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  GSimpleAsyncResult *simple;
  gboolean degraded;

  /* TODO: handle GCancellable */

  if (!gdu_drive_can_activate (GDU_DRIVE (drive->presentable), &degraded))
    goto not_supported;

  if (start_operation == NULL && degraded)
    goto refuse_degraded_without_confirmation;

  simple = g_simple_async_result_new (G_OBJECT (drive),
                                      callback,
                                      user_data,
                                      NULL);

  if (degraded)
    {
      const gchar *message;
      const gchar *choices[3];
      StartOpData *data;

      message = _("Start drive in degraded mode?\n"
                  "Starting a drive in degraded mode means that "
                  "the drive is no longer tolerant to failures. "
                  "Data on the drive may be irrevocably lost if a "
                  "component fails.");

      choices[0] = _("Start Anyway");
      choices[1] = _("Cancel");
      choices[2] = NULL;

      data = g_new0 (StartOpData, 1);
      data->drive = g_object_ref (drive);
      data->simple = simple;
      data->start_operation = g_object_ref (start_operation);
      data->start_operation_reply_handler_id = g_signal_connect (start_operation,
                                                                 "reply",
                                                                 G_CALLBACK (start_operation_reply),
                                                                 data);

      g_signal_emit_by_name (start_operation,
                             "ask-question",
                             message,
                             choices);
    }
  else
    {
      gdu_drive_activate (GDU_DRIVE (drive->presentable), start_cb, simple);
    }

  return;

 not_supported:
  simple = g_simple_async_result_new_error (G_OBJECT (drive),
                                            callback,
                                            user_data,
                                            G_IO_ERROR,
                                            G_IO_ERROR_NOT_SUPPORTED,
                                            "Starting drive with start_stop_type %d is not supported",
                                            drive->start_stop_type);
  g_simple_async_result_complete_in_idle (simple);
  g_object_unref (simple);
  return;

 refuse_degraded_without_confirmation:
  simple = g_simple_async_result_new_error (G_OBJECT (drive),
                                            callback,
                                            user_data,
                                            G_IO_ERROR,
                                            G_IO_ERROR_FAILED,
                                            "Refusing to start degraded multidisk drive without user confirmation");
  g_simple_async_result_complete_in_idle (simple);
  g_object_unref (simple);

}

static gboolean
g_gdu_drive_start_finish (GDrive        *drive,
                          GAsyncResult  *result,
                          GError       **error)
{
  return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
poll_media_cb (GduDevice *device,
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
g_gdu_drive_poll_for_media (GDrive              *_drive,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  GSimpleAsyncResult *simple;
  GduDevice *device;

  device = gdu_presentable_get_device (drive->presentable);
  if (device == NULL)
    {
      simple = g_simple_async_result_new_error (G_OBJECT (drive),
                                                callback,
                                                user_data,
                                                G_IO_ERROR,
                                                G_IO_ERROR_FAILED,
                                                "Device is not active");
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
    }
  else
    {
      simple = g_simple_async_result_new (G_OBJECT (drive),
                                          callback,
                                          user_data,
                                          NULL);

      gdu_device_op_drive_poll_media (device, poll_media_cb, simple);
      g_object_unref (device);
    }
}

static gboolean
g_gdu_drive_poll_for_media_finish (GDrive        *drive,
                                   GAsyncResult  *result,
                                   GError       **error)
{
  return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
}

/* ---------------------------------------------------------------------------------------------------- */

static char *
g_gdu_drive_get_identifier (GDrive              *_drive,
                            const char          *kind)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  gchar *id;

  id = NULL;

  if (drive->device_file != NULL)
    {
      if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE) == 0)
        id = g_strdup (drive->device_file);
    }

  return id;
}

static char **
g_gdu_drive_enumerate_identifiers (GDrive *_drive)
{
  GGduDrive *drive = G_GDU_DRIVE (_drive);
  GPtrArray *p;

  p = g_ptr_array_new ();
  if (drive->device_file != NULL)
    g_ptr_array_add (p, g_strdup (G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));
  g_ptr_array_add (p, NULL);

  return (gchar **) g_ptr_array_free (p, FALSE);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_gdu_drive_drive_iface_init (GDriveIface *iface)
{
  iface->get_name = g_gdu_drive_get_name;
  iface->get_icon = g_gdu_drive_get_icon;
  iface->has_volumes = g_gdu_drive_has_volumes;
  iface->get_volumes = g_gdu_drive_get_volumes;
  iface->is_media_removable = g_gdu_drive_is_media_removable;
  iface->has_media = g_gdu_drive_has_media;
  iface->is_media_check_automatic = g_gdu_drive_is_media_check_automatic;
  iface->can_eject = g_gdu_drive_can_eject;
  iface->can_poll_for_media = g_gdu_drive_can_poll_for_media;
  iface->eject = g_gdu_drive_eject;
  iface->eject_finish = g_gdu_drive_eject_finish;
  iface->eject_with_operation = g_gdu_drive_eject_with_operation;
  iface->eject_with_operation_finish = g_gdu_drive_eject_with_operation_finish;
  iface->poll_for_media = g_gdu_drive_poll_for_media;
  iface->poll_for_media_finish = g_gdu_drive_poll_for_media_finish;
  iface->get_identifier = g_gdu_drive_get_identifier;
  iface->enumerate_identifiers = g_gdu_drive_enumerate_identifiers;

  iface->get_start_stop_type = g_gdu_drive_get_start_stop_type;
  iface->can_start = g_gdu_drive_can_start;
  iface->can_start_degraded = g_gdu_drive_can_start_degraded;
  iface->can_stop = g_gdu_drive_can_stop;
  iface->start = g_gdu_drive_start;
  iface->start_finish = g_gdu_drive_start_finish;
  iface->stop = g_gdu_drive_stop;
  iface->stop_finish = g_gdu_drive_stop_finish;
}

gboolean
g_gdu_drive_has_dev (GGduDrive      *drive,
                     dev_t           dev)
{
  return drive->dev == dev;
}

gboolean
g_gdu_drive_has_presentable (GGduDrive       *drive,
                             GduPresentable  *presentable)
{
  return g_strcmp0 (gdu_presentable_get_id (drive->presentable), gdu_presentable_get_id (presentable)) == 0;
}

time_t
g_gdu_drive_get_time_of_last_media_insertion (GGduDrive *drive)
{
  GduDevice *device;
  time_t ret;

  ret = 0;
  device = gdu_presentable_get_device (drive->presentable);
  if (device != NULL) {
    ret = gdu_device_get_media_detection_time (device);
    g_object_unref (device);
  }
  return ret;
}

GduPresentable *
g_gdu_drive_get_presentable (GGduDrive       *drive)
{
  return drive->presentable;
}
