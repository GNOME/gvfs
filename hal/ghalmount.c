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

#include "ghalvolumemonitor.h"
#include "ghalmount.h"
#include "ghalvolume.h"

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
};

static void g_hal_mount_mount_iface_init (GMountIface *iface);

#define g_hal_mount_get_type _g_hal_mount_get_type
G_DEFINE_TYPE_WITH_CODE (GHalMount, g_hal_mount, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT,
						g_hal_mount_mount_iface_init))


static void
g_hal_mount_finalize (GObject *object)
{
  GHalMount *mount;
  
  mount = G_HAL_MOUNT (object);

  if (mount->volume != NULL)
    _g_hal_volume_unset_mount (mount->volume, mount);

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

const static struct {
        const char *disc_type;
        const char *icon_name;
        char *ui_name;
        char *ui_name_blank;
} disc_data[] = {
        {"cd_rom",        "media-optical-cd-rom", N_("CD-ROM Disc"), N_("Blank CD-ROM Disc")},
        {"cd_r",          "media-optical-cd-r", N_("CD-R Disc"), N_("Blank CD-R Disc")},
        {"cd_rw",         "media-optical-cd-rw", N_("CD-RW Disc"), N_("Blank CD-RW Disc")},
        {"dvd_rom",       "media-optical-dvd-rom", N_("DVD-ROM Disc"), N_("Blank DVD-ROM Disc")},
        {"dvd_ram",       "media-optical-dvd-ram", N_("DVD-RAM Disc"), N_("Blank DVD-RAM Disc")},
        {"dvd_r",         "media-optical-dvd-r", N_("DVD-ROM Disc"), N_("Blank DVD-ROM Disc")},
        {"dvd_rw",        "media-optical-dvd-rw", N_("DVD-RW Disc"), N_("Blank DVD-RW Disc")},
        {"dvd_plus_r",    "media-optical-dvd-r-plus", N_("DVD+R Disc"), N_("Blank DVD+R Disc")},
        {"dvd_plus_rw",   "media-optical-dvd-rw-plus",  N_("DVD+RW Disc"), N_("Blank DVD+RW Disc")},
        {"dvd_plus_r_dl", "media-optical-dvd-dl-r-plus", N_("DVD+R DL Disc"), N_("Blank DVD+R DL Disc")},
        {"bd_rom",        "media-optical-bd-rom", N_("Blu-Ray Disc"), N_("Blank Blu-Ray Disc")},
        {"bd_r",          "media-optical-bd-r", N_("Blu-Ray R Disc"), N_("Blank Blu-Ray R Disc")},
        {"bd_re",         "media-optical-bd-re", N_("Blu-Ray RW Disc"), N_("Blank Blu-Ray RW Disc")},
        {"hddvd_rom",     "media-optical-hddvd-rom", N_("HD DVD Disc"), N_("Blank HD DVD Disc")},
        {"hddvd_r",       "media-optical-hddvd-r", N_("HD DVD-R Disc"), N_("Blank HD DVD-R Disc")},
        {"hddvd_rw",      "media-optical-hddvd-rw", N_("HD DVD-RW Disc"), N_("Blank HD DVD-RW Disc")},
        {"mo",            "media-optical-mo", N_("MO Disc"), N_("Blank MO Disc")},
        {NULL,            "media-optical", N_("Disc"), N_("Blank Disc")},
};

static const char *
get_disc_icon (const char *disc_type)
{
  int n;
  
  for (n = 0; disc_data[n].disc_type != NULL; n++)
    {
      if (strcmp (disc_data[n].disc_type, disc_type) == 0)
        break;
    }
  
  return disc_data[n].icon_name;
}

static const char *
get_disc_name (const char *disc_type, gboolean is_blank)
{
  int n;
  
  for (n = 0; disc_data[n].disc_type != NULL; n++)
    {
      if (strcmp (disc_data[n].disc_type, disc_type) == 0)
        break;
    }
  
  if (is_blank)
    return disc_data[n].ui_name_blank;
  else
    return disc_data[n].ui_name;
}


#define KILOBYTE_FACTOR 1000.0
#define MEGABYTE_FACTOR (1000.0 * 1000.0)
#define GIGABYTE_FACTOR (1000.0 * 1000.0 * 1000.0)

static char *
_format_size_for_display (guint64 size)
{
  char *str;
  gdouble displayed_size;
  
  if (size < MEGABYTE_FACTOR)
    {
      displayed_size = (double) size / KILOBYTE_FACTOR;
      str = g_strdup_printf (_("%.1f kB Media"), displayed_size);
    } 
  else if (size < GIGABYTE_FACTOR)
    {
      displayed_size = (double) size / MEGABYTE_FACTOR;
      str = g_strdup_printf (_("%.1f MB Media"), displayed_size);
    } 
  else 
    {
      displayed_size = (double) size / GIGABYTE_FACTOR;
      str = g_strdup_printf (_("%.1f GB Media"), displayed_size);
    }
  
  return str;
}

static void
_do_update_from_hal (GHalMount *m)
{
  HalDevice *volume;
  HalDevice *drive;

  char *name;
  const char *icon_name;

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
  
  //g_warning ("drive_type='%s'", drive_type);
  //g_warning ("drive_bus='%s'", drive_bus);
  //g_warning ("drive_uses_removable_media=%d", drive_uses_removable_media);
  
  if (strcmp (drive_type, "disk") == 0) {
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
  } else if (strcmp (drive_type, "cdrom") == 0)
    icon_name = g_strdup (get_disc_icon (volume_disc_type));
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

  
  if (volume_fs_label != NULL && strlen (volume_fs_label) > 0) {
    name = g_strdup (volume_fs_label);
  } else if (volume_is_disc) {
    if (volume_disc_has_audio) {
      if (volume_disc_has_data)
        name = g_strdup (_("Mixed Audio/Data Disc"));
      else
        name = g_strdup (_("Audio Disc"));
    } else {
      name = g_strdup (get_disc_name (volume_disc_type, volume_disc_is_blank));
    }
  } else {
    name = _format_size_for_display (volume_size);
  }

  if (m->override_name != NULL)
    {
      m->name = g_strdup (m->override_name);
      g_free (name);
    }
  else
    {
      m->name = name;
    }

  if (m->override_icon != NULL)
    {
      m->icon = g_object_ref (m->override_icon);
    }
  else
    {
      m->icon = g_themed_icon_new (icon_name);
    }
}


static void
_update_from_hal (GHalMount *m, gboolean emit_changed)
{
  char *old_name;
  GIcon *old_icon;

  old_name = g_strdup (m->name);
  old_icon = m->icon != NULL ? g_object_ref (m->icon) : NULL;

  g_free (m->name);
  if (m->icon != NULL)
    g_object_unref (m->icon);
  _do_update_from_hal (m);

  if (emit_changed)
    {
      if (old_name == NULL || 
          old_icon == NULL ||
          strcmp (old_name, m->name) != 0 ||
          (! g_icon_equal (old_icon, m->icon)))
        {
          g_signal_emit_by_name (m, "changed");
          if (m->volume_monitor != NULL)
            g_signal_emit_by_name (m->volume_monitor, "mount_changed", m);
        }
    }
  g_free (old_name);
  if (old_icon != NULL)
    g_object_unref (old_icon);
}

static void
hal_changed (HalDevice *device, const char *key, gpointer user_data)
{
  GHalMount *hal_mount = G_HAL_MOUNT (user_data);
  
  //g_warning ("mounthal modifying %s (property %s changed)", hal_mount->device_path, key);
  _update_from_hal (hal_mount, TRUE);
}

static void
_compute_uuid (GHalMount *mount)
{
  const char *fs_uuid;
  const char *fs_label;

  /* use the FS uuid before falling back to the FS label */

  fs_uuid = hal_device_get_property_string (mount->device, "volume.uuid");
  fs_label = hal_device_get_property_string (mount->device, "volume.label");

  if (strlen (fs_uuid) == 0)
    {
      if (strlen (fs_label) == 0)
        {
          mount->uuid = NULL;
        }
      else
        {
          mount->uuid = g_strdup (fs_label);
        }
    }
  else
    {
      mount->uuid = g_strdup (fs_uuid);
    }
}


GHalMount *
_g_hal_mount_new_for_hal_device    (GVolumeMonitor    *volume_monitor,
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
  
  //g_warning ("storage_udi = %s", storage_udi);
  
  drive_device = hal_pool_get_device_by_udi (pool, storage_udi);
  if (drive_device == NULL)
    goto fail;
  
  //g_warning ("drive_device = %p", drive_device);
  
  mount = g_object_new (G_TYPE_HAL_MOUNT, NULL);
  mount->volume_monitor = volume_monitor;
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

  _compute_uuid (mount);
  _update_from_hal (mount, FALSE);

  /* need to do this last */
  mount->volume = volume;
  if (volume != NULL)
    _g_hal_volume_set_mount (volume, mount);

  return mount;

 fail:
  return NULL;
}

void 
_g_hal_mount_override_name (GHalMount *mount, const char *name)
{
  if (mount->override_name != NULL)
    g_free (mount->override_name);

  if (name != NULL)
    mount->override_name = g_strdup (name);
  else
    mount->override_name = NULL;

  _update_from_hal (mount, TRUE);
}

void
_g_hal_mount_override_icon (GHalMount *mount, GIcon *icon)
{
  if (mount->override_icon != NULL)
    g_object_unref (mount->override_icon);

  if (icon == NULL)
    mount->override_icon = g_object_ref (icon);
  else
    mount->override_icon = NULL;

  _update_from_hal (mount, TRUE);
}

GHalMount *
_g_hal_mount_new (GVolumeMonitor       *volume_monitor,
                  GUnixMountEntry      *mount_entry,
                  HalPool              *pool,
                  GHalVolume           *volume)
{
  HalDevice *device;
  HalDevice *drive_device;
  const char *storage_udi;
  GHalMount *mount;
  
  /* No volume for mount: Ignore internal things */
  if (volume == NULL && g_unix_mount_is_system_internal (mount_entry))
    return NULL;

  mount = g_object_new (G_TYPE_HAL_MOUNT, NULL);
  mount->volume_monitor = volume_monitor;
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
      //g_warning ("device = %p for %s", device, mount->device_path);
      if (device != NULL)
        {      
          //g_warning ("udi = %s", hal_device_get_udi (device));

          storage_udi = hal_device_get_property_string (device, "block.storage_device");
          if (storage_udi == NULL)
            goto not_hal;

          //g_warning ("storage_udi = %s", storage_udi);
          
          drive_device = hal_pool_get_device_by_udi (pool, storage_udi);
          if (drive_device == NULL)
            goto not_hal;

          //g_warning ("drive_device = %p", drive_device);
          
          mount->device = g_object_ref (device);
          mount->drive_device = g_object_ref (drive_device);
          
          g_signal_connect_object (device, "hal_property_changed", (GCallback) hal_changed, mount, 0);
          g_signal_connect_object (drive_device, "hal_property_changed", (GCallback) hal_changed, mount, 0);
          
          _compute_uuid (mount);
          _update_from_hal (mount, FALSE);
          
          goto was_hal;
        }
    }

 not_hal:

  mount->name = g_unix_mount_guess_name (mount_entry);
  mount->icon = g_unix_mount_guess_icon (mount_entry);

 was_hal:

  /* need to do this last */
  mount->volume = volume;
  if (volume != NULL)
    _g_hal_volume_set_mount (volume, mount);

  return mount;
}

void
_g_hal_mount_unmounted (GHalMount *mount)
{
  if (mount->volume != NULL)
    {
      _g_hal_volume_unset_mount (mount->volume, mount);
      mount->volume = NULL;
      g_signal_emit_by_name (mount, "changed");
      /* there's really no need to emit volume_changed on the volume monitor 
       * as we're going to be deleted.. */
    }
}

void
_g_hal_mount_unset_volume (GHalMount *mount,
                                       GHalVolume  *volume)
{
  if (mount->volume == volume)
    {
      mount->volume = NULL;
      /* TODO: Emit changed in idle to avoid locking issues */
      g_signal_emit_by_name (mount, "changed");
      if (mount->volume_monitor != NULL)
        g_signal_emit_by_name (mount->volume_monitor, "mount_changed", mount);
    }
}

static GFile *
g_hal_mount_get_root (GMount *mount)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);

  if (hal_mount->override_root != NULL)
    return g_object_ref (hal_mount->override_root);
  else
    return g_file_new_for_path (hal_mount->mount_path);
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
_g_hal_mount_has_uuid (GHalMount         *mount,
                       const char        *uuid)
{
  if (mount->uuid == NULL)
    return FALSE;
  return strcmp (mount->uuid, uuid) == 0;
}

gboolean
_g_hal_mount_has_mount_path (GHalMount *mount,
                             const char  *mount_path)
{
  return strcmp (mount->mount_path, mount_path) == 0;
}

gboolean
_g_hal_mount_has_udi (GHalMount *mount,
                      const char  *udi)
{
  if (mount->device == NULL)
    return FALSE;
  return strcmp (hal_device_get_udi (mount->device), udi) == 0;
}

static GDrive *
g_hal_mount_get_drive (GMount *mount)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);

  if (hal_mount->volume != NULL)
    return g_volume_get_drive (G_VOLUME (hal_mount->volume));

  return NULL;
}

static GVolume *
g_hal_mount_get_volume (GMount *mount)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);

  if (hal_mount->volume)
    return G_VOLUME (g_object_ref (hal_mount->volume));
  
  return NULL;
}

static gboolean
g_hal_mount_can_unmount (GMount *mount)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);

  if (hal_mount->cannot_unmount)
    return FALSE;

  return TRUE;
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
} EjectUnmountOp;

static void 
eject_unmount_cb (GPid pid, gint status, gpointer user_data)
{
  EjectUnmountOp *data = user_data;
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
          /* TODO: how do we report an error back to the caller while telling
           * him that we already have shown an error dialog to the user?
           *
           * G_IO_ERROR_FAILED_NO_UI or something?
           */
          simple = g_simple_async_result_new (data->object,
                                              data->callback,
                                              data->user_data,
                                              NULL);
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
  close (data->error_fd);
  g_spawn_close_pid (pid);
  g_free (data);
}

static gboolean
eject_unmount_read_error (GIOChannel *channel,
                          GIOCondition condition,
                          gpointer user_data)
{
  char *str;
  gsize str_len;
  EjectUnmountOp *data = user_data;

  g_io_channel_read_to_end (channel, &str, &str_len, NULL);
  g_string_append (data->error_string, str);
  g_free (str);
  return TRUE;
}

static void
eject_unmount_do (GMount               *mount,
                  GCancellable         *cancellable,
                  GAsyncReadyCallback   callback,
                  gpointer              user_data,
                  char                **argv,
                  gboolean              using_legacy)
{
  EjectUnmountOp *data;
  GPid child_pid;
  GError *error;

  data = g_new0 (EjectUnmountOp, 1);
  data->object = G_OBJECT (mount);
  data->callback = callback;
  data->user_data = user_data;
  data->cancellable = cancellable;  
  data->using_legacy = using_legacy;
  
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
    GSimpleAsyncResult *simple;
    simple = g_simple_async_result_new_from_error (data->object,
                                                   data->callback,
                                                   data->user_data,
                                                   error);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
    g_error_free (error);
    g_free (data);
    return;
  }
  data->error_string = g_string_new ("");
  data->error_channel = g_io_channel_unix_new (data->error_fd);
  data->error_channel_source_id = g_io_add_watch (data->error_channel, G_IO_IN, eject_unmount_read_error, data);
  g_child_watch_add (child_pid, eject_unmount_cb, data);
}


static void
g_hal_mount_unmount (GMount             *mount,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  char *argv[] = {"gnome-mount", "-u", "-b", "-d", NULL, NULL};
  gboolean using_legacy = FALSE;

  if (hal_mount->device != NULL)
    {
      argv[4] = hal_mount->device_path;
    }
  else
    {
      using_legacy = TRUE;
      argv[0] = "umount";
      argv[1] = hal_mount->mount_path;
      argv[2] = NULL;
    }

  eject_unmount_do (mount, cancellable, callback, user_data, argv, using_legacy);
}

static gboolean
g_hal_mount_unmount_finish (GMount       *mount,
			      GAsyncResult  *result,
			      GError       **error)
{
  return TRUE;
}

static void
g_hal_mount_eject (GMount              *mount,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  char *argv[] = {"gnome-mount", "-e", "-b", "-d", NULL, NULL};
  gboolean using_legacy = FALSE;

  if (hal_mount->device != NULL)
    {
      argv[4] = hal_mount->device_path;
    }
  else
    {
      using_legacy = TRUE;
      argv[0] = "eject";
      argv[1] = hal_mount->mount_path;
      argv[2] = NULL;
    }

  eject_unmount_do (mount, cancellable, callback, user_data, argv, using_legacy);
}

static gboolean
g_hal_mount_eject_finish (GMount        *mount,
                          GAsyncResult  *result,
                          GError       **error)
{
  return TRUE;
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
  iface->eject = g_hal_mount_eject;
  iface->eject_finish = g_hal_mount_eject_finish;
}

