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
  gboolean is_media_removable;
  gboolean has_media;
  gboolean can_eject;
  gboolean can_poll_for_media;
  gboolean is_media_check_automatic;
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
  gboolean old_is_media_removable;
  gboolean old_has_media;
  gboolean old_can_eject;
  gboolean old_is_media_check_automatic;
  gboolean old_can_poll_for_media;

  /* save old values */
  old_is_media_removable = drive->is_media_removable;
  old_has_media = drive->has_media;
  old_can_eject = drive->can_eject;
  old_can_poll_for_media = drive->can_poll_for_media;
  old_is_media_check_automatic = drive->is_media_check_automatic;

  old_name = g_strdup (drive->name);
  old_device_file = g_strdup (drive->device_file);
  old_icon = drive->icon != NULL ? g_object_ref (drive->icon) : NULL;

  /* in with the new */
  device = gdu_presentable_get_device (drive->presentable);

  if (drive->icon != NULL)
    g_object_unref (drive->icon);
  drive->icon = gdu_presentable_get_icon (drive->presentable);

  g_free (drive->name);
  drive->name = gdu_presentable_get_name (drive->presentable);

  /* the GduDevice for an activatable drive (such as RAID) is NULL if the drive is not activated */
  if (device == NULL)
    {
      g_free (drive->device_file);
      drive->device_file = NULL;
      drive->is_media_removable = TRUE;
      drive->has_media = TRUE;
      drive->can_eject = FALSE;
    }
  else
    {
      g_free (drive->device_file);
      drive->device_file = g_strdup (gdu_device_get_device_file (device));
      drive->is_media_removable = gdu_device_is_removable (device);
      drive->has_media = gdu_device_is_media_available (device);
      /* All drives with removable media are ejectable
       *
       * See http://bugzilla.gnome.org/show_bug.cgi?id=576587 for why we want this.
       */
      drive->can_eject = gdu_device_drive_get_is_media_ejectable (device) || gdu_device_drive_get_requires_eject (device) || gdu_device_is_removable (device) || gdu_device_drive_get_can_detach (device);
      drive->is_media_check_automatic = gdu_device_is_media_change_detected (device);
      drive->can_poll_for_media = TRUE;
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
              (old_is_media_check_automatic == drive->is_media_check_automatic) &&
              (old_can_poll_for_media == drive->can_poll_for_media) &&
              (g_strcmp0 (old_name, drive->name) == 0) &&
              (g_strcmp0 (old_device_file, drive->device_file) == 0) &&
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
  GGduDrive *drive = G_GDU_DRIVE (drive);
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

static void
detach_cb (GduDevice *device,
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
eject_cb (GduDevice *device,
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
g_gdu_drive_eject_do (GDrive              *_drive,
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

      /* Note that we also offer the Eject option for non-removable
       * devices (see update_drive() above) that are detachable so the
       * device may actually not be removable when we get here...
       *
       * However, keep in mind that a device may be both removable and
       * detachable (e.g. a usb optical drive)..
       *
       * Now, consider what would happen if we detached a USB optical
       * drive? The device would power down without actually ejecting
       * the media... and it would require a power-cycle or a replug
       * to use it for other media. Therefore, never detach devices
       * with removable media, only eject them.
       */
      if (gdu_device_drive_get_can_detach (device) && !gdu_device_is_removable (device))
        {
          gdu_device_op_drive_detach (device, detach_cb, simple);
        }
      else
        {
          gdu_device_op_drive_eject (device, eject_cb, simple);
        }
      g_object_unref (device);
    }
}

typedef struct {
  GDrive *drive;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  GMountUnmountFlags flags;

  GList *pending_mounts;
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

static void _eject_unmount_mounts (UnmountMountsOp *data);

static void
_eject_unmount_mounts_cb (GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
  UnmountMountsOp *data = user_data;
  GMount *mount = G_MOUNT (source_object);
  GSimpleAsyncResult *simple;
  GError *error = NULL;

  if (!g_mount_unmount_finish (mount, res, &error))
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
      _eject_unmount_mounts (data);
    }

  g_object_unref (mount);
}

static void
_eject_unmount_mounts (UnmountMountsOp *data)
{
  GMount *mount;

  if (data->pending_mounts == NULL)
    {

      /*g_warning ("all pending mounts done; ejecting drive");*/

      g_gdu_drive_eject_do (data->drive,
                            data->cancellable,
                            data->callback,
                            data->user_data);

      g_object_unref (data->drive);
      g_free (data);
    }
  else
    {
      mount = data->pending_mounts->data;
      data->pending_mounts = g_list_remove (data->pending_mounts, mount);

      /*g_warning ("unmounting %p", mount);*/

      g_mount_unmount (mount,
                       data->flags,
                       data->cancellable,
                       _eject_unmount_mounts_cb,
                       data);
    }
}

static void
g_gdu_drive_eject (GDrive              *drive,
                   GMountUnmountFlags   flags,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  GGduDrive *gdu_drive = G_GDU_DRIVE (drive);
  UnmountMountsOp *data;
  GList *l;

  /* first we need to go through all the volumes and unmount their assoicated mounts (if any) */

  data = g_new0 (UnmountMountsOp, 1);
  data->drive = g_object_ref (drive);
  data->cancellable = cancellable;
  data->callback = callback;
  data->user_data = user_data;
  data->flags = flags;

  for (l = gdu_drive->volumes; l != NULL; l = l->next)
    {
      GGduVolume *volume = l->data;
      GMount *mount;

      mount = g_volume_get_mount (G_VOLUME (volume));
      if (mount != NULL && g_mount_can_unmount (mount))
        data->pending_mounts = g_list_prepend (data->pending_mounts, g_object_ref (mount));
    }

  _eject_unmount_mounts (data);
}

static gboolean
g_gdu_drive_eject_finish (GDrive        *drive,
                          GAsyncResult  *result,
                          GError       **error)
{
  return !g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
}

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
  iface->poll_for_media = g_gdu_drive_poll_for_media;
  iface->poll_for_media_finish = g_gdu_drive_poll_for_media_finish;
  iface->get_identifier = g_gdu_drive_get_identifier;
  iface->enumerate_identifiers = g_gdu_drive_enumerate_identifiers;
}

gboolean
g_gdu_drive_has_device_file (GGduDrive      *drive,
                             const gchar    *device_file)
{
  return g_strcmp0 (drive->device_file, device_file) == 0;
}

gboolean
g_gdu_drive_has_presentable (GGduDrive       *drive,
                             GduPresentable  *presentable)
{
  return gdu_presentable_get_id (drive->presentable) == gdu_presentable_get_id (presentable);
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
