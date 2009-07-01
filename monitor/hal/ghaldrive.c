/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2006-2007 Red Hat, Inc.
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

#include "ghalvolumemonitor.h"
#include "ghaldrive.h"
#include "ghalvolume.h"

struct _GHalDrive {
  GObject parent;

  GVolumeMonitor  *volume_monitor; /* owned by volume monitor */
  GList           *volumes;        /* entries in list are owned by volume_monitor */

  char *name;
  char *icon;
  char *device_path;

  gboolean can_eject;
  gboolean can_poll_for_media;
  gboolean is_media_check_automatic;
  gboolean has_media;
  gboolean uses_removable_media;

  HalDevice *device;
  HalPool *pool;
};

static void g_hal_drive_drive_iface_init (GDriveIface *iface);

G_DEFINE_TYPE_EXTENDED (GHalDrive, g_hal_drive, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_DRIVE,
                                               g_hal_drive_drive_iface_init))

static void
g_hal_drive_finalize (GObject *object)
{
  GList *l;
  GHalDrive *drive;
  
  drive = G_HAL_DRIVE (object);

  for (l = drive->volumes; l != NULL; l = l->next)
    {
      GHalVolume *volume = l->data;
      g_hal_volume_unset_drive (volume, drive);
    }

  g_free (drive->device_path);
  if (drive->device != NULL)
    g_object_unref (drive->device);
  if (drive->pool != NULL)
    g_object_unref (drive->pool);

  g_free (drive->name);
  g_free (drive->icon);

  if (drive->volume_monitor != NULL)
    g_object_remove_weak_pointer (G_OBJECT (drive->volume_monitor), (gpointer) &(drive->volume_monitor));
  
  if (G_OBJECT_CLASS (g_hal_drive_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_hal_drive_parent_class)->finalize) (object);
}

static void
g_hal_drive_class_init (GHalDriveClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_hal_drive_finalize;
}

static void
g_hal_drive_init (GHalDrive *hal_drive)
{
}

static char *
_drive_get_description (HalDevice *d)
{
  char *s = NULL;
  const char *drive_type;
  const char *drive_bus;
  const char *name_from_hal;

  drive_type = hal_device_get_property_string (d, "storage.drive_type");
  drive_bus = hal_device_get_property_string (d, "storage.bus");
  name_from_hal = hal_device_get_property_string (d, "info.desktop.name");

  
  if (strlen (name_from_hal) > 0)
    {
      s = g_strdup (name_from_hal);
    }
  else if (strcmp (drive_type, "cdrom") == 0)
    {
      const char *first;
      const char *second;
      
      first = _("CD-ROM");
      if (hal_device_get_property_bool (d, "storage.cdrom.cdr"))
        first = _("CD-R");
      if (hal_device_get_property_bool (d, "storage.cdrom.cdrw"))
        first = _("CD-RW");
      
      second = NULL;
      if (hal_device_get_property_bool (d, "storage.cdrom.dvd"))
        second = _("DVD-ROM");
      if (hal_device_get_property_bool (d, "storage.cdrom.dvdplusr"))
        second = _("DVD+R");
      if (hal_device_get_property_bool (d, "storage.cdrom.dvdplusrw"))
        second = _("DVD+RW");
      if (hal_device_get_property_bool (d, "storage.cdrom.dvdr"))
        second = _("DVD-R");
      if (hal_device_get_property_bool (d, "storage.cdrom.dvdrw"))
        second = _("DVD-RW");
      if (hal_device_get_property_bool (d, "storage.cdrom.dvdram"))
        second = _("DVD-RAM");
      if ((hal_device_get_property_bool (d, "storage.cdrom.dvdr")) &&
          (hal_device_get_property_bool (d, "storage.cdrom.dvdplusr")))
        second = _("DVD\xc2\xb1R");
      if (hal_device_get_property_bool (d, "storage.cdrom.dvdrw") &&
          hal_device_get_property_bool (d, "storage.cdrom.dvdplusrw"))
        second = _("DVD\xc2\xb1RW");
      if (hal_device_get_property_bool (d, "storage.cdrom.hddvd"))
        second = _("HDDVD");
      if (hal_device_get_property_bool (d, "storage.cdrom.hddvdr"))
        second = _("HDDVD-r");
      if (hal_device_get_property_bool (d, "storage.cdrom.hddvdrw"))
        second = _("HDDVD-RW");
      if (hal_device_get_property_bool (d, "storage.cdrom.bd"))
        second = _("Blu-ray");
      if (hal_device_get_property_bool (d, "storage.cdrom.bdr"))
        second = _("Blu-ray-R");
      if (hal_device_get_property_bool (d, "storage.cdrom.bdre"))
        second = _("Blu-ray-RE");
      
      if (second != NULL)
        {
          /* translators: This wis something like "CD-ROM/DVD Drive" or
             "CD-RW/Blue-ray Drive" depending on the properties of the drive */
          s = g_strdup_printf (_("%s/%s Drive"), first, second);
        }
      else
        {
          /* translators: This wis something like "CD-ROM Drive" or "CD-RW Drive
             depending on the properties of the drive */
          s = g_strdup_printf (_("%s Drive"), first);
        }
    } 
  else if (strcmp (drive_type, "floppy") == 0)
    s = g_strdup (_("Floppy Drive"));
  else if (strcmp (drive_type, "disk") == 0)
    {
      if (drive_bus != NULL)
        {
          if (strcmp (drive_bus, "linux_raid") == 0)
            s = g_strdup (_("Software RAID Drive"));
          if (strcmp (drive_bus, "usb") == 0)
            s = g_strdup (_("USB Drive"));
          if (strcmp (drive_bus, "ide") == 0)
            s = g_strdup (_("ATA Drive"));
          if (strcmp (drive_bus, "scsi") == 0)
            s = g_strdup (_("SCSI Drive"));
          if (strcmp (drive_bus, "ieee1394") == 0)
            s = g_strdup (_("FireWire Drive"));
        } 
    } 
  else if (strcmp (drive_type, "tape") == 0)
    s = g_strdup (_("Tape Drive"));
  else if (strcmp (drive_type, "compact_flash") == 0)
    s = g_strdup (_("CompactFlash Drive"));
  else if (strcmp (drive_type, "memory_stick") == 0)
    s = g_strdup (_("MemoryStick Drive"));
  else if (strcmp (drive_type, "smart_media") == 0)
    s = g_strdup (_("SmartMedia Drive"));
  else if (strcmp (drive_type, "sd_mmc") == 0)
    s = g_strdup (_("SD/MMC Drive"));
  else if (strcmp (drive_type, "zip") == 0)
    s = g_strdup (_("Zip Drive"));
  else if (strcmp (drive_type, "jaz") == 0)
    s = g_strdup (_("Jaz Drive"));
  else if (strcmp (drive_type, "flashkey") == 0)
    s = g_strdup (_("Thumb Drive"));

  if (s == NULL)
    s = g_strdup (_("Mass Storage Drive"));

  return s;
}

char *
_drive_get_icon (HalDevice *d)
{
  char *s = NULL;
  const char *drive_type;
  const char *drive_bus;
  const char *icon_from_hal;
  gboolean is_audio_player;

  drive_type = hal_device_get_property_string (d, "storage.drive_type");
  drive_bus = hal_device_get_property_string (d, "storage.bus");
  is_audio_player = hal_device_has_capability (d, "portable_audio_player");
  icon_from_hal = hal_device_get_property_string (d, "storage.icon.drive");

  if (strlen (icon_from_hal) > 0)
    s = g_strdup (icon_from_hal);
  else if (is_audio_player)
    s = g_strdup ("multimedia-player");
  else if (strcmp (drive_type, "disk") == 0)
    {
      if (strcmp (drive_bus, "ide") == 0)
        s = g_strdup ("drive-removable-media-ata");
      else if (strcmp (drive_bus, "scsi") == 0)
        s = g_strdup ("drive-removable-media-scsi");
      else if (strcmp (drive_bus, "ieee1394") == 0)
        s = g_strdup ("drive-removable-media-ieee1394");
      else if (strcmp (drive_bus, "usb") == 0)
        s = g_strdup ("drive-removable-media-usb");
      else
        s = g_strdup ("drive-removable-media");
    }
  else if (strcmp (drive_type, "cdrom") == 0)
    {
      /* TODO: maybe there's a better heuristic than this */
      if (hal_device_get_property_int (d, "storage.cdrom.write_speed") > 0)
        s = g_strdup ("drive-optical-recorder");
      else
        s = g_strdup ("drive-optical");
    }
  else if (strcmp (drive_type, "floppy") == 0)
    s = g_strdup ("drive-removable-media-floppy");
  else if (strcmp (drive_type, "tape") == 0)
    s = g_strdup ("drive-removable-media-tape");
  else if (strcmp (drive_type, "compact_flash") == 0)
    s = g_strdup ("drive-removable-media-flash-cf");
  else if (strcmp (drive_type, "memory_stick") == 0)
    s = g_strdup ("drive-removable-media-flash-ms");
  else if (strcmp (drive_type, "smart_media") == 0)
    s = g_strdup ("drive-removable-media-flash-sm");
  else if (strcmp (drive_type, "sd_mmc") == 0)
    s = g_strdup ("drive-removable-media-flash-sd");

  if (s == NULL)
    s = g_strdup ("drive-removable-media");
  
  return s;
}

static void
_do_update_from_hal (GHalDrive *d)
{
  d->name = _drive_get_description (d->device);
  d->icon = _drive_get_icon (d->device);
  
  d->uses_removable_media = hal_device_get_property_bool (d->device, "storage.removable");
  if (d->uses_removable_media)
    {
      d->has_media = hal_device_get_property_bool (d->device, "storage.removable.media_available");
      d->is_media_check_automatic = hal_device_get_property_bool (d->device, "storage.media_check_enabled");
      d->can_poll_for_media = hal_device_has_interface (d->device, "org.freedesktop.Hal.Device.Storage.Removable");
      d->can_eject = hal_device_get_property_bool (d->device, "storage.requires_eject");
    }
  else
    {
      d->has_media = TRUE;
      d->is_media_check_automatic = FALSE;
      d->can_poll_for_media = FALSE;
      d->can_eject = FALSE;
    }
}

static void
emit_drive_changed (GHalDrive *drive)
{
  g_signal_emit_by_name (drive, "changed");
  if (drive->volume_monitor != NULL)
    g_signal_emit_by_name (drive->volume_monitor, "drive_changed", drive);
}

static void
_update_from_hal (GHalDrive *d, gboolean emit_changed)
{
  char *old_name;
  char *old_icon;
  gboolean old_uses_removable_media;
  gboolean old_has_media;
  gboolean old_is_media_check_automatic;
  gboolean old_can_poll_for_media;
  gboolean old_can_eject;

  old_name = g_strdup (d->name);
  old_icon = g_strdup (d->icon);
  old_uses_removable_media = d->uses_removable_media;
  old_has_media = d->has_media;
  old_is_media_check_automatic = d->is_media_check_automatic;
  old_can_poll_for_media = d->can_poll_for_media;
  old_can_eject = d->can_eject;

  g_free (d->name);
  g_free (d->icon);
  _do_update_from_hal (d);

  if (emit_changed &&
      (old_uses_removable_media != d->uses_removable_media ||
       old_has_media != d->has_media ||
       old_is_media_check_automatic != d->is_media_check_automatic ||
       old_can_poll_for_media != d->can_poll_for_media ||
       old_can_eject != d->can_eject ||
       old_name == NULL || 
       old_icon == NULL ||
       strcmp (old_name, d->name) != 0 ||
       strcmp (old_icon, d->icon) != 0))
    emit_drive_changed (d);
  
  g_free (old_name);
  g_free (old_icon);
}

static void
hal_condition (HalDevice *device, const char *name, const char *detail, gpointer user_data)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (user_data);

  if (strcmp (name, "EjectPressed") == 0)
    {
      g_signal_emit_by_name (hal_drive, "eject-button");
      if (hal_drive->volume_monitor != NULL)
        g_signal_emit_by_name (hal_drive->volume_monitor, "drive-eject-button", hal_drive);
    }

}

static void
hal_changed (HalDevice *device, const char *key, gpointer user_data)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (user_data);
  
  /*g_warning ("volhal modifying %s (property %s changed)", hal_drive->device_path, key);*/
  _update_from_hal (hal_drive, TRUE);
}

GHalDrive *
g_hal_drive_new (GVolumeMonitor       *volume_monitor,
                 HalDevice            *device,
                 HalPool              *pool)
{
  GHalDrive *drive;

  drive = g_object_new (G_TYPE_HAL_DRIVE, NULL);
  drive->volume_monitor = volume_monitor;
  g_object_add_weak_pointer (G_OBJECT (volume_monitor), (gpointer) &(drive->volume_monitor));
  drive->device_path = g_strdup (hal_device_get_property_string (device, "block.device"));
  drive->device = g_object_ref (device);
  drive->pool = g_object_ref (pool);

  drive->name = g_strdup_printf ("Drive for %s", drive->device_path);
  drive->icon = g_strdup_printf ("drive-removable-media");

  g_signal_connect_object (device, "hal_property_changed", (GCallback) hal_changed, drive, 0);
  g_signal_connect_object (device, "hal_condition", (GCallback) hal_condition, drive, 0);

  _update_from_hal (drive, FALSE);

  return drive;
}

void 
g_hal_drive_disconnected (GHalDrive *drive)
{
  GList *l, *volumes;

  volumes = drive->volumes;
  drive->volumes = NULL;
  
  for (l = volumes; l != NULL; l = l->next)
    {
      GHalVolume *volume = l->data;
      g_hal_volume_unset_drive (volume, drive);
    }

  g_list_free (volumes);
}

void 
g_hal_drive_set_volume (GHalDrive *drive, 
                        GHalVolume *volume)
{

  if (g_list_find (drive->volumes, volume) == NULL)
    {
      drive->volumes = g_list_prepend (drive->volumes, volume);
      emit_drive_changed (drive);
    }
}

void 
g_hal_drive_unset_volume (GHalDrive *drive, 
                          GHalVolume *volume)
{
  GList *l;

  l = g_list_find (drive->volumes, volume);
  if (l != NULL)
    {
      drive->volumes = g_list_delete_link (drive->volumes, l);

      emit_drive_changed (drive);
    }
}

gboolean 
g_hal_drive_has_udi (GHalDrive *drive, const char *udi)
{
  gboolean res;
  
  res = strcmp (udi, hal_device_get_udi (drive->device)) == 0;

  return res;
}

static GIcon *
g_hal_drive_get_icon (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  GIcon *icon;

  icon = g_themed_icon_new_with_default_fallbacks (hal_drive->icon);
  
  return icon; 
}

static char *
g_hal_drive_get_name (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  char *name;

  name = g_strdup (hal_drive->name);
  
  return name;
}

static GList *
g_hal_drive_get_volumes (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  GList *l;

  l = g_list_copy (hal_drive->volumes);
  g_list_foreach (l, (GFunc) g_object_ref, NULL);

  return l;
}

static gboolean
g_hal_drive_has_volumes (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);

  return hal_drive->volumes != NULL;
}

static gboolean
g_hal_drive_is_media_removable (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);

  return hal_drive->uses_removable_media;
}

static gboolean
g_hal_drive_has_media (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);

  return hal_drive->has_media;
}

static gboolean
g_hal_drive_is_media_check_automatic (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);

  return hal_drive->is_media_check_automatic;
}

static gboolean
g_hal_drive_can_eject (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  
  return hal_drive->can_eject;
}

static gboolean
g_hal_drive_can_poll_for_media (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  
  return hal_drive->can_poll_for_media;
}

typedef struct {
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
} SpawnOp;

static void 
spawn_cb (GPid pid, gint status, gpointer user_data)
{
  SpawnOp *data = user_data;
  GSimpleAsyncResult *simple;


  if (WEXITSTATUS (status) != 0)
    {
      GError *error;
      error = g_error_new_literal (G_IO_ERROR, 
                                   G_IO_ERROR_FAILED_HANDLED,
                                   "You are not supposed to show G_IO_ERROR_FAILED_HANDLED in the UI");
      simple = g_simple_async_result_new_from_error (data->object,
                                                     data->callback,
                                                     data->user_data,
                                                     error);
      g_error_free (error);
    }
  else
    {
      simple = g_simple_async_result_new (data->object,
                                          data->callback,
                                          data->user_data,
                                          NULL);
    }
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
  g_object_unref (data->object);
  g_free (data);
}

static void
g_hal_drive_eject_do (GDrive              *drive,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  SpawnOp *data;
  GPid child_pid;
  GError *error;
  char *argv[] = {"gnome-mount", "-e", "-b", "-d", NULL, NULL};

  argv[4] = g_strdup (hal_drive->device_path);
  
  data = g_new0 (SpawnOp, 1);
  data->object = g_object_ref (drive);
  data->callback = callback;
  data->user_data = user_data;
  data->cancellable = cancellable;

  error = NULL;
  if (!g_spawn_async (NULL,         /* working dir */
                      argv,
                      NULL,         /* envp */
                      G_SPAWN_DO_NOT_REAP_CHILD|G_SPAWN_SEARCH_PATH,
                      NULL,         /* child_setup */
                      NULL,         /* user_data for child_setup */
                      &child_pid,
                      &error))
    {
      GSimpleAsyncResult *simple;

      simple = g_simple_async_result_new_from_error (data->object,
                                                     data->callback,
                                                     data->user_data,
                                                     error);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      g_object_unref (drive);
      g_error_free (error);
      g_free (data);
    }
  else
    g_child_watch_add (child_pid, spawn_cb, data);

  g_free (argv[4]);
}


typedef struct {
  GDrive *drive;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GMountOperation *mount_operation;
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

      g_hal_drive_eject_do (data->drive,
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

      g_mount_unmount_with_operation (mount,
                                      data->flags,
                                      data->mount_operation,
                                      data->cancellable,
                                      _eject_unmount_mounts_cb,
                                      data);
    }
}

static void
g_hal_drive_eject_with_operation (GDrive              *drive,
                                  GMountUnmountFlags   flags,
                                  GMountOperation     *mount_operation,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  UnmountMountsOp *data;
  GList *l;

  /* first we need to go through all the volumes and unmount their assoicated mounts (if any) */

  data = g_new0 (UnmountMountsOp, 1);
  data->drive = g_object_ref (drive);
  data->mount_operation = mount_operation;
  data->cancellable = cancellable;
  data->callback = callback;
  data->user_data = user_data;
  data->flags = flags;

  for (l = hal_drive->volumes; l != NULL; l = l->next)
    {
      GHalVolume *volume = l->data;
      GMount *mount; /* the mount may be foreign; cannot assume GHalMount */

      mount = g_volume_get_mount (G_VOLUME (volume));
      if (mount != NULL && g_mount_can_unmount (mount))
        data->pending_mounts = g_list_prepend (data->pending_mounts, g_object_ref (mount));
    }

  _eject_unmount_mounts (data);
}

static gboolean
g_hal_drive_eject_with_operation_finish (GDrive        *drive,
                                         GAsyncResult  *result,
                                         GError       **error)
{
  return TRUE;
}

static void
g_hal_drive_eject (GDrive              *drive,
                   GMountUnmountFlags   flags,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  g_hal_drive_eject (drive, flags, cancellable, callback, user_data);
}

static gboolean
g_hal_drive_eject_finish (GDrive        *drive,
                          GAsyncResult  *result,
                          GError       **error)
{
  return g_hal_drive_eject_with_operation_finish (drive, result, error);
}

typedef struct {
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
} PollOp;

static void
poll_for_media_cb (DBusPendingCall *pending_call, void *user_data)
{
  PollOp *data = (PollOp *) user_data;
  GSimpleAsyncResult *simple;
  DBusMessage *reply;

  reply = dbus_pending_call_steal_reply (pending_call);

  if (dbus_message_get_type (reply) == DBUS_MESSAGE_TYPE_ERROR)
    {
      GError *error;
      DBusError dbus_error;

      dbus_error_init (&dbus_error);
      dbus_set_error_from_message (&dbus_error, reply);
      error = g_error_new (G_IO_ERROR,
                           G_IO_ERROR_FAILED,
                           "Cannot invoke CheckForMedia on HAL: %s: %s", dbus_error.name, dbus_error.message);
      simple = g_simple_async_result_new_from_error (data->object,
                                                     data->callback,
                                                     data->user_data,
                                                     error);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      g_error_free (error);
      dbus_error_free (&dbus_error);
      goto out;
    }

  /* TODO: parse reply and extract result?
   * (the result is whether the media availability state changed)
   */

  simple = g_simple_async_result_new (data->object,
                                      data->callback,
                                      data->user_data,
                                      NULL);
  g_simple_async_result_complete (simple);
  g_object_unref (simple);

 out:
  g_object_unref (data->object);
  dbus_message_unref (reply);
  dbus_pending_call_unref (pending_call);
}


static void
g_hal_drive_poll_for_media (GDrive              *drive,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  DBusConnection *con;
  DBusMessage *msg;
  DBusPendingCall *pending_call;
  PollOp *data;

  data = g_new0 (PollOp, 1);
  data->object = g_object_ref (drive);
  data->callback = callback;
  data->user_data = user_data;
  data->cancellable = cancellable;

  /*g_warning ("Rescanning udi %s", hal_device_get_udi (hal_drive->device));*/

  con = hal_pool_get_dbus_connection (hal_drive->pool);
  msg = dbus_message_new_method_call ("org.freedesktop.Hal",
                                      hal_device_get_udi (hal_drive->device),
                                      "org.freedesktop.Hal.Device.Storage.Removable",
                                      "CheckForMedia");

  if (!dbus_connection_send_with_reply (con, msg, &pending_call, -1))
    {
      GError *error;
      GSimpleAsyncResult *simple;
      error = g_error_new_literal (G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   "Cannot invoke CheckForMedia on HAL");
      simple = g_simple_async_result_new_from_error (data->object,
                                                     data->callback,
                                                     data->user_data,
                                                     error);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      g_error_free (error);
      g_object_unref (data->object);
      g_free (data);
    }
  else
    dbus_pending_call_set_notify (pending_call,
                                  poll_for_media_cb,
                                  data,
                                  (DBusFreeFunction) g_free);

  dbus_message_unref (msg);
}

static gboolean
g_hal_drive_poll_for_media_finish (GDrive        *drive,
                                   GAsyncResult  *result,
                                   GError       **error)
{
  /*g_warning ("poll finish");*/
  return TRUE;
}

static char *
g_hal_drive_get_identifier (GDrive              *drive,
                             const char          *kind)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  char *res;

  res = NULL;

  if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_HAL_UDI) == 0)
    res = g_strdup (hal_device_get_udi (hal_drive->device));
  
  if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE) == 0)
    res = g_strdup (hal_drive->device_path);
  
  return res;
}

static char **
g_hal_drive_enumerate_identifiers (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  GPtrArray *res;

  res = g_ptr_array_new ();

  g_ptr_array_add (res,
                   g_strdup (G_VOLUME_IDENTIFIER_KIND_HAL_UDI));

  if (hal_drive->device_path && *hal_drive->device_path != 0)
    g_ptr_array_add (res,
                     g_strdup (G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));
    
  
  /* Null-terminate */
  g_ptr_array_add (res, NULL);
  
  return (char **)g_ptr_array_free (res, FALSE);
}

static void
g_hal_drive_drive_iface_init (GDriveIface *iface)
{
  iface->get_name = g_hal_drive_get_name;
  iface->get_icon = g_hal_drive_get_icon;
  iface->has_volumes = g_hal_drive_has_volumes;
  iface->get_volumes = g_hal_drive_get_volumes;
  iface->is_media_removable = g_hal_drive_is_media_removable;
  iface->has_media = g_hal_drive_has_media;
  iface->is_media_check_automatic = g_hal_drive_is_media_check_automatic;
  iface->can_eject = g_hal_drive_can_eject;
  iface->can_poll_for_media = g_hal_drive_can_poll_for_media;
  iface->eject = g_hal_drive_eject;
  iface->eject_finish = g_hal_drive_eject_finish;
  iface->eject_with_operation = g_hal_drive_eject_with_operation;
  iface->eject_with_operation_finish = g_hal_drive_eject_with_operation_finish;
  iface->poll_for_media = g_hal_drive_poll_for_media;
  iface->poll_for_media_finish = g_hal_drive_poll_for_media_finish;
  iface->get_identifier = g_hal_drive_get_identifier;
  iface->enumerate_identifiers = g_hal_drive_enumerate_identifiers;
}
