/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
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
#include <gio/gio.h>

#include <gvfsmountinfo.h>

#include "ghalvolumemonitor.h"
#include "ghalmount.h"
#include "ghalvolume.h"

#include "hal-utils.h"

struct _GHalMount {
  GObject parent;

  GVolumeMonitor *volume_monitor; /* owned by volume monitor */
  GHalVolume *volume;             /* owned by volume monitor */

  char *name;
  GIcon *icon;
  char *device_path;
  char *mount_path;

  char *uuid;

  char *override_name;
  GIcon *override_icon;
  GFile *override_root;
  gboolean cannot_unmount;

  HalDevice *device;
  HalDevice *drive_device;

  GIcon *autorun_icon;
  gboolean searched_for_autorun;

  gchar *xdg_volume_info_name;
  GIcon *xdg_volume_info_icon;
  gboolean searched_for_xdg_volume_info;
};

static GFile *get_root (GHalMount *hal_mount);

static void update_from_hal (GHalMount *m, gboolean emit_changed);

static void g_hal_mount_mount_iface_init (GMountIface *iface);

G_DEFINE_TYPE_EXTENDED (GHalMount, g_hal_mount, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT,
                                               g_hal_mount_mount_iface_init))

static void
g_hal_mount_finalize (GObject *object)
{
  GHalMount *mount;
  
  mount = G_HAL_MOUNT (object);

  if (mount->volume != NULL)
    g_hal_volume_unset_mount (mount->volume, mount);

  g_free (mount->device_path);
  g_free (mount->mount_path);
  g_free (mount->uuid);

  if (mount->device != NULL)
    g_object_unref (mount->device);
  if (mount->drive_device != NULL)
    g_object_unref (mount->drive_device);

  g_free (mount->name);
  if (mount->icon != NULL)
    g_object_unref (mount->icon);

  g_free (mount->override_name);
  if (mount->override_icon != NULL)
    g_object_unref (mount->override_icon);

  if (mount->override_root != NULL)
    g_object_unref (mount->override_root);

  if (mount->autorun_icon != NULL)
    g_object_unref (mount->autorun_icon);

  g_free (mount->xdg_volume_info_name);
  if (mount->xdg_volume_info_icon != NULL)
    g_object_unref (mount->xdg_volume_info_icon);

  if (mount->volume_monitor != NULL)
    g_object_remove_weak_pointer (G_OBJECT (mount->volume_monitor), (gpointer) &(mount->volume_monitor));
  
  if (G_OBJECT_CLASS (g_hal_mount_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_hal_mount_parent_class)->finalize) (object);
}

static void
g_hal_mount_class_init (GHalMountClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_hal_mount_finalize;
}

static void
g_hal_mount_init (GHalMount *hal_mount)
{
}

static void
emit_mount_changed (GHalMount *mount)
{
  g_signal_emit_by_name (mount, "changed");
  if (mount->volume_monitor != NULL)
    g_signal_emit_by_name (mount->volume_monitor, "mount_changed", mount);
}

#define KILOBYTE_FACTOR 1000.0
#define MEGABYTE_FACTOR (1000.0 * 1000.0)
#define GIGABYTE_FACTOR (1000.0 * 1000.0 * 1000.0)

static char *
format_size_for_display (guint64 size)
{
  char *str;
  gdouble displayed_size;
  
  if (size < MEGABYTE_FACTOR)
    {
      displayed_size = (double) size / KILOBYTE_FACTOR;
      str = g_strdup_printf (_("%.1f kB"), displayed_size);
    } 
  else if (size < GIGABYTE_FACTOR)
    {
      displayed_size = (double) size / MEGABYTE_FACTOR;
      str = g_strdup_printf (_("%.1f MB"), displayed_size);
    } 
  else 
    {
      displayed_size = (double) size / GIGABYTE_FACTOR;
      str = g_strdup_printf (_("%.1f GB"), displayed_size);
    }
  
  return str;
}

static void
got_autorun_info_cb (GObject      *source_object,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  GHalMount *mount = G_HAL_MOUNT (user_data);

  mount->autorun_icon = g_vfs_mount_info_query_autorun_info_finish (G_FILE (source_object),
                                                                    res,
                                                                    NULL);

  update_from_hal (mount, TRUE);

  g_object_unref (mount);
}

static void
got_xdg_volume_info_cb (GObject      *source_object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  GHalMount *mount = G_HAL_MOUNT (user_data);

  mount->xdg_volume_info_icon = g_vfs_mount_info_query_xdg_volume_info_finish (G_FILE (source_object),
                                                                               res,
                                                                               &(mount->xdg_volume_info_name),
                                                                               NULL);
  update_from_hal (mount, TRUE);

  g_object_unref (mount);
}

static void
do_update_from_hal (GHalMount *m)
{
  HalDevice *volume;
  HalDevice *drive;
  char *name;
  const char *icon_name;
  const char *icon_name_fallback;
  const char *drive_type;
  const char *drive_bus;
  gboolean drive_uses_removable_media;
  const char *volume_fs_label;
  guint64 volume_size;
  gboolean volume_is_disc;
  gboolean volume_disc_has_audio;
  gboolean volume_disc_has_data;
  const char *volume_disc_type;
  gboolean volume_disc_is_blank;
  gboolean is_audio_player;
  const char *icon_from_hal;
  const char *volume_icon_from_hal;
  const char *name_from_hal;
  const char *volume_name_from_hal;
  gboolean is_crypto;
  gboolean is_crypto_cleartext;

  volume = m->device;
  drive = m->drive_device;
  
  drive_type = hal_device_get_property_string (drive, "storage.drive_type");
  drive_bus = hal_device_get_property_string (drive, "storage.bus");
  drive_uses_removable_media = hal_device_get_property_bool (drive, "storage.removable");
  volume_fs_label = hal_device_get_property_string (volume, "volume.label");
  volume_size = hal_device_get_property_uint64 (volume, "volume.size");
  volume_is_disc = hal_device_get_property_bool (volume, "volume.is_disc");
  volume_disc_has_audio = hal_device_get_property_bool (volume, "volume.disc.has_audio");
  volume_disc_has_data = hal_device_get_property_bool (volume, "volume.disc.has_data");
  volume_disc_is_blank = hal_device_get_property_bool (volume, "volume.disc.is_blank");
  volume_disc_type = hal_device_get_property_string (volume, "volume.disc.type");
  is_audio_player = hal_device_has_capability (drive, "portable_audio_player");
  icon_from_hal = hal_device_get_property_string (drive, "storage.icon.drive");
  volume_icon_from_hal = hal_device_get_property_string (volume, "storage.icon.volume");
  name_from_hal = hal_device_get_property_string (drive, "info.desktop.name");
  volume_name_from_hal = hal_device_get_property_string (volume, "info.desktop.name");

  is_crypto = FALSE;
  is_crypto_cleartext = FALSE;
  if (strcmp (hal_device_get_property_string (volume, "volume.fsusage"), "crypto") == 0)
      is_crypto = TRUE;

  if (strlen (hal_device_get_property_string (volume, "volume.crypto_luks.clear.backing_volume")) > 0)
      is_crypto_cleartext = TRUE;

  /*g_warning ("drive_type='%s'", drive_type); */
  /*g_warning ("drive_bus='%s'", drive_bus); */
  /*g_warning ("drive_uses_removable_media=%d", drive_uses_removable_media); */

  icon_name_fallback = NULL;

  if (strlen (volume_icon_from_hal) > 0)
    icon_name = volume_icon_from_hal;
  else if (strlen (icon_from_hal) > 0)
    icon_name = icon_from_hal;
  else if (is_audio_player)
    icon_name = "multimedia-player";
  else if (strcmp (drive_type, "disk") == 0)
    {
      if (strcmp (drive_bus, "ide") == 0)
        icon_name = "drive-harddisk-ata";
      else if (strcmp (drive_bus, "scsi") == 0)
        icon_name = "drive-harddisk-scsi";
      else if (strcmp (drive_bus, "ieee1394") == 0)
        icon_name = "drive-harddisk-ieee1394";
      else if (strcmp (drive_bus, "usb") == 0)
        icon_name = "drive-harddisk-usb";
      else
        icon_name = "drive-harddisk";
    }
  else if (strcmp (drive_type, "cdrom") == 0)
    icon_name = get_disc_icon (volume_disc_type);
  else if (strcmp (drive_type, "floppy") == 0)
    icon_name = "media-floppy";
  else if (strcmp (drive_type, "tape") == 0)
    icon_name = "media-tape";
  else if (strcmp (drive_type, "compact_flash") == 0)
    icon_name = "media-flash-cf";
  else if (strcmp (drive_type, "memory_stick") == 0)
    icon_name = "media-flash-ms";
  else if (strcmp (drive_type, "smart_media") == 0)
    icon_name = "media-flash-sm";
  else if (strcmp (drive_type, "sd_mmc") == 0)
    icon_name = "media-flash-sd";
  else
    icon_name = "drive-harddisk";

  /* Create default fallbacks for the icon_name by default
   * with get_themed_icon_with_fallbacks () */
  icon_name_fallback = icon_name;

  /* Note: we are not chaning the default fallbacks
   * so we get all the fallbacks if the media-encrytped
   * icon is not there */
  if (is_crypto || is_crypto_cleartext)
    icon_name = "media-encrypted";

  if (strlen (volume_name_from_hal) > 0)
    name = g_strdup (volume_name_from_hal);
  else if (strlen (name_from_hal) > 0)
    name = g_strdup (name_from_hal);
  else if (volume_fs_label != NULL && strlen (volume_fs_label) > 0)
    name = g_strdup (volume_fs_label);
  else if (volume_is_disc)
    {
      if (volume_disc_has_audio)
        {
          if (volume_disc_has_data)
            name = g_strdup (_("Mixed Audio/Data Disc"));
          else
            name = g_strdup (_("Audio Disc"));
        }
      else
        name = g_strdup (get_disc_name (volume_disc_type, volume_disc_is_blank));
    }
  else
    {
      char *size;

      size = format_size_for_display (volume_size);  
      /* Translators: %s is the size of the mount (e.g. 512 MB) */
      name = g_strdup_printf (_("%s Media"), size);
      g_free (size);
    }

  /* order of preference : xdg, override, probed */
  if (m->xdg_volume_info_name != NULL)
    {
      m->name = g_strdup (m->xdg_volume_info_name);
      g_free (name);
    }
  else if (m->override_name != NULL)
    {
      m->name = g_strdup (m->override_name);
      g_free (name);
    }
  else
    m->name = name;

  /* order of preference: xdg, autorun, override, probed */
  if (m->xdg_volume_info_icon != NULL)
    m->icon = g_object_ref (m->xdg_volume_info_icon);
  else if (m->autorun_icon != NULL)
    m->icon = g_object_ref (m->autorun_icon);
  else if (m->override_icon != NULL)
    m->icon = g_object_ref (m->override_icon);
  else
    m->icon = get_themed_icon_with_fallbacks (icon_name,
                                              icon_name_fallback);

  /* search for .xdg-volume-info */
  if (!m->searched_for_xdg_volume_info)
    {
      GFile *root;
      root = get_root (m);
      m->searched_for_xdg_volume_info = TRUE;
      g_vfs_mount_info_query_xdg_volume_info (root,
                                              NULL,
                                              got_xdg_volume_info_cb,
                                              g_object_ref (m));
      g_object_unref (root);
    }

  /* search for autorun.inf */
  if (!m->searched_for_autorun)
    {
      GFile *root;
      root = get_root (m);
      m->searched_for_autorun = TRUE;
      g_vfs_mount_info_query_autorun_info (root,
                                           NULL,
                                           got_autorun_info_cb,
                                           g_object_ref (m));
      g_object_unref (root);
    }
}


static void
update_from_hal (GHalMount *m, gboolean emit_changed)
{
  char *old_name;
  GIcon *old_icon;

  old_name = g_strdup (m->name);
  old_icon = m->icon != NULL ? g_object_ref (m->icon) : NULL;

  g_free (m->name);
  if (m->icon != NULL)
    g_object_unref (m->icon);
  do_update_from_hal (m);

  if (emit_changed)
    {
      if (old_name == NULL || 
          old_icon == NULL ||
          strcmp (old_name, m->name) != 0 ||
          (! g_icon_equal (old_icon, m->icon)))
        emit_mount_changed (m);
    }
  g_free (old_name);
  if (old_icon != NULL)
    g_object_unref (old_icon);
}

static void
hal_changed (HalDevice *device, const char *key, gpointer user_data)
{
  GHalMount *hal_mount = G_HAL_MOUNT (user_data);
  
  /*g_warning ("mounthal modifying %s (property %s changed)", hal_mount->device_path, key); */
  update_from_hal (hal_mount, TRUE);
}

static void
compute_uuid (GHalMount *mount)
{
  const char *fs_uuid;
  const char *fs_label;

  /* use the FS uuid before falling back to the FS label */

  fs_uuid = hal_device_get_property_string (mount->device, "volume.uuid");
  fs_label = hal_device_get_property_string (mount->device, "volume.label");

  if (strlen (fs_uuid) == 0)
    {
      if (strlen (fs_label) == 0)
        mount->uuid = NULL;
      else
        mount->uuid = g_strdup (fs_label);
    }
  else
    mount->uuid = g_strdup (fs_uuid);
}


GHalMount *
g_hal_mount_new_for_hal_device    (GVolumeMonitor    *volume_monitor,
                                   HalDevice         *device,
                                   GFile             *override_root,
                                   const char        *override_name,
                                   GIcon             *override_icon,
                                   gboolean           cannot_unmount,
                                   HalPool           *pool,
                                   GHalVolume        *volume)
{
  HalDevice *drive_device;
  const char *storage_udi;
  GHalMount *mount;

  storage_udi = hal_device_get_property_string (device, "block.storage_device");
  if (storage_udi == NULL)
    goto fail;
  
  /* g_warning ("storage_udi = %s", storage_udi); */
  
  drive_device = hal_pool_get_device_by_udi (pool, storage_udi);
  if (drive_device == NULL)
    goto fail;
  
  /* g_warning ("drive_device = %p", drive_device); */
  
  mount = g_object_new (G_TYPE_HAL_MOUNT, NULL);
  mount->volume_monitor = volume_monitor;
  g_object_add_weak_pointer (G_OBJECT (mount->volume_monitor), (gpointer) &(mount->volume_monitor));
  mount->device_path = g_strdup (hal_device_get_property_string (device, "block.device"));
  mount->mount_path = g_strdup ("/");
  mount->device = g_object_ref (device);
  mount->drive_device = g_object_ref (drive_device);
  mount->override_root = override_root != NULL ? g_object_ref (override_root) : NULL;
  mount->override_icon = override_icon != NULL ? g_object_ref (override_icon) : NULL;
  mount->override_name = g_strdup (override_name);
  mount->cannot_unmount = cannot_unmount;
          
  g_signal_connect_object (device, "hal_property_changed", (GCallback) hal_changed, mount, 0);
  g_signal_connect_object (drive_device, "hal_property_changed", (GCallback) hal_changed, mount, 0);

  compute_uuid (mount);
  update_from_hal (mount, FALSE);

  /* need to do this last */
  mount->volume = volume;
  if (volume != NULL)
    g_hal_volume_set_mount (volume, mount);

  return mount;

 fail:
  return NULL;
}

void 
g_hal_mount_override_name (GHalMount *mount, const char *name)
{
  g_free (mount->override_name);

  if (name != NULL)
    mount->override_name = g_strdup (name);
  else
    mount->override_name = NULL;
  
  update_from_hal (mount, TRUE);
}

void
g_hal_mount_override_icon (GHalMount *mount, GIcon *icon)
{
  if (mount->override_icon != NULL)
    g_object_unref (mount->override_icon);

  if (icon != NULL)
    mount->override_icon = g_object_ref (icon);
  else
    mount->override_icon = NULL;

  update_from_hal (mount, TRUE);
}

GHalMount *
g_hal_mount_new (GVolumeMonitor       *volume_monitor,
                 GUnixMountEntry      *mount_entry,
                 HalPool              *pool,
                 GHalVolume           *volume)
{
  HalDevice *device;
  HalDevice *drive_device;
  const char *storage_udi;
  GHalMount *mount;

  /* If no volume for mount - Ignore internal things */
  if (volume == NULL && !g_unix_mount_guess_should_display (mount_entry))
    return NULL;

  mount = g_object_new (G_TYPE_HAL_MOUNT, NULL);
  mount->volume_monitor = volume_monitor;
  g_object_add_weak_pointer (G_OBJECT (volume_monitor), (gpointer) &(mount->volume_monitor));
  mount->device_path = g_strdup (g_unix_mount_get_device_path (mount_entry));
  mount->mount_path = g_strdup (g_unix_mount_get_mount_path (mount_entry));
  mount->device = NULL;
  mount->drive_device = NULL;
  mount->uuid = NULL;

  if (pool != NULL)
    {
      device = hal_pool_get_device_by_capability_and_string (pool,
                                                             "volume",
                                                             "block.device",
                                                             mount->device_path);
      /* g_warning ("device = %p for %s", device, mount->device_path); */
      if (device != NULL)
        {      
          /* g_warning ("udi = %s", hal_device_get_udi (device)); */
          
          storage_udi = hal_device_get_property_string (device, "block.storage_device");
          if (storage_udi == NULL)
            goto not_hal;

          /* g_warning ("storage_udi = %s", storage_udi); */
          
          drive_device = hal_pool_get_device_by_udi (pool, storage_udi);
          if (drive_device == NULL)
            goto not_hal;

          /* g_warning ("drive_device = %p", drive_device); */
          
          mount->device = g_object_ref (device);
          mount->drive_device = g_object_ref (drive_device);
          
          g_signal_connect_object (device, "hal_property_changed", (GCallback) hal_changed, mount, 0);
          g_signal_connect_object (drive_device, "hal_property_changed", (GCallback) hal_changed, mount, 0);
          
          compute_uuid (mount);
          update_from_hal (mount, FALSE);
          
          goto was_hal;
        }
    }

 not_hal:

  if (volume != NULL)
    {
      g_object_unref (mount);
      return NULL;
    }
  
  mount->name = g_unix_mount_guess_name (mount_entry);
  mount->icon = g_unix_mount_guess_icon (mount_entry);

 was_hal:

  /* need to do this last */
  mount->volume = volume;
  if (volume != NULL)
    g_hal_volume_set_mount (volume, mount);

  return mount;
}

void
g_hal_mount_unmounted (GHalMount *mount)
{
  if (mount->volume != NULL)
    {
      g_hal_volume_unset_mount (mount->volume, mount);
      mount->volume = NULL;
      emit_mount_changed (mount);
    }
}

void
g_hal_mount_unset_volume (GHalMount *mount,
                                       GHalVolume  *volume)
{
  if (mount->volume == volume)
    {
      mount->volume = NULL;
      emit_mount_changed (mount);
    }
}

static GFile *
get_root (GHalMount *hal_mount)
{
  if (hal_mount->override_root != NULL)
    return g_object_ref (hal_mount->override_root);
  else
    return g_file_new_for_path (hal_mount->mount_path);
}

static GFile *
g_hal_mount_get_root (GMount *mount)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  
  return get_root (hal_mount);
}

static GIcon *
g_hal_mount_get_icon (GMount *mount)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);

  return g_object_ref (hal_mount->icon);
}

static char *
g_hal_mount_get_uuid (GMount *mount)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);

  return g_strdup (hal_mount->uuid);
}

static char *
g_hal_mount_get_name (GMount *mount)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  
  return g_strdup (hal_mount->name);
}

gboolean
g_hal_mount_has_uuid (GHalMount         *mount,
                       const char        *uuid)
{
  gboolean res;

  res = FALSE;
  if (mount->uuid != NULL)
    res = strcmp (mount->uuid, uuid) == 0;

  return res;
}

gboolean
g_hal_mount_has_mount_path (GHalMount *mount,
                            const char  *mount_path)
{
  return strcmp (mount->mount_path, mount_path) == 0;
}

gboolean
g_hal_mount_has_udi (GHalMount *mount,
                     const char  *udi)
{
  gboolean res;

  res = FALSE;
  if (mount->device != NULL)
    res = strcmp (hal_device_get_udi (mount->device), udi) == 0;

  return res;
}

static GDrive *
g_hal_mount_get_drive (GMount *mount)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  GDrive *drive;

  drive = NULL;
  if (hal_mount->volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (hal_mount->volume));

  return drive;
}

static GVolume *
g_hal_mount_get_volume (GMount *mount)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  GVolume *volume;

  volume = NULL;
  if (hal_mount->volume)
    volume = G_VOLUME (g_object_ref (hal_mount->volume));
  
  return volume;
}

static gboolean
g_hal_mount_can_unmount (GMount *mount)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  gboolean res;

  res = TRUE;
  if (hal_mount->cannot_unmount)
    res = FALSE;

  return res;
}

static gboolean
g_hal_mount_can_eject (GMount *mount)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  GDrive *drive;
  gboolean can_eject;

  can_eject = FALSE;
  if (hal_mount->volume != NULL)
    {
      drive = g_volume_get_drive (G_VOLUME (hal_mount->volume));
      if (drive != NULL)
        can_eject = g_drive_can_eject (drive);
    }

  return can_eject;
}

typedef struct {
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  int error_fd;
  GIOChannel *error_channel;
  guint error_channel_source_id;
  GString *error_string;
  gboolean using_legacy;
  gchar **argv;
} UnmountOp;

static void 
unmount_cb (GPid pid, gint status, gpointer user_data)
{
  UnmountOp *data = user_data;
  GSimpleAsyncResult *simple;

  if (WEXITSTATUS (status) != 0)
    {
      if (data->using_legacy)
        {
          GError *error;
          error = g_error_new_literal (G_IO_ERROR, 
                                       G_IO_ERROR_FAILED,
                                       data->error_string->str);
          simple = g_simple_async_result_new_from_error (data->object,
                                                         data->callback,
                                                         data->user_data,
                                                         error);
          g_error_free (error);
        }
      else
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

  g_source_remove (data->error_channel_source_id);
  g_io_channel_unref (data->error_channel);
  g_string_free (data->error_string, TRUE);
  g_strfreev (data->argv);
  close (data->error_fd);
  g_spawn_close_pid (pid);

  g_object_unref (data->object);
  g_free (data);
}

static gboolean
unmount_read_error (GIOChannel *channel,
                          GIOCondition condition,
                          gpointer user_data)
{
  char *str;
  gsize str_len;
  UnmountOp *data = user_data;

  g_io_channel_read_to_end (channel, &str, &str_len, NULL);
  g_string_append (data->error_string, str);
  g_free (str);
  return TRUE;
}

static gboolean
unmount_do_cb (gpointer user_data)
{
  UnmountOp *data = (UnmountOp *) user_data;
  GPid child_pid;
  GError *error;

  error = NULL;
  if (!g_spawn_async_with_pipes (NULL,         /* working dir */
                                 data->argv,
                                 NULL,         /* envp */
                                 G_SPAWN_DO_NOT_REAP_CHILD|G_SPAWN_SEARCH_PATH,
                                 NULL,         /* child_setup */
                                 NULL,         /* user_data for child_setup */
                                 &child_pid,
                                 NULL,           /* standard_input */
                                 NULL,           /* standard_output */
                                 &(data->error_fd),
                                 &error))
    {
      GSimpleAsyncResult *simple;
      simple = g_simple_async_result_new_from_error (data->object,
                                                     data->callback,
                                                     data->user_data,
                                                     error);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      g_error_free (error);
      g_strfreev (data->argv);
      g_free (data);

      return FALSE;
    }
  data->error_string = g_string_new ("");
  data->error_channel = g_io_channel_unix_new (data->error_fd);
  data->error_channel_source_id = g_io_add_watch (data->error_channel, G_IO_IN, unmount_read_error, data);
  g_child_watch_add (child_pid, unmount_cb, data);

  return FALSE;
}

static void
unmount_do (GMount               *mount,
            GCancellable         *cancellable,
            GAsyncReadyCallback   callback,
            gpointer              user_data,
            char                **argv,
            gboolean              using_legacy)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  UnmountOp *data;

  data = g_new0 (UnmountOp, 1);
  data->object = g_object_ref (mount);
  data->callback = callback;
  data->user_data = user_data;
  data->cancellable = cancellable;
  data->using_legacy = using_legacy;
  data->argv = g_strdupv (argv);

  if (hal_mount->volume_monitor != NULL)
    g_signal_emit_by_name (hal_mount->volume_monitor, "mount-pre-unmount", mount);

  g_timeout_add (500, unmount_do_cb, data);
}

static void
g_hal_mount_unmount_with_operation (GMount              *mount,
                                    GMountUnmountFlags   flags,
                                    GMountOperation     *mount_operation,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  char *argv[] = {"gnome-mount", "-u", "-b", "-d", NULL, NULL};
  gboolean using_legacy = FALSE;
  char *d, *m;

  d = g_strdup (hal_mount->device_path);
  m = g_strdup (hal_mount->mount_path);
  
  if (hal_mount->device != NULL)
    argv[4] = d;
  else
    {
      using_legacy = TRUE;
      argv[0] = "umount";
      argv[1] = m;
      argv[2] = NULL;
    }

  unmount_do (mount, cancellable, callback, user_data, argv, using_legacy);
  g_free (d);
  g_free (m);
}

static gboolean
g_hal_mount_unmount_with_operation_finish (GMount       *mount,
                                           GAsyncResult  *result,
                                           GError       **error)
{
  return TRUE;
}

static void
g_hal_mount_unmount (GMount              *mount,
                     GMountUnmountFlags   flags,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  return g_hal_mount_unmount_with_operation (mount, flags, NULL, cancellable, callback, user_data);
}

static gboolean
g_hal_mount_unmount_finish (GMount        *mount,
                            GAsyncResult  *result,
                            GError       **error)
{
  return g_hal_mount_unmount_with_operation_finish (mount, result, error);
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
g_hal_mount_eject_with_operation (GMount              *mount,
                                  GMountUnmountFlags   flags,
                                  GMountOperation     *mount_operation,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  GDrive *drive;

  drive = NULL;
  if (hal_mount->volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (hal_mount->volume));
  
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
}

static gboolean
g_hal_mount_eject_with_operation_finish (GMount        *mount,
                                         GAsyncResult  *result,
                                         GError       **error)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  GDrive *drive;
  gboolean res;

  res = TRUE;
  
  drive = NULL;
  if (hal_mount->volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (hal_mount->volume));
  
  if (drive != NULL)
    {
      res = g_drive_eject_with_operation_finish (drive, result, error);
      g_object_unref (drive);
    }
  return res;
}

static void
g_hal_mount_eject (GMount              *mount,
                   GMountUnmountFlags   flags,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  return g_hal_mount_eject_with_operation (mount, flags, NULL, cancellable, callback, user_data);
}

static gboolean
g_hal_mount_eject_finish (GMount        *mount,
                          GAsyncResult  *result,
                          GError       **error)
{
  return g_hal_mount_eject_with_operation_finish (mount, result, error);
}

/* TODO: handle force_rescan */
static char **
g_hal_mount_guess_content_type_sync (GMount              *mount,
                                     gboolean             force_rescan,
                                     GCancellable        *cancellable,
                                     GError             **error)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  const char *disc_type;
  char **x_content_types;
  GFile *root;
  GPtrArray *p;
  char **result;
  int n;
  char **caps;
  char *uri;

  p = g_ptr_array_new ();

  root = get_root (hal_mount);
  uri = g_file_get_uri (root);
  if (g_str_has_prefix (uri, "burn://"))
    {
      /* doesn't make sense to probe burn:/// - look at the disc type instead */
      if (hal_mount->device != NULL)
        {
          disc_type = hal_device_get_property_string (hal_mount->device, "volume.disc.type");
          if (disc_type != NULL)
            {
              if (g_str_has_prefix (disc_type, "dvd"))
                g_ptr_array_add (p, g_strdup ("x-content/blank-dvd"));
              else if (g_str_has_prefix (disc_type, "hddvd"))
                g_ptr_array_add (p, g_strdup ("x-content/blank-hddvd"));
              else if (g_str_has_prefix (disc_type, "bd"))
                g_ptr_array_add (p, g_strdup ("x-content/blank-bd"));
              else
                g_ptr_array_add (p, g_strdup ("x-content/blank-cd")); /* assume CD */
            }
        }
    }
  else
    {
      /* sniff content type */
      x_content_types = g_content_type_guess_for_tree (root);
      if (x_content_types != NULL)
        {
          for (n = 0; x_content_types[n] != NULL; n++)
            g_ptr_array_add (p, g_strdup (x_content_types[n]));
          g_strfreev (x_content_types);
        }
    }
  g_object_unref (root);
  g_free (uri);

  /* also add content types from hal capabilities */
  if (hal_mount->drive_device != NULL)
    {
      caps = dupv_and_uniqify (hal_device_get_property_strlist (hal_mount->drive_device, "info.capabilities"));
      if (caps != NULL)
        {
          for (n = 0; caps[n] != NULL; n++)
            {
              if (strcmp (caps[n], "portable_audio_player") == 0)
                g_ptr_array_add (p, g_strdup ("x-content/audio-player"));
            }
          g_strfreev (caps);
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

  return result;
}

/* since we're an out-of-process volume monitor we'll just do this sync */
static void
g_hal_mount_guess_content_type (GMount              *mount,
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

static char **
g_hal_mount_guess_content_type_finish (GMount              *mount,
                                       GAsyncResult        *result,
                                       GError             **error)
{
  /* TODO: handle force_rescan */
  return g_hal_mount_guess_content_type_sync (mount, FALSE, NULL, error);
}

static void
g_hal_mount_mount_iface_init (GMountIface *iface)
{
  iface->get_root = g_hal_mount_get_root;
  iface->get_name = g_hal_mount_get_name;
  iface->get_icon = g_hal_mount_get_icon;
  iface->get_uuid = g_hal_mount_get_uuid;
  iface->get_drive = g_hal_mount_get_drive;
  iface->get_volume = g_hal_mount_get_volume;
  iface->can_unmount = g_hal_mount_can_unmount;
  iface->can_eject = g_hal_mount_can_eject;
  iface->unmount = g_hal_mount_unmount;
  iface->unmount_finish = g_hal_mount_unmount_finish;
  iface->unmount_with_operation = g_hal_mount_unmount_with_operation;
  iface->unmount_with_operation_finish = g_hal_mount_unmount_with_operation_finish;
  iface->eject = g_hal_mount_eject;
  iface->eject_finish = g_hal_mount_eject_finish;
  iface->eject_with_operation = g_hal_mount_eject_with_operation;
  iface->eject_with_operation_finish = g_hal_mount_eject_with_operation_finish;
  iface->guess_content_type = g_hal_mount_guess_content_type;
  iface->guess_content_type_finish = g_hal_mount_guess_content_type_finish;
  iface->guess_content_type_sync = g_hal_mount_guess_content_type_sync;
}
