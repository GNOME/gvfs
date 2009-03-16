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

#include <limits.h>
#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "ghalvolumemonitor.h"
#include "ghalmount.h"
#include "ghalvolume.h"
#include "ghaldrive.h"
#include "hal-pool.h"

/* We use this static variable for enforcing a singleton pattern since
 * the get_mount_for_mount_path() method on GNativeVolumeMonitor calls
 * us without an instance..  and ideally we want to piggyback on an
 * already existing instance. 
 *
 * We don't need locking since this runs out of process in a single
 * threaded mode with now weird things happening in signal handlers.
 */

static GHalVolumeMonitor *the_volume_monitor = NULL;
static HalPool *pool = NULL;

struct _GHalVolumeMonitor {
  GNativeVolumeMonitor parent;

  GUnixMountMonitor *mount_monitor;

  HalPool *pool;

  GList *last_optical_disc_devices;
  GList *last_drive_devices;
  GList *last_volume_devices;
  GList *last_mountpoints;
  GList *last_mounts;

  GList *drives;
  GList *volumes;
  GList *mounts;

  /* we keep volumes/mounts for blank and audio discs separate to handle e.g. mixed discs properly */
  GList *disc_volumes;
  GList *disc_mounts;

};

static void mountpoints_changed      (GUnixMountMonitor  *mount_monitor,
                                      gpointer            user_data);
static void mounts_changed           (GUnixMountMonitor  *mount_monitor,
                                      gpointer            user_data);
static void hal_changed              (HalPool    *pool,
                                      HalDevice  *device,
                                      gpointer    user_data);
static void update_all               (GHalVolumeMonitor *monitor,
                                      gboolean emit_changes,
                                      gboolean emit_in_idle);
static void update_drives            (GHalVolumeMonitor *monitor,
                                      GList **added_drives,
                                      GList **removed_drives);
static void update_volumes           (GHalVolumeMonitor *monitor,
                                      GList **added_volumes,
                                      GList **removed_volumes);
static void update_mounts            (GHalVolumeMonitor *monitor,
                                      GList **added_mounts,
                                      GList **removed_mounts);
static void update_discs             (GHalVolumeMonitor *monitor,
                                      GList **added_volumes,
                                      GList **removed_volumes,
                                      GList **added_mounts,
                                      GList **removed_mounts);


G_DEFINE_TYPE (GHalVolumeMonitor, g_hal_volume_monitor, G_TYPE_NATIVE_VOLUME_MONITOR)

static void
list_free (GList *objects)
{
  g_list_foreach (objects, (GFunc)g_object_unref, NULL);
  g_list_free (objects);
}

static HalPool *
get_hal_pool (void)
{
  char *cap_only[] = {"block", NULL};

  if (pool == NULL)
    pool = hal_pool_new (cap_only);
  
  return pool;
}

static void
g_hal_volume_monitor_dispose (GObject *object)
{
  GHalVolumeMonitor *monitor;
  
  monitor = G_HAL_VOLUME_MONITOR (object);

  the_volume_monitor = NULL;
  
  if (G_OBJECT_CLASS (g_hal_volume_monitor_parent_class)->dispose)
    (*G_OBJECT_CLASS (g_hal_volume_monitor_parent_class)->dispose) (object);
}

static void
g_hal_volume_monitor_finalize (GObject *object)
{
  GHalVolumeMonitor *monitor;

  monitor = G_HAL_VOLUME_MONITOR (object);

  g_signal_handlers_disconnect_by_func (monitor->mount_monitor, mountpoints_changed, monitor);
  g_signal_handlers_disconnect_by_func (monitor->mount_monitor, mounts_changed, monitor);
  g_signal_handlers_disconnect_by_func (monitor->pool, hal_changed, monitor);

  g_object_unref (monitor->mount_monitor);
  g_object_unref (monitor->pool);
  
  list_free (monitor->last_optical_disc_devices);
  list_free (monitor->last_drive_devices);
  list_free (monitor->last_volume_devices);
  list_free (monitor->last_mountpoints);
  g_list_foreach (monitor->last_mounts,
		  (GFunc)g_unix_mount_free, NULL);
  g_list_free (monitor->last_mounts);

  list_free (monitor->drives);
  list_free (monitor->volumes);
  list_free (monitor->mounts);

  list_free (monitor->disc_volumes);
  list_free (monitor->disc_mounts);
  
  if (G_OBJECT_CLASS (g_hal_volume_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_hal_volume_monitor_parent_class)->finalize) (object);
}

static GList *
get_mounts (GVolumeMonitor *volume_monitor)
{
  GHalVolumeMonitor *monitor;
  GList *l, *ll;
  
  monitor = G_HAL_VOLUME_MONITOR (volume_monitor);

  l = g_list_copy (monitor->mounts);
  ll = g_list_copy (monitor->disc_mounts);
  l = g_list_concat (l, ll);

  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  return l;
}

static GList *
get_volumes (GVolumeMonitor *volume_monitor)
{
  GHalVolumeMonitor *monitor;
  GList *l, *ll;
  
  monitor = G_HAL_VOLUME_MONITOR (volume_monitor);

  l = g_list_copy (monitor->volumes);
  ll = g_list_copy (monitor->disc_volumes);
  l = g_list_concat (l, ll);

  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  return l;
}

static GList *
get_connected_drives (GVolumeMonitor *volume_monitor)
{
  GHalVolumeMonitor *monitor;
  GList *l;
  
  monitor = G_HAL_VOLUME_MONITOR (volume_monitor);

  l = g_list_copy (monitor->drives);
  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  return l;
}

static GVolume *
get_volume_for_uuid (GVolumeMonitor *volume_monitor, const char *uuid)
{
  GHalVolumeMonitor *monitor;
  GHalVolume *volume;
  GList *l;
  
  monitor = G_HAL_VOLUME_MONITOR (volume_monitor);

  volume = NULL;

  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      volume = l->data;
      if (g_hal_volume_has_uuid (volume, uuid))
        goto found;
    }

  for (l = monitor->disc_volumes; l != NULL; l = l->next)
    {
      volume = l->data;
      if (g_hal_volume_has_uuid (volume, uuid))
        goto found;
    }

  return NULL;

 found:

  g_object_ref (volume);

  return (GVolume *)volume;
}

static GMount *
get_mount_for_uuid (GVolumeMonitor *volume_monitor, const char *uuid)
{
  GHalVolumeMonitor *monitor;
  GHalMount *mount;
  GList *l;
  
  monitor = G_HAL_VOLUME_MONITOR (volume_monitor);

  mount = NULL;

  for (l = monitor->mounts; l != NULL; l = l->next)
    {
      mount = l->data;
      if (g_hal_mount_has_uuid (mount, uuid))
        goto found;
    }

  for (l = monitor->disc_mounts; l != NULL; l = l->next)
    {
      mount = l->data;
      if (g_hal_mount_has_uuid (mount, uuid))
        goto found;
    }

  return NULL;

 found:

  g_object_ref (mount);
  
  return (GMount *)mount;
}

static GMount *
get_mount_for_mount_path (const char *mount_path,
                          GCancellable *cancellable)
{
  GMount *mount;
  GHalMount *hal_mount;
  GHalVolumeMonitor *volume_monitor;

  volume_monitor = NULL;
  if (the_volume_monitor != NULL)
    volume_monitor = g_object_ref (the_volume_monitor);

  if (volume_monitor == NULL)
    {
      /* Dammit, no monitor is set up.. so we have to create one, find
       * what the user asks for and throw it away again. 
       *
       * What a waste - especially considering that there's IO
       * involved in doing this: connect to the system message bus;
       * IPC to hald...
       */
      volume_monitor = G_HAL_VOLUME_MONITOR (g_hal_volume_monitor_new ());
    }

  mount = NULL;

  /* creation of the volume monitor might actually fail */
  if (volume_monitor != NULL)
    {
      GList *l;

      for (l = volume_monitor->mounts; l != NULL; l = l->next)
        {
          hal_mount = l->data;
          
          if (g_hal_mount_has_mount_path (hal_mount, mount_path))
            {
              mount = g_object_ref (hal_mount);
              break;
            }
        }

      g_object_unref (volume_monitor);
    }

  return (GMount *)mount;
}

static void
mountpoints_changed (GUnixMountMonitor *mount_monitor,
		     gpointer           user_data)
{
  GHalVolumeMonitor *monitor = G_HAL_VOLUME_MONITOR (user_data);

  update_all (monitor, TRUE, TRUE);
}

static void
mounts_changed (GUnixMountMonitor *mount_monitor,
		gpointer           user_data)
{
  GHalVolumeMonitor *monitor = G_HAL_VOLUME_MONITOR (user_data);

  update_all (monitor, TRUE, TRUE);
}

void 
g_hal_volume_monitor_force_update (GHalVolumeMonitor *monitor, gboolean emit_in_idle)
{
  update_all (monitor, TRUE, emit_in_idle);
}

static void
hal_changed (HalPool    *pool,
             HalDevice  *device,
             gpointer    user_data)
{
  GHalVolumeMonitor *monitor = G_HAL_VOLUME_MONITOR (user_data);
  
  /*g_warning ("hal changed");*/
  
  update_all (monitor, TRUE, TRUE);
}

static GObject *
g_hal_volume_monitor_constructor (GType                  type,
                                  guint                  n_construct_properties,
                                  GObjectConstructParam *construct_properties)
{
  GObject *object;
  GHalVolumeMonitor *monitor;
  GHalVolumeMonitorClass *klass;
  GObjectClass *parent_class;  

  if (the_volume_monitor != NULL)
    {
      object = g_object_ref (the_volume_monitor);
      return object;
    }

  /*g_warning ("creating hal vm");*/

  object = NULL;

  /* Invoke parent constructor. */
  klass = G_HAL_VOLUME_MONITOR_CLASS (g_type_class_peek (G_TYPE_HAL_VOLUME_MONITOR));
  parent_class = G_OBJECT_CLASS (g_type_class_peek_parent (klass));
  object = parent_class->constructor (type,
                                      n_construct_properties,
                                      construct_properties);

  monitor = G_HAL_VOLUME_MONITOR (object);
  monitor->pool = g_object_ref (get_hal_pool ());

  monitor->mount_monitor = g_unix_mount_monitor_new ();

  g_signal_connect (monitor->mount_monitor,
		    "mounts_changed", G_CALLBACK (mounts_changed),
		    monitor);
  
  g_signal_connect (monitor->mount_monitor,
		    "mountpoints_changed", G_CALLBACK (mountpoints_changed),
		    monitor);

  g_signal_connect (monitor->pool,
                    "device_added", G_CALLBACK (hal_changed),
                    monitor);
  
  g_signal_connect (monitor->pool,
                    "device_removed", G_CALLBACK (hal_changed),
                    monitor);
		    
  update_all (monitor, FALSE, TRUE);

  the_volume_monitor = monitor;

  return object;
}

static void
g_hal_volume_monitor_init (GHalVolumeMonitor *monitor)
{
}

static gboolean
is_supported (void)
{
  return get_hal_pool() != NULL;
}

static GVolume *
adopt_orphan_mount (GMount *mount, GVolumeMonitor *monitor)
{
  GList *l;
  GFile *mount_root;
  GVolume *ret;
  
  /* This is called by the union volume monitor which does
     have a ref to this. So its guaranteed to live, unfortunately
     the pointer is not passed as an argument :/
  */
  ret = NULL;
  
  if (the_volume_monitor == NULL)
    {
      return NULL;
    }

  mount_root = g_mount_get_root (mount);
  
  /* cdda:// as foreign mounts */
  for (l = the_volume_monitor->disc_volumes; l != NULL; l = l->next)
    {
      GHalVolume *volume = l->data;
      
      if (g_hal_volume_has_foreign_mount_root (volume, mount_root))
        {
          g_hal_volume_adopt_foreign_mount (volume, mount);
          ret = g_object_ref (volume);
          goto found;
        }
    }

 found:
  g_object_unref (mount_root);
  
  return ret;
}

static void
g_hal_volume_monitor_class_init (GHalVolumeMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVolumeMonitorClass *monitor_class = G_VOLUME_MONITOR_CLASS (klass);
  GNativeVolumeMonitorClass *native_class = G_NATIVE_VOLUME_MONITOR_CLASS (klass);

  gobject_class->constructor = g_hal_volume_monitor_constructor;
  gobject_class->finalize = g_hal_volume_monitor_finalize;
  gobject_class->dispose = g_hal_volume_monitor_dispose;

  monitor_class->get_mounts = get_mounts;
  monitor_class->get_volumes = get_volumes;
  monitor_class->get_connected_drives = get_connected_drives;
  monitor_class->get_volume_for_uuid = get_volume_for_uuid;
  monitor_class->get_mount_for_uuid = get_mount_for_uuid;
  monitor_class->adopt_orphan_mount = adopt_orphan_mount;
  monitor_class->is_supported = is_supported;

  native_class->get_mount_for_mount_path = get_mount_for_mount_path;
}

/**
 * g_hal_volume_monitor_new:
 * 
 * Returns:  a new #GVolumeMonitor.
 **/
GVolumeMonitor *
g_hal_volume_monitor_new (void)
{
  GHalVolumeMonitor *monitor;

  monitor = g_object_new (G_TYPE_HAL_VOLUME_MONITOR, NULL);

  return G_VOLUME_MONITOR (monitor);
}

static void
diff_sorted_lists (GList         *list1, 
                   GList         *list2, 
                   GCompareFunc   compare,
		   GList        **added, 
                   GList        **removed)
{
  int order;
  
  *added = *removed = NULL;
  
  while (list1 != NULL &&
	 list2 != NULL)
    {
      order = (*compare) (list1->data, list2->data);
      if (order < 0)
	{
	  *removed = g_list_prepend (*removed, list1->data);
	  list1 = list1->next;
	}
      else if (order > 0)
	{
	  *added = g_list_prepend (*added, list2->data);
	  list2 = list2->next;
	}
      else
	{ /* same item */
	  list1 = list1->next;
	  list2 = list2->next;
	}
    }

  while (list1 != NULL)
    {
      *removed = g_list_prepend (*removed, list1->data);
      list1 = list1->next;
    }
  while (list2 != NULL)
    {
      *added = g_list_prepend (*added, list2->data);
      list2 = list2->next;
    }
}

static GHalVolume *
lookup_volume_for_mount_path (GHalVolumeMonitor *monitor,
                                                   const char         *mount_path)
{
  GList *l;
  GHalVolume *found;

  found = NULL;
  
  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      GHalVolume *volume = l->data;

      if (g_hal_volume_has_mount_path (volume, mount_path))
        {
          found = volume;
          break;
        }
    }

  return found;
}

static GHalVolume *
lookup_volume_for_device_path (GHalVolumeMonitor *monitor,
                                                    const char         *device_path)
{
  GList *l;

  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      GHalVolume *volume = l->data;

      if (g_hal_volume_has_device_path (volume, device_path))
	return volume;
    }
  
  return NULL;
}



static GHalMount *
find_mount_by_mount_path (GHalVolumeMonitor *monitor,
                          const char *mount_path)
{
  GList *l;

  for (l = monitor->mounts; l != NULL; l = l->next)
    {
      GHalMount *mount = l->data;

      if (g_hal_mount_has_mount_path (mount, mount_path))
	return mount;
    }
  
  return NULL;
}

static GHalVolume *
find_volume_by_udi (GHalVolumeMonitor *monitor, const char *udi)
{
  GList *l;

  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      GHalVolume *volume = l->data;

      if (g_hal_volume_has_udi (volume, udi))
	return volume;
    }
  
  return NULL;
}

static GHalDrive *
find_drive_by_udi (GHalVolumeMonitor *monitor, const char *udi)
{
  GList *l;

  for (l = monitor->drives; l != NULL; l = l->next)
    {
      GHalDrive *drive = l->data;

      if (g_hal_drive_has_udi (drive, udi))
	return drive;
    }
  
  return NULL;
}

static GHalMount *
find_disc_mount_by_udi (GHalVolumeMonitor *monitor, const char *udi)
{
  GList *l;

  for (l = monitor->disc_mounts; l != NULL; l = l->next)
    {
      GHalMount *mount = l->data;
      
      if (g_hal_mount_has_udi (mount, udi))
	return mount;
    }
  
  return NULL;
}

static GHalVolume *
find_disc_volume_by_udi (GHalVolumeMonitor *monitor, const char *udi)
{
  GList *l;

  for (l = monitor->disc_volumes; l != NULL; l = l->next)
    {
      GHalVolume *volume = l->data;

      if (g_hal_volume_has_udi (volume, udi))
	return volume;
    }
  
  return NULL;
}

static gint
hal_device_compare (HalDevice *a, HalDevice *b)
{
  return strcmp (hal_device_get_udi (a), hal_device_get_udi (b));
}

/* TODO: move to gio */
static gboolean
_g_unix_mount_point_guess_should_display (GUnixMountPoint *mount_point)
{
  const char *mount_path;

  mount_path = g_unix_mount_point_get_mount_path (mount_point);

  /* Never display internal mountpoints */
  if (g_unix_is_mount_path_system_internal (mount_path))
    return FALSE;

  /* Only display things in /media (which are generally user mountable)
     and home dir (fuse stuff) */
  if (g_str_has_prefix (mount_path, "/media/"))
    return TRUE;

  if (g_str_has_prefix (mount_path, g_get_home_dir ()))
    return TRUE;

  return FALSE;
}


static GUnixMountPoint *
get_mount_point_for_device (HalDevice *d, GList *fstab_mount_points)
{
  GList *l;
  const char *device_file;
  const char *device_mount_point;

  device_mount_point = hal_device_get_property_string (d, "volume.mount_point");

  device_file = hal_device_get_property_string (d, "block.device");

  for (l = fstab_mount_points; l != NULL; l = l->next)
    {
      GUnixMountPoint *mount_point = l->data;
      const char *device_path;
      const char *mount_path;
      
      mount_path = g_unix_mount_point_get_mount_path (mount_point);
      if (device_mount_point != NULL &&
          mount_path != NULL &&
          strcmp (device_mount_point, mount_path) == 0)
        return mount_point;
      
      device_path = g_unix_mount_point_get_device_path (mount_point);
      if (g_str_has_prefix (device_path, "LABEL="))
        {
          if (strcmp (device_path + 6, hal_device_get_property_string (d, "volume.label")) == 0)
            return mount_point;
        }
      else if (g_str_has_prefix (device_path, "UUID="))
        {
          if (g_ascii_strcasecmp (device_path + 5, hal_device_get_property_string (d, "volume.uuid")) == 0)
            return mount_point;
        }
      else
        {
          char resolved_device_path[PATH_MAX];
          /* handle symlinks such as /dev/disk/by-uuid/47C2-1994 */
          if (realpath (device_path, resolved_device_path) != NULL &&
              strcmp (resolved_device_path, device_file) == 0)
            return mount_point;
        }
    }

  return NULL;
}

static gboolean
should_mount_be_ignored (HalPool *pool, HalDevice *d)
{
  const char *device_mount_point;

  device_mount_point = hal_device_get_property_string (d, "volume.mount_point");
  if (device_mount_point != NULL && strlen (device_mount_point) > 0)
    {
      GUnixMountEntry *mount_entry;

      /*g_warning ("device_mount_point = '%s'", device_mount_point);*/

      mount_entry = g_unix_mount_at (device_mount_point, NULL);
      if (mount_entry != NULL) {
        if (!g_unix_mount_guess_should_display (mount_entry))
          {
            g_unix_mount_free (mount_entry);
            return TRUE;
          }
        g_unix_mount_free (mount_entry);
      }
    }

  return FALSE;
}

static gboolean
should_volume_be_ignored (HalPool *pool, HalDevice *d, GList *fstab_mount_points)
{
  gboolean volume_ignore;
  const char *volume_fsusage;
  GUnixMountPoint *mount_point;
  
  volume_fsusage = hal_device_get_property_string (d, "volume.fsusage");
  volume_ignore = hal_device_get_property_bool (d, "volume.ignore");
  
  if (volume_fsusage == NULL)
    {
      /*g_warning ("no volume.fsusage property. Refusing to ignore");*/
      return FALSE;
    }

  if (volume_ignore)
    return TRUE;

  if (strcmp (volume_fsusage, "filesystem") != 0)
    {
      /* no file system on the volume... blank and audio discs are handled in update_discs() */

      /* check if it's a LUKS crypto volume */
      if (strcmp (volume_fsusage, "crypto") == 0)
        {
          if (strcmp (hal_device_get_property_string (d, "volume.fstype"), "crypto_LUKS") == 0)
            {
              HalDevice *cleartext_device;

              /* avoid showing cryptotext volume if it's corresponding cleartext volume is available */
              cleartext_device = hal_pool_get_device_by_capability_and_string (pool,
                                                                               "block",
                                                                               "volume.crypto_luks.clear.backing_volume",
                                                                               hal_device_get_udi (d));
              
              if (cleartext_device == NULL)
                {
                  return FALSE;
                }
            }
        }
      return TRUE;
    }

  mount_point = get_mount_point_for_device (d, fstab_mount_points);
  if (mount_point != NULL && !_g_unix_mount_point_guess_should_display (mount_point))
    return TRUE;

  if (hal_device_get_property_bool (d, "volume.is_mounted"))
    return should_mount_be_ignored (pool, d);

  return FALSE;
}

static gboolean
should_drive_be_ignored (HalPool *pool, HalDevice *d, GList *fstab_mount_points)
{
  GList *volumes, *l;
  const char *drive_udi;
  gboolean all_volumes_ignored, got_volumes;

  /* never ignore drives with removable media */
  if (hal_device_get_property_bool (d, "storage.removable"))
    return FALSE;

  drive_udi = hal_device_get_udi (d);

  volumes = hal_pool_find_by_capability (pool, "volume");

  all_volumes_ignored = TRUE;
  got_volumes = FALSE;
  for (l = volumes; l != NULL; l = l->next)
    {
      HalDevice *volume_dev = l->data;
      if (strcmp (drive_udi, hal_device_get_property_string (volume_dev, "block.storage_device")) == 0)
        {
          got_volumes = TRUE;
          if (!should_volume_be_ignored (pool, volume_dev, fstab_mount_points) ||
              hal_device_get_property_bool (volume_dev, "volume.disc.has_audio") ||
              hal_device_get_property_bool (volume_dev, "volume.disc.is_blank"))
            {
              all_volumes_ignored = FALSE;
              break;
            }
        }
    }

  return got_volumes && all_volumes_ignored;
}

static void
list_emit (GHalVolumeMonitor *monitor,
           const char *monitor_signal,
           const char *object_signal,
           GList *objects)
{
  GList *l;

  for (l = objects; l != NULL; l = l->next)
    {
      g_signal_emit_by_name (monitor, monitor_signal, l->data);
      if (object_signal)
        g_signal_emit_by_name (l->data, object_signal);
    }
}

typedef struct {
  GHalVolumeMonitor *monitor;
  GList *added_drives, *removed_drives;
  GList *added_volumes, *removed_volumes;
  GList *added_mounts, *removed_mounts;
} ChangedLists;


static gboolean
emit_lists_in_idle (gpointer data)
{
  ChangedLists *lists = data;

  list_emit (lists->monitor,
             "drive_disconnected", NULL,
             lists->removed_drives);
  list_emit (lists->monitor,
             "drive_connected", NULL,
             lists->added_drives);

  list_emit (lists->monitor,
             "volume_removed", "removed",
             lists->removed_volumes);
  list_emit (lists->monitor,
             "volume_added", NULL,
             lists->added_volumes);

  list_emit (lists->monitor,
             "mount_removed", "unmounted",
             lists->removed_mounts);
  list_emit (lists->monitor,
             "mount_added", NULL,
             lists->added_mounts);
  
  list_free (lists->removed_drives);
  list_free (lists->added_drives);
  list_free (lists->removed_volumes);
  list_free (lists->added_volumes);
  list_free (lists->removed_mounts);
  list_free (lists->added_mounts);
  g_object_unref (lists->monitor);
  g_free (lists);
  
  return FALSE;
}

/* Must be called from idle if emit_changes, with no locks held */
static void
update_all (GHalVolumeMonitor *monitor,
            gboolean emit_changes,
            gboolean emit_in_idle)
{
  ChangedLists *lists;
  GList *added_drives, *removed_drives;
  GList *added_volumes, *removed_volumes;
  GList *added_mounts, *removed_mounts;

  added_drives = NULL;
  removed_drives = NULL;
  added_volumes = NULL;
  removed_volumes = NULL;
  added_mounts = NULL;
  removed_mounts = NULL;
  
  update_drives (monitor, &added_drives, &removed_drives);
  update_volumes (monitor, &added_volumes, &removed_volumes);
  update_mounts (monitor, &added_mounts, &removed_mounts);
  update_discs (monitor,
                &added_volumes, &removed_volumes,
                &added_mounts, &removed_mounts);

  if (emit_changes)
    {
      lists = g_new0 (ChangedLists, 1);
      lists->monitor = g_object_ref (monitor);
      lists->added_drives = added_drives;
      lists->removed_drives = removed_drives;
      lists->added_volumes = added_volumes;
      lists->removed_volumes = removed_volumes;
      lists->added_mounts = added_mounts;
      lists->removed_mounts = removed_mounts;
      
      if (emit_in_idle)
        g_idle_add (emit_lists_in_idle, lists);
      else
        emit_lists_in_idle (lists);
    }
  else
    {
      list_free (removed_drives);
      list_free (added_drives);
      list_free (removed_volumes);
      list_free (added_volumes);
      list_free (removed_mounts);
      list_free (added_mounts);
    }
}

static void
update_drives (GHalVolumeMonitor *monitor,
               GList **added_drives,
               GList **removed_drives)
{
  GList *new_drive_devices;
  GList *removed, *added;
  GList *l, *ll;
  GHalDrive *drive;
  GList *fstab_mount_points;

  fstab_mount_points = g_unix_mount_points_get (NULL);

  new_drive_devices = hal_pool_find_by_capability (monitor->pool, "storage");

  /* remove devices we want to ignore - we do it here so we get to reevaluate
   * on the next update whether they should still be ignored
   */
  for (l = new_drive_devices; l != NULL; l = ll)
    {
      HalDevice *d = l->data;
      ll = l->next;
      if (should_drive_be_ignored (monitor->pool, d, fstab_mount_points))
        new_drive_devices = g_list_delete_link (new_drive_devices, l);
    }

  g_list_foreach (new_drive_devices, (GFunc) g_object_ref, NULL);

  new_drive_devices = g_list_sort (new_drive_devices, (GCompareFunc) hal_device_compare);
  diff_sorted_lists (monitor->last_drive_devices,
                     new_drive_devices, (GCompareFunc) hal_device_compare,
                     &added, &removed);
  
  for (l = removed; l != NULL; l = l->next)
    {
      HalDevice *d = l->data;
      
      drive = find_drive_by_udi (monitor, hal_device_get_udi (d));
      if (drive != NULL)
        {
          /*g_warning ("hal removing drive %s", hal_device_get_property_string (d, "block.device"));*/
          g_hal_drive_disconnected (drive);
          monitor->drives = g_list_remove (monitor->drives, drive);
          *removed_drives = g_list_prepend (*removed_drives, drive);
        }
    }
  
  for (l = added; l != NULL; l = l->next)
    {
      HalDevice *d = l->data;

      drive = find_drive_by_udi (monitor, hal_device_get_udi (d));
      if (drive == NULL)
        {
          /*g_warning ("hal adding drive %s", hal_device_get_property_string (d, "block.device"));*/
          drive = g_hal_drive_new (G_VOLUME_MONITOR (monitor), d, monitor->pool);
          if (drive != NULL)
            {
              monitor->drives = g_list_prepend (monitor->drives, drive);
              *added_drives = g_list_prepend (*added_drives, g_object_ref (drive));
            }
        }
    }
  
  g_list_free (added);
  g_list_free (removed);
  list_free (monitor->last_drive_devices);
  monitor->last_drive_devices = new_drive_devices;

  g_list_foreach (fstab_mount_points, (GFunc) g_unix_mount_point_free, NULL);
  g_list_free (fstab_mount_points);
}

static void
update_volumes (GHalVolumeMonitor *monitor,
                GList **added_volumes,
                GList **removed_volumes)
{
  GList *new_volume_devices;
  GList *removed, *added;
  GList *l, *ll;
  GHalVolume *volume;
  GHalDrive *drive;
  GList *fstab_mount_points;

  fstab_mount_points = g_unix_mount_points_get (NULL);

  new_volume_devices = hal_pool_find_by_capability (monitor->pool, "volume");

  /* remove devices we want to ignore - we do it here so we get to reevaluate
   * on the next update whether they should still be ignored
   */
  for (l = new_volume_devices; l != NULL; l = ll)
    {
      HalDevice *d = l->data;
      ll = l->next;
      if (should_volume_be_ignored (monitor->pool, d, fstab_mount_points))
        new_volume_devices = g_list_delete_link (new_volume_devices, l);
    }

  g_list_foreach (new_volume_devices, (GFunc) g_object_ref, NULL);

  new_volume_devices = g_list_sort (new_volume_devices, (GCompareFunc) hal_device_compare);
  diff_sorted_lists (monitor->last_volume_devices,
                     new_volume_devices, (GCompareFunc) hal_device_compare,
                     &added, &removed);
  
  for (l = removed; l != NULL; l = l->next)
    {
      HalDevice *d = l->data;

      volume = find_volume_by_udi (monitor, hal_device_get_udi (d));
      if (volume != NULL)
        {
          /*g_warning ("hal removing vol %s", hal_device_get_property_string (d, "block.device"));*/
          g_hal_volume_removed (volume);
          monitor->volumes = g_list_remove (monitor->volumes, volume);

          *removed_volumes = g_list_prepend (*removed_volumes, volume);
        }
    }
  
  for (l = added; l != NULL; l = l->next)
    {
      HalDevice *d = l->data;
      
      volume = find_volume_by_udi (monitor, hal_device_get_udi (d));
      if (volume == NULL)
        {
          drive = find_drive_by_udi (monitor, hal_device_get_property_string (d, "block.storage_device"));
          
          /*g_warning ("hal adding vol %s (drive %p)", hal_device_get_property_string (d, "block.device"), drive);*/
          volume = g_hal_volume_new (G_VOLUME_MONITOR (monitor), 
                                     d, 
                                     monitor->pool, 
                                     NULL, 
                                     TRUE, 
                                     drive);
          if (volume != NULL)
            {
              monitor->volumes = g_list_prepend (monitor->volumes, volume);
              *added_volumes = g_list_prepend (*added_volumes, g_object_ref (volume));
            }
        }
    }
  
  g_list_free (added);
  g_list_free (removed);
  list_free (monitor->last_volume_devices);
  monitor->last_volume_devices = new_volume_devices;

  g_list_foreach (fstab_mount_points, (GFunc) g_unix_mount_point_free, NULL);
  g_list_free (fstab_mount_points);
}

static void
update_mounts (GHalVolumeMonitor *monitor,
               GList **added_mounts,
               GList **removed_mounts)
{
  GList *new_mounts;
  GList *removed, *added;
  GList *l, *ll;
  GHalMount *mount;
  GHalVolume *volume;
  const char *device_path;
  const char *mount_path;
  
  new_mounts = g_unix_mounts_get (NULL);

  /* remove mounts we want to ignore - we do it here so we get to reevaluate
   * on the next update whether they should still be ignored
   */
  for (l = new_mounts; l != NULL; l = ll)
    {
      GUnixMountEntry *mount_entry = l->data;
      ll = l->next;

      /* keep in sync with should_mount_be_ignored() */
      if (!g_unix_mount_guess_should_display (mount_entry))
        {
          g_unix_mount_free (mount_entry);
          new_mounts = g_list_delete_link (new_mounts, l);
        }
    }
  
  new_mounts = g_list_sort (new_mounts, (GCompareFunc) g_unix_mount_compare);
  
  diff_sorted_lists (monitor->last_mounts,
		     new_mounts, (GCompareFunc) g_unix_mount_compare,
		     &added, &removed);
  
  for (l = removed; l != NULL; l = l->next)
    {
      GUnixMountEntry *mount_entry = l->data;
      
      mount = find_mount_by_mount_path (monitor, g_unix_mount_get_mount_path (mount_entry));
      /*g_warning ("hal removing mount %s (%p)", g_unix_mount_get_device_path (mount_entry), mount);*/
      if (mount)
	{
	  g_hal_mount_unmounted (mount);
	  monitor->mounts = g_list_remove (monitor->mounts, mount);

          *removed_mounts = g_list_prepend (*removed_mounts, mount);
	}
    }
  
  for (l = added; l != NULL; l = l->next)
    {
      GUnixMountEntry *mount_entry = l->data;

      device_path = g_unix_mount_get_device_path (mount_entry);
      mount_path = g_unix_mount_get_mount_path (mount_entry);
      volume = lookup_volume_for_device_path (monitor, device_path);
      if (volume == NULL)
        volume = lookup_volume_for_mount_path (monitor, mount_path);

      /*g_warning ("hal adding mount %s (vol %p)", g_unix_mount_get_device_path (mount_entry), volume);*/
      mount = g_hal_mount_new (G_VOLUME_MONITOR (monitor), mount_entry, monitor->pool, volume);
      if (mount)
	{
	  monitor->mounts = g_list_prepend (monitor->mounts, mount);
          *added_mounts = g_list_prepend (*added_mounts, g_object_ref (mount));
	}
    }
  
  g_list_free (added);
  g_list_free (removed);
  g_list_foreach (monitor->last_mounts,
		  (GFunc)g_unix_mount_free, NULL);
  g_list_free (monitor->last_mounts);
  monitor->last_mounts = new_mounts;
}

static void
update_discs (GHalVolumeMonitor *monitor,
              GList **added_volumes,
              GList **removed_volumes,
              GList **added_mounts,
              GList **removed_mounts)
{
  GList *new_optical_disc_devices;
  GList *removed, *added;
  GList *l, *ll;
  GHalDrive *drive;
  GHalVolume *volume;
  GHalMount *mount;
  const char *udi;
  const char *drive_udi;

  /* we also need to generate GVolume + GMount objects for
   *
   * - optical discs that have audio
   * - optical discs that are blank
   *
   */

  new_optical_disc_devices = hal_pool_find_by_capability (monitor->pool, "volume.disc");
  for (l = new_optical_disc_devices; l != NULL; l = ll)
    {
      HalDevice *d = l->data;
      ll = l->next;
      if (! (hal_device_get_property_bool (d, "volume.disc.is_blank") ||
             hal_device_get_property_bool (d, "volume.disc.has_audio")))
        {
          /* filter out everything but discs that are blank or has audio */
          new_optical_disc_devices = g_list_delete_link (new_optical_disc_devices, l);
        }
    }

  g_list_foreach (new_optical_disc_devices, (GFunc) g_object_ref, NULL);

  new_optical_disc_devices = g_list_sort (new_optical_disc_devices, (GCompareFunc) hal_device_compare);
  diff_sorted_lists (monitor->last_optical_disc_devices,
                     new_optical_disc_devices, (GCompareFunc) hal_device_compare,
                     &added, &removed);

  for (l = removed; l != NULL; l = l->next)
    {
      HalDevice *d = l->data;

      udi = hal_device_get_udi (d);
      /*g_warning ("audio/blank disc removing %s", udi);*/

      mount = find_disc_mount_by_udi (monitor, udi);
      if (mount != NULL)
        {
	  g_hal_mount_unmounted (mount);
	  monitor->disc_mounts = g_list_remove (monitor->disc_mounts, mount);
          *removed_mounts = g_list_prepend (*removed_mounts, mount);
        }

      volume = find_disc_volume_by_udi (monitor, udi);
      if (volume != NULL)
        {
	  g_hal_volume_removed (volume);
	  monitor->disc_volumes = g_list_remove (monitor->disc_volumes, volume);
          *removed_volumes = g_list_prepend (*removed_volumes, volume);
        }
    }
  
  for (l = added; l != NULL; l = l->next)
    {
      HalDevice *d = l->data;

      udi = hal_device_get_udi (d);
      /*g_warning ("audio/blank disc adding %s", udi);*/

      drive_udi = hal_device_get_property_string (d, "block.storage_device");
      drive = find_drive_by_udi (monitor, drive_udi);
      if (drive != NULL)
        {
          mount = NULL;
          if (hal_device_get_property_bool (d, "volume.disc.is_blank"))
            {
              volume = g_hal_volume_new (G_VOLUME_MONITOR (monitor), 
                                         d, monitor->pool, 
                                         NULL, 
                                         FALSE, 
                                         drive);
              if (volume != NULL)
                {
                  GFile *root;
                  root = g_file_new_for_uri ("burn:///");
                  mount = g_hal_mount_new_for_hal_device (G_VOLUME_MONITOR (monitor), 
                                                          d, 
                                                          root,
                                                          NULL,
                                                          NULL,
                                                          TRUE,
                                                          monitor->pool,
                                                          volume);
                  g_object_unref (root);
                }
            }
          else
            {
              char *uri;
              char *device_basename;
              GFile *foreign_mount_root;

              /* the gvfsd-cdda backend uses URI's like these */
              device_basename = g_path_get_basename (hal_device_get_property_string (d, "block.device"));
              uri = g_strdup_printf ("cdda://%s", device_basename);
              foreign_mount_root = g_file_new_for_uri (uri);
              g_free (device_basename);
              g_free (uri);

              volume = g_hal_volume_new (G_VOLUME_MONITOR (monitor), 
                                         d, 
                                         monitor->pool, 
                                         foreign_mount_root, 
                                         TRUE, 
                                         drive);
              g_object_unref (foreign_mount_root);
              mount = NULL;
            }

          if (volume != NULL)
            {
              monitor->disc_volumes = g_list_prepend (monitor->disc_volumes, volume);
              *added_volumes = g_list_prepend (*added_volumes, g_object_ref (volume));
              
              if (mount != NULL)
                {
                  monitor->disc_mounts = g_list_prepend (monitor->disc_mounts, mount);
                  *added_mounts = g_list_prepend (*added_mounts, g_object_ref (mount));
                }
            }
        }
    }

  g_list_free (added);
  g_list_free (removed);
  list_free (monitor->last_optical_disc_devices);
  monitor->last_optical_disc_devices = new_optical_disc_devices;
}
