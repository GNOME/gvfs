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
  gboolean searched_for_icon;

  HalDevice *device;
  HalDevice *drive_device;
};

static GFile *
_g_find_file_insensitive_finish (GFile        *parent,
                                 GAsyncResult *result,
                                 GError      **error);

static void
_g_find_file_insensitive_async (GFile              *parent,
                                const gchar        *name,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data);

static void g_hal_mount_mount_iface_init (GMountIface *iface);

#define _G_IMPLEMENT_INTERFACE_DYNAMIC(TYPE_IFACE, iface_init)       { \
  const GInterfaceInfo g_implement_interface_info = { \
    (GInterfaceInitFunc) iface_init, NULL, NULL \
  }; \
  g_type_module_add_interface (type_module, g_define_type_id, TYPE_IFACE, &g_implement_interface_info); \
}
G_DEFINE_DYNAMIC_TYPE_EXTENDED (GHalMount, g_hal_mount, G_TYPE_OBJECT, 0, 
                                _G_IMPLEMENT_INTERFACE_DYNAMIC (G_TYPE_MOUNT,
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
g_hal_mount_class_finalize (GHalMountClass *klass)
{
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

typedef struct _MountIconSearchData
{
  GHalMount *mount;
  GFile *root;
} MountIconSearchData;

static void
clear_icon_search_data (MountIconSearchData *data)
{
  if (data->mount)
    g_object_unref (data->mount);
  if (data->root)
    g_object_unref (data->root);
  g_free (data);
}

static void
on_icon_file_located (GObject *source_object, GAsyncResult *res,
                      gpointer user_data)
{
  GFile *icon_file;
  GIcon *icon;
  MountIconSearchData *data = (MountIconSearchData *) (user_data);
  
  icon_file = _g_find_file_insensitive_finish (G_FILE (source_object),
                                               res, NULL);
  
  /* TODO: check if the file actually exists? */

  icon = g_file_icon_new (icon_file);
  g_object_unref (icon_file);

  g_hal_mount_override_icon (data->mount, icon);
  g_object_unref (icon);
  
  clear_icon_search_data (data);
}

static void
on_autorun_loaded (GObject *source_object, GAsyncResult *res,
                   gpointer user_data)
{
  gchar *content, *relative_icon_path = NULL;
  gsize content_length;
  MountIconSearchData *data = (MountIconSearchData *) (user_data);
  
  if (g_file_load_contents_finish (G_FILE (source_object), res, &content,
                                   &content_length, NULL, NULL))
    {
      /* Scan through for an "icon=" line. Can't use GKeyFile,
       * because .inf files aren't always valid key files
       **/
      GRegex *icon_regex;
      GMatchInfo *match_info;
      
      /* [^,] is because sometimes the icon= line
       * has a comma at the end
       **/
      icon_regex = g_regex_new ("icon=([^,\\r\\n]+)",
                                G_REGEX_CASELESS, 0, NULL);
      g_regex_match (icon_regex, content, 0,
                     &match_info);
      
      /* Even if there are multiple matches, pick only the
       * first.
       **/
      if (g_match_info_matches (match_info))
        {
          gchar *chr;
          gchar *word = g_match_info_fetch (match_info, 1);
          
          /* Replace '\' with '/' */
          while ((chr = strchr (word, '\\')) != NULL)
            *chr = '/';
          
          /* If the file name's not valid UTF-8,
           * don't even try to load it
           **/
          if (g_utf8_validate (word, -1, NULL))
            relative_icon_path = word;
          else
            g_free (word);
        }
      
      g_match_info_free (match_info);
      
      g_regex_unref (icon_regex);
      g_free (content);
    }
  
  if (relative_icon_path)
    {
      _g_find_file_insensitive_async (data->root,
                                      relative_icon_path,
                                      NULL, on_icon_file_located,
                                      data);
      
      g_free (relative_icon_path);
    }
  else
    clear_icon_search_data (data);
}

static void
on_autorun_located (GObject *source_object, GAsyncResult *res,
                    gpointer user_data)
{
  GFile *autorun_path;
  MountIconSearchData *data = (MountIconSearchData *) (user_data);
  
  autorun_path = _g_find_file_insensitive_finish (G_FILE (source_object),
                                                  res, NULL);
  if (autorun_path)
    g_file_load_contents_async (autorun_path, NULL, on_autorun_loaded, data);
  else
    clear_icon_search_data (data);
  
  g_object_unref (autorun_path);
}

static void
_g_find_mount_icon (GHalMount *m)
{
  MountIconSearchData *search_data;
  
  m->searched_for_icon = TRUE;
	
  search_data = g_new (MountIconSearchData, 1);
  search_data->mount = g_object_ref (m);
  search_data->root = g_mount_get_root (G_MOUNT (m));
  
  _g_find_file_insensitive_async (search_data->root,
                                  "autorun.inf",
                                  NULL, on_autorun_located,
                                  search_data);
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
do_update_from_hal (GHalMount *m)
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
  
  /*g_warning ("drive_type='%s'", drive_type); */
  /*g_warning ("drive_bus='%s'", drive_bus); */
  /*g_warning ("drive_uses_removable_media=%d", drive_uses_removable_media); */
  
  if (strcmp (drive_type, "disk") == 0)
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

  
  if (volume_fs_label != NULL && strlen (volume_fs_label) > 0)
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
    name = format_size_for_display (volume_size);

  if (m->override_name != NULL)
    {
      m->name = g_strdup (m->override_name);
      g_free (name);
    }
  else
    m->name = name;

  if (m->override_icon != NULL)
    m->icon = g_object_ref (m->override_icon);
  else
    m->icon = g_themed_icon_new_with_default_fallbacks (icon_name);
    
  /* If this is a CD-ROM, begin searching for an icon specified in
   * autorun.inf.
  **/
  if (strcmp (drive_type, "cdrom") == 0 && !m->searched_for_icon)
    _g_find_mount_icon (m);
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
  if (mount->override_name != NULL)
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

static gboolean
should_ignore_non_hal (GUnixMountEntry *mount_entry)
{
  const char *fs_type;
  
  fs_type = g_unix_mount_get_fs_type (mount_entry);

  /* We don't want to report nfs mounts. They are
     generally internal things, and cause a lot
     of pain with autofs and autorun */
  if (strcmp (fs_type, "nfs") == 0)
    return TRUE;

  return FALSE;
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
  if (volume == NULL && g_unix_mount_is_system_internal (mount_entry))
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

  if (volume != NULL || should_ignore_non_hal (mount_entry))
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
      g_signal_emit_by_name (mount, "changed");
      /* there's really no need to emit volume_changed on the volume monitor 
       * as we're going to be deleted.. */
    }
}

void
g_hal_mount_unset_volume (GHalMount *mount,
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
g_hal_mount_has_uuid (GHalMount         *mount,
                       const char        *uuid)
{
  if (mount->uuid == NULL)
    return FALSE;
  return strcmp (mount->uuid, uuid) == 0;
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
  close (data->error_fd);
  g_spawn_close_pid (pid);
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

static void
unmount_do (GMount               *mount,
                  GCancellable         *cancellable,
                  GAsyncReadyCallback   callback,
                  gpointer              user_data,
                  char                **argv,
                  gboolean              using_legacy)
{
  UnmountOp *data;
  GPid child_pid;
  GError *error;

  data = g_new0 (UnmountOp, 1);
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
      g_free (data);
      return;
    }
  data->error_string = g_string_new ("");
  data->error_channel = g_io_channel_unix_new (data->error_fd);
  data->error_channel_source_id = g_io_add_watch (data->error_channel, G_IO_IN, unmount_read_error, data);
  g_child_watch_add (child_pid, unmount_cb, data);
}


static void
g_hal_mount_unmount (GMount              *mount,
                     GMountUnmountFlags   flags,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  char *argv[] = {"gnome-mount", "-u", "-b", "-d", NULL, NULL};
  gboolean using_legacy = FALSE;

  if (hal_mount->device != NULL)
    argv[4] = hal_mount->device_path;
  else
    {
      using_legacy = TRUE;
      argv[0] = "umount";
      argv[1] = hal_mount->mount_path;
      argv[2] = NULL;
    }

  unmount_do (mount, cancellable, callback, user_data, argv, using_legacy);
}

static gboolean
g_hal_mount_unmount_finish (GMount       *mount,
                            GAsyncResult  *result,
                            GError       **error)
{
  return TRUE;
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
  g_free (data);
}

static void
g_hal_mount_eject (GMount              *mount,
                   GMountUnmountFlags   flags,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  GDrive *drive;

  if (hal_mount->volume != NULL)
    {
      drive = g_volume_get_drive (G_VOLUME (hal_mount->volume));
      if (drive != NULL)
        {
          EjectWrapperOp *data;
          data = g_new0 (EjectWrapperOp, 1);
          data->object = G_OBJECT (mount);
          data->callback = callback;
          data->user_data = user_data;
          g_drive_eject (drive, flags, cancellable, eject_wrapper_callback, data);
        }
    }
}

static gboolean
g_hal_mount_eject_finish (GMount        *mount,
                          GAsyncResult  *result,
                          GError       **error)
{
  GHalMount *hal_mount = G_HAL_MOUNT (mount);
  GDrive *drive;

  if (hal_mount->volume != NULL)
    {
      drive = g_volume_get_drive (G_VOLUME (hal_mount->volume));
      if (drive != NULL)
        return g_drive_eject_finish (drive, result, error);
    }
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

void 
g_hal_mount_register (GIOModule *module)
{
  g_hal_mount_register_type (G_TYPE_MODULE (module));
}

#define INSENSITIVE_SEARCH_ITEMS_PER_CALLBACK 100
 
static void
enumerated_children_callback (GObject *source_object, GAsyncResult *res,
                              gpointer user_data);
 
static void
more_files_callback (GObject *source_object, GAsyncResult *res,
                     gpointer user_data);

typedef struct _InsensitiveFileSearchData
{
	GFile *root;
	gchar *original_path;
	gchar **split_path;
	gint index;
	GFileEnumerator *enumerator;
	GFile *current_file;
	
	GCancellable *cancellable;
	GAsyncReadyCallback callback;
	gpointer user_data;
} InsensitiveFileSearchData;

static void
_g_find_file_insensitive_async (GFile              *parent,
                                const gchar        *name,
                                GCancellable       *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer            user_data)
{
  InsensitiveFileSearchData *data;
  
  g_return_if_fail (g_utf8_validate (name, -1, NULL));

  data = g_new (InsensitiveFileSearchData, 1);
  data->root = g_object_ref (parent);
  data->original_path = g_strdup (name);
  data->split_path = g_strsplit (name, G_DIR_SEPARATOR_S, -1);
  data->index = 0;
  data->enumerator = NULL;
  data->current_file = g_object_ref (parent);
  data->cancellable = cancellable;
  data->callback = callback;
  data->user_data = user_data;

  /* Skip any empty components due to multiple slashes */
  while (data->split_path[data->index] != NULL &&
         *data->split_path[data->index] == 0)
    data->index++;
  
  g_file_enumerate_children_async (data->current_file,
                                   G_FILE_ATTRIBUTE_STANDARD_NAME,
                                   0, G_PRIORITY_DEFAULT, cancellable,
                                   enumerated_children_callback, data);
}

static void
clear_find_file_insensitive_state (InsensitiveFileSearchData *data)
{
  if (data->root)
    g_object_unref (data->root);
  if (data->original_path)
    g_free (data->original_path);
  if (data->split_path)
    g_strfreev (data->split_path);
  if (data->enumerator)
    g_object_unref (data->enumerator);
  if (data->current_file)
    g_object_unref (data->current_file);
  g_free (data);
}

static void
enumerated_children_callback (GObject *source_object, GAsyncResult *res,
                              gpointer user_data)
{
  GFileEnumerator *enumerator;
  InsensitiveFileSearchData *data = (InsensitiveFileSearchData *) (user_data);
  
  enumerator = g_file_enumerate_children_finish (G_FILE (source_object),
                                                 res, NULL);
  
  if (enumerator == NULL)
    {
      GSimpleAsyncResult *simple;
      GFile *file;
      
      simple = g_simple_async_result_new (G_OBJECT (data->root),
                                          data->callback,
                                          data->user_data,
                                          _g_find_file_insensitive_async);
      
      file = g_file_get_child (data->root, data->original_path);
      
      g_simple_async_result_set_op_res_gpointer (simple, g_object_ref (file), g_object_unref);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      clear_find_file_insensitive_state (data);
      return;
    }
  
  data->enumerator = enumerator;
  g_file_enumerator_next_files_async (enumerator,
                                      INSENSITIVE_SEARCH_ITEMS_PER_CALLBACK,
                                      G_PRIORITY_DEFAULT,
                                      data->cancellable,
                                      more_files_callback,
                                      data);
}

static void
more_files_callback (GObject *source_object, GAsyncResult *res,
                     gpointer user_data)
{
  InsensitiveFileSearchData *data = (InsensitiveFileSearchData *) (user_data);
  GList *files, *l;
  gchar *filename = NULL, *component, *case_folded_name,
    *name_collation_key;
  gboolean end_of_files;
  
  files = g_file_enumerator_next_files_finish (data->enumerator,
                                               res, NULL);
  
  end_of_files = files == NULL;
  
  component = data->split_path[data->index];
  g_return_if_fail (component != NULL);
  
  case_folded_name = g_utf8_casefold (component, -1);
  name_collation_key = g_utf8_collate_key (case_folded_name, -1);
  g_free (case_folded_name);
  
  for (l = files; l != NULL; l = l->next)
    {
      GFileInfo *info;
      const gchar *this_name;
      
      info = l->data;
      this_name = g_file_info_get_name (info);
      
      if (g_utf8_validate (this_name, -1, NULL))
        {
          gchar *case_folded, *key;
          case_folded = g_utf8_casefold (this_name, -1);
          key = g_utf8_collate_key (case_folded, -1);
          g_free (case_folded);
          
          if (strcmp (key, name_collation_key) == 0)
            filename = g_strdup (this_name);
          g_free (key);
        }
      if (filename)
        break;
    }

  g_list_foreach (files, (GFunc)g_object_unref, NULL);
  g_list_free (files);
  g_free (name_collation_key);
  
  if (filename)
    {
      GFile *next_file;
      
      g_object_unref (data->enumerator);
      data->enumerator = NULL;
      
      /* Set the current file and continue searching */
      next_file = g_file_get_child (data->current_file, filename);
      g_free (filename);
      g_object_unref (data->current_file);
      data->current_file = next_file;
      
      data->index++;
      /* Skip any empty components due to multiple slashes */
      while (data->split_path[data->index] != NULL &&
             *data->split_path[data->index] == 0)
        data->index++;
      
      if (data->split_path[data->index] == NULL)
       {
          /* Search is complete, file was found */
          GSimpleAsyncResult *simple;
          
          simple = g_simple_async_result_new (G_OBJECT (data->root),
                                              data->callback,
                                              data->user_data,
                                              _g_find_file_insensitive_async);
          
          g_simple_async_result_set_op_res_gpointer (simple, g_object_ref (data->current_file), g_object_unref);
          g_simple_async_result_complete_in_idle (simple);
          g_object_unref (simple);
          clear_find_file_insensitive_state (data);
          return;
        }
      
      /* Continue searching down the tree */
      g_file_enumerate_children_async (data->current_file,
                                       G_FILE_ATTRIBUTE_STANDARD_NAME,
                                       0, G_PRIORITY_DEFAULT,
                                       data->cancellable,
                                       enumerated_children_callback,
                                       data);
      return;
    }
  
  if (end_of_files)
    {
      /* Could not find the given file, abort the search */
      GSimpleAsyncResult *simple;
      GFile *file;
      
      g_object_unref (data->enumerator);
      data->enumerator = NULL;
      
      simple = g_simple_async_result_new (G_OBJECT (data->root),
                                          data->callback,
                                          data->user_data,
                                          _g_find_file_insensitive_async);
      
      file = g_file_get_child (data->root, data->original_path);
      g_simple_async_result_set_op_res_gpointer (simple, file, g_object_unref);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      clear_find_file_insensitive_state (data);
      return;
    }
  
  /* Continue enumerating */
  g_file_enumerator_next_files_async (data->enumerator,
                                      INSENSITIVE_SEARCH_ITEMS_PER_CALLBACK,
                                      G_PRIORITY_DEFAULT,
                                      data->cancellable,
                                      more_files_callback,
                                      data);
}

static GFile *
_g_find_file_insensitive_finish (GFile        *parent,
                                 GAsyncResult *result,
                                 GError      **error)
{
  GSimpleAsyncResult *simple;
  GFile *file;
  
  g_return_val_if_fail (G_IS_SIMPLE_ASYNC_RESULT (result), NULL);
  
  simple = G_SIMPLE_ASYNC_RESULT (result);
  
  if (g_simple_async_result_propagate_error (simple, error))
    return NULL;
  
  file = G_FILE (g_simple_async_result_get_op_res_gpointer (simple));
  return g_object_ref (file);
}

