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
#include <gphoto2.h>
#include <gio/gio.h>

#include "ggphoto2volumemonitor.h"
#include "ggphoto2volume.h"

#ifdef HAVE_GUDEV
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#else
#include "hal-pool.h"
#endif

G_LOCK_DEFINE_STATIC(hal_vm);

static GGPhoto2VolumeMonitor *the_volume_monitor = NULL;
#ifndef HAVE_GUDEV
static HalPool *pool = NULL;
#endif

struct _GGPhoto2VolumeMonitor {
  GNativeVolumeMonitor parent;

  GUnixMountMonitor *mount_monitor;

#ifdef HAVE_GUDEV
  GUdevClient *gudev_client;
#else
  HalPool *pool;
#endif

  GList *last_camera_devices;

  GList *camera_volumes;
};

#ifdef HAVE_GUDEV
static void on_uevent                (GUdevClient *client, 
                                      gchar *action,
                                      GUdevDevice *device,
                                      gpointer user_data);
#else
static void hal_changed              (HalPool    *pool,
                                      HalDevice  *device,
                                      gpointer    user_data);

static void update_all (GGPhoto2VolumeMonitor *monitor,
                        gboolean emit_changes);

static void update_cameras           (GGPhoto2VolumeMonitor *monitor,
                                      GList **added_volumes,
                                      GList **removed_volumes);
#endif

static GList* get_stores_for_camera (int bus_num, int device_num);

G_DEFINE_TYPE (GGPhoto2VolumeMonitor, g_gphoto2_volume_monitor, G_TYPE_VOLUME_MONITOR)

static void
list_free (GList *objects)
{
  g_list_foreach (objects, (GFunc)g_object_unref, NULL);
  g_list_free (objects);
}

#ifndef HAVE_GUDEV
static HalPool *
get_hal_pool (void)
{
  char *cap_only[] = {"camera", "portable_audio_player", "usb_device", NULL};

  if (pool == NULL)
    pool = hal_pool_new (cap_only);

  return pool;
}
#endif

static void
g_gphoto2_volume_monitor_dispose (GObject *object)
{
  GGPhoto2VolumeMonitor *monitor;

  monitor = G_GPHOTO2_VOLUME_MONITOR (object);

  G_LOCK (hal_vm);
  the_volume_monitor = NULL;
  G_UNLOCK (hal_vm);

  if (G_OBJECT_CLASS (g_gphoto2_volume_monitor_parent_class)->dispose)
    (*G_OBJECT_CLASS (g_gphoto2_volume_monitor_parent_class)->dispose) (object);
}

static void
g_gphoto2_volume_monitor_finalize (GObject *object)
{
  GGPhoto2VolumeMonitor *monitor;

  monitor = G_GPHOTO2_VOLUME_MONITOR (object);

#ifdef HAVE_GUDEV
  g_signal_handlers_disconnect_by_func (monitor->gudev_client, on_uevent, monitor);

  g_object_unref (monitor->gudev_client);
#else
  g_signal_handlers_disconnect_by_func (monitor->pool, hal_changed, monitor);
  g_object_unref (monitor->pool);
#endif

  list_free (monitor->last_camera_devices);
  list_free (monitor->camera_volumes);

  if (G_OBJECT_CLASS (g_gphoto2_volume_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_gphoto2_volume_monitor_parent_class)->finalize) (object);
}

static GList *
get_mounts (GVolumeMonitor *volume_monitor)
{
  return NULL;
}

static GList *
get_volumes (GVolumeMonitor *volume_monitor)
{
  GGPhoto2VolumeMonitor *monitor;
  GList *l;

  monitor = G_GPHOTO2_VOLUME_MONITOR (volume_monitor);

  G_LOCK (hal_vm);

  l = g_list_copy (monitor->camera_volumes);
  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  G_UNLOCK (hal_vm);

  return l;
}

static GList *
get_connected_drives (GVolumeMonitor *volume_monitor)
{
  return NULL;
}

static GVolume *
get_volume_for_uuid (GVolumeMonitor *volume_monitor, const char *uuid)
{
  return NULL;
}

static GMount *
get_mount_for_uuid (GVolumeMonitor *volume_monitor, const char *uuid)
{
  return NULL;
}

#ifdef HAVE_GUDEV

static void
gudev_add_camera (GGPhoto2VolumeMonitor *monitor, GUdevDevice *device, gboolean do_emit)
{
    GGPhoto2Volume *volume;
    GList *store_heads, *l;
    guint num_store_heads;
    const char *property;
    int usb_bus_num;
    int usb_device_num;

  /* For iPhones and iPod Touches, don't mount gphoto mounts,
   * we already have access through AFC */
#ifdef HAVE_AFC
    if (g_udev_device_get_property_as_boolean (device, "USBMUX_SUPPORTED"))
      {
	/* g_debug ("ignoring device, is AFC"); */
	return;
      }
#endif /* HAVE_AFC */

    property = g_udev_device_get_property (device, "BUSNUM");
    if (property == NULL) {
	g_warning("device %s has no BUSNUM property, ignoring", g_udev_device_get_device_file (device));
	return;
    }
    usb_bus_num = atoi (property);

    property = g_udev_device_get_property (device, "DEVNUM");
    if (property == NULL) {
	g_warning("device %s has no DEVNUM property, ignoring", g_udev_device_get_device_file (device));
	return;
    }
    usb_device_num = atoi (property);

    /* g_debug ("gudev_add_camera: camera device %s (bus: %i, device: %i)", 
             g_udev_device_get_device_file (device),
             usb_bus_num, usb_device_num); */

    store_heads = get_stores_for_camera (usb_bus_num, usb_device_num);
    num_store_heads = g_list_length (store_heads);
    for (l = store_heads ; l != NULL; l = l->next)
      {
        char *store_path = (char *) l->data;
        GFile *activation_mount_root;
        gchar *uri;

        /* If we only have a single store, don't use the store name at all. The backend automatically
         * prepend the storename; this is to work around bugs with devices (like the iPhone) for which
         * the store name changes every time the camera is initialized (e.g. mounted).
         */
        if (num_store_heads == 1)
          {
            uri = g_strdup_printf ("gphoto2://[usb:%03d,%03d]", usb_bus_num, usb_device_num);
          }
        else
          {
            uri = g_strdup_printf ("gphoto2://[usb:%03d,%03d]/%s", usb_bus_num, usb_device_num,
                                   store_path[0] == '/' ? store_path + 1 : store_path);
          }
        /* g_debug ("gudev_add_camera: ... adding URI for storage head: %s", uri); */
        activation_mount_root = g_file_new_for_uri (uri);
        g_free (uri);

        volume = g_gphoto2_volume_new (G_VOLUME_MONITOR (monitor),
                                       device, 
                                       monitor->gudev_client,
                                       activation_mount_root);
        if (volume != NULL)
          {
            monitor->camera_volumes = g_list_prepend (monitor->camera_volumes, volume);
            if (do_emit)
                g_signal_emit_by_name (monitor, "volume_added", volume);
          }

        if (activation_mount_root != NULL)
          g_object_unref (activation_mount_root);
      }

    g_list_foreach (store_heads, (GFunc) g_free, NULL);
    g_list_free (store_heads);
}

static void
gudev_remove_camera (GGPhoto2VolumeMonitor *monitor, GUdevDevice *device)
{
  GList *l, *ll;
  const gchar* sysfs_path;

  sysfs_path = g_udev_device_get_sysfs_path (device);

  /* g_debug ("gudev_remove_camera: %s", g_udev_device_get_device_file (device)); */

  for (l = monitor->camera_volumes; l != NULL; l = ll)
    {
      GGPhoto2Volume *volume = G_GPHOTO2_VOLUME (l->data);

      ll = l->next;

      if (g_gphoto2_volume_has_path (volume, sysfs_path))
        {
          /* g_debug ("gudev_remove_camera: found volume %s, deleting", sysfs_path); */
          g_signal_emit_by_name (monitor, "volume_removed", volume);
          g_signal_emit_by_name (volume, "removed");
          g_gphoto2_volume_removed (volume);
          monitor->camera_volumes = g_list_remove (monitor->camera_volumes, volume);
          g_object_unref (volume);
        }
    }
}

static void
on_uevent (GUdevClient *client, 
           gchar *action,
           GUdevDevice *device,
           gpointer user_data)
{
  GGPhoto2VolumeMonitor *monitor = G_GPHOTO2_VOLUME_MONITOR (user_data);

  /* g_debug ("on_uevent: action=%s, device=%s", action, g_udev_device_get_device_file(device)); */

  /* filter out uninteresting events */
  if (!g_udev_device_has_property (device, "ID_GPHOTO2"))
    {
      /* g_debug ("on_uevent: discarding, not ID_GPHOTO2"); */
      return;
    }

  if (strcmp (action, "add") == 0)
     gudev_add_camera (monitor, device, TRUE); 
  else if (strcmp (action, "remove") == 0)
     gudev_remove_camera (monitor, device); 
}

/* Find all attached gphoto supported cameras; this is called on startup
 * (coldplugging). */
static void
gudev_coldplug_cameras (GGPhoto2VolumeMonitor *monitor)
{
    GList *usb_devices, *l;

    usb_devices = g_udev_client_query_by_subsystem (monitor->gudev_client, "usb");
    for (l = usb_devices; l != NULL; l = l->next)
    {
        GUdevDevice *d = l->data;
        if (g_udev_device_has_property (d, "ID_GPHOTO2"))
            gudev_add_camera (monitor, d, FALSE);
    }
}

#else

static void
hal_changed (HalPool    *pool,
             HalDevice  *device,
             gpointer    user_data)
{
  GGPhoto2VolumeMonitor *monitor = G_GPHOTO2_VOLUME_MONITOR (user_data);

  /*g_warning ("hal changed");*/

  update_all (monitor, TRUE);
}
#endif

static GObject *
g_gphoto2_volume_monitor_constructor (GType                  type,
                                  guint                  n_construct_properties,
                                  GObjectConstructParam *construct_properties)
{
  GObject *object;
  GGPhoto2VolumeMonitor *monitor;
  GGPhoto2VolumeMonitorClass *klass;
  GObjectClass *parent_class;

  G_LOCK (hal_vm);
  if (the_volume_monitor != NULL)
    {
      object = g_object_ref (the_volume_monitor);
      G_UNLOCK (hal_vm);
      return object;
    }
  G_UNLOCK (hal_vm);

  /*g_warning ("creating hal vm");*/

  object = NULL;

  /* Invoke parent constructor. */
  klass = G_GPHOTO2_VOLUME_MONITOR_CLASS (g_type_class_peek (G_TYPE_GPHOTO2_VOLUME_MONITOR));
  parent_class = G_OBJECT_CLASS (g_type_class_peek_parent (klass));
  object = parent_class->constructor (type,
                                      n_construct_properties,
                                      construct_properties);

  monitor = G_GPHOTO2_VOLUME_MONITOR (object);

#ifdef HAVE_GUDEV
  const char *subsystems[] = {"usb", NULL};
  monitor->gudev_client = g_udev_client_new (subsystems);

  g_signal_connect (monitor->gudev_client, 
                    "uevent", G_CALLBACK (on_uevent), 
                    monitor);

  gudev_coldplug_cameras (monitor);

#else
  monitor->pool = g_object_ref (get_hal_pool ());

  g_signal_connect (monitor->pool,
                    "device_added", G_CALLBACK (hal_changed),
                    monitor);

  g_signal_connect (monitor->pool,
                    "device_removed", G_CALLBACK (hal_changed),
                    monitor);

  update_all (monitor, FALSE);
#endif

  G_LOCK (hal_vm);
  the_volume_monitor = monitor;
  G_UNLOCK (hal_vm);

  return object;
}

static void
g_gphoto2_volume_monitor_init (GGPhoto2VolumeMonitor *monitor)
{
}

static gboolean
is_supported (void)
{
#ifdef HAVE_GUDEV
  /* Today's Linux desktops pretty much need udev to have anything working, so
   * assume it's there */
  return TRUE;
#else
  return get_hal_pool() != NULL;
#endif
}

static void
g_gphoto2_volume_monitor_class_init (GGPhoto2VolumeMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVolumeMonitorClass *monitor_class = G_VOLUME_MONITOR_CLASS (klass);

  gobject_class->constructor = g_gphoto2_volume_monitor_constructor;
  gobject_class->finalize = g_gphoto2_volume_monitor_finalize;
  gobject_class->dispose = g_gphoto2_volume_monitor_dispose;

  monitor_class->get_mounts = get_mounts;
  monitor_class->get_volumes = get_volumes;
  monitor_class->get_connected_drives = get_connected_drives;
  monitor_class->get_volume_for_uuid = get_volume_for_uuid;
  monitor_class->get_mount_for_uuid = get_mount_for_uuid;
  monitor_class->is_supported = is_supported;
}

/**
 * g_gphoto2_volume_monitor_new:
 *
 * Returns:  a new #GVolumeMonitor.
 **/
GVolumeMonitor *
g_gphoto2_volume_monitor_new (void)
{
  GGPhoto2VolumeMonitor *monitor;

  monitor = g_object_new (G_TYPE_GPHOTO2_VOLUME_MONITOR, NULL);

  return G_VOLUME_MONITOR (monitor);
}

#ifndef HAVE_GUDEV
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

static GGPhoto2Volume *
find_camera_volume_by_udi (GGPhoto2VolumeMonitor *monitor, const char *udi)
{
  GList *l;

  for (l = monitor->camera_volumes; l != NULL; l = l->next)
    {
      GGPhoto2Volume *volume = l->data;

      if (g_gphoto2_volume_has_udi (volume, udi))
        return volume;
    }

  return NULL;
}

static gint
hal_device_compare (HalDevice *a, HalDevice *b)
{
  return strcmp (hal_device_get_udi (a), hal_device_get_udi (b));
}

static void
list_emit (GGPhoto2VolumeMonitor *monitor,
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
  GGPhoto2VolumeMonitor *monitor;
  GList *added_volumes, *removed_volumes;
} ChangedLists;


static gboolean
emit_lists_in_idle (gpointer data)
{
  ChangedLists *lists = data;

  list_emit (lists->monitor,
             "volume_removed", "removed",
             lists->removed_volumes);
  list_emit (lists->monitor,
             "volume_added", NULL,
             lists->added_volumes);

  list_free (lists->removed_volumes);
  list_free (lists->added_volumes);
  g_object_unref (lists->monitor);
  g_free (lists);

  return FALSE;
}

/* Must be called from idle if emit_changes, with no locks held */
static void
update_all (GGPhoto2VolumeMonitor *monitor,
            gboolean emit_changes)
{
  ChangedLists *lists;
  GList *added_volumes, *removed_volumes;

  added_volumes = NULL;
  removed_volumes = NULL;

  G_LOCK (hal_vm);
  update_cameras (monitor, &added_volumes, &removed_volumes);
  G_UNLOCK (hal_vm);

  if (emit_changes)
    {
      lists = g_new0 (ChangedLists, 1);
      lists->monitor = g_object_ref (monitor);
      lists->added_volumes = added_volumes;
      lists->removed_volumes = removed_volumes;

      g_idle_add (emit_lists_in_idle, lists);
    }
  else
    {
      list_free (removed_volumes);
      list_free (added_volumes);
    }
}
#endif

static GList *
get_stores_for_camera (int bus_num, int device_num)
{
  GList *l;
  CameraStorageInformation *storage_info;
  GPContext *context;
  GPPortInfo info;
  GPPortInfoList *il;
  int num_storage_info, n;
  Camera *camera;
  char *port;
  guint i;

  il = NULL;
  camera = NULL;
  context = NULL;
  l = NULL;
  port = g_strdup_printf ("usb:%d,%d", bus_num, device_num);

  /* Connect to the camera */
  context = gp_context_new ();
  if (gp_camera_new (&camera) != 0)
    goto out;
  if (gp_port_info_list_new (&il) != 0)
    goto out;
  if (gp_port_info_list_load (il) != 0)
    goto out;
  n = gp_port_info_list_lookup_path (il, port);
  if (n == GP_ERROR_UNKNOWN_PORT)
    goto out;
  if (gp_port_info_list_get_info (il, n, &info) != 0)
    goto out;
  if (gp_camera_set_port_info (camera, info) != 0)
    goto out;
  gp_port_info_list_free (il);
  il = NULL;
  if (gp_camera_init (camera, context) != 0)
    goto out;

  /* Get information about the storage heads */
  if (gp_camera_get_storageinfo (camera, &storage_info, &num_storage_info, context) != 0)
    goto out;

  /* Append the data to the list */
  for (i = 0; i < num_storage_info; i++)
    {
      const gchar *basedir;

      /* Ignore storage with no capacity (see bug 570888) */
      if ((storage_info[i].fields & GP_STORAGEINFO_MAXCAPACITY) &&
          storage_info[i].capacitykbytes == 0)
        continue;

      /* Some cameras, such as the Canon 5D, won't report the basedir */
      if (storage_info[i].fields & GP_STORAGEINFO_BASE)
        basedir = storage_info[i].basedir;
      else
        basedir = "/";

      /* g_debug ("capacitykbytes[%d] = %d", i, (gint) storage_info[i].capacitykbytes); */

      l = g_list_prepend (l, g_strdup (basedir));
    }

out:
  /* Clean up */
  if (il != NULL)
    gp_port_info_list_free (il);
  if (context != NULL)
    gp_context_unref (context);
  if (camera != NULL)
    gp_camera_unref (camera);

  g_free (port);

  return l;
}

#ifndef HAVE_GUDEV
static void
update_cameras (GGPhoto2VolumeMonitor *monitor,
                GList **added_volumes,
                GList **removed_volumes)
{
  GList *new_camera_devices;
  GList *new_mtp_devices;
  GList *removed, *added;
  GList *l, *ll;
  GGPhoto2Volume *volume;
  const char *udi;

  new_mtp_devices = hal_pool_find_by_capability (monitor->pool, "portable_audio_player");
  for (l = new_mtp_devices; l != NULL; l = ll)
    {
      HalDevice *d = l->data;
      ll = l->next;
      if (! hal_device_get_property_bool (d, "camera.libgphoto2.support"))
        {
          /*g_warning ("ignoring %s", hal_device_get_udi (d));*/
          /* filter out everything that isn't supported by libgphoto2 */
          new_mtp_devices = g_list_delete_link (new_mtp_devices, l);
        }
    }

  new_camera_devices = hal_pool_find_by_capability (monitor->pool, "camera");
  new_camera_devices = g_list_concat (new_camera_devices, new_mtp_devices);
  for (l = new_camera_devices; l != NULL; l = ll)
    {
      HalDevice *d = l->data;
      ll = l->next;
      /*g_warning ("got %s", hal_device_get_udi (d));*/
      if (! hal_device_get_property_bool (d, "camera.libgphoto2.support"))
        {
          /*g_warning ("ignoring %s", hal_device_get_udi (d));*/
          /* filter out everything that isn't supported by libgphoto2 */
          new_camera_devices = g_list_delete_link (new_camera_devices, l);
        }
    }
  g_list_foreach (new_camera_devices, (GFunc) g_object_ref, NULL);

  new_camera_devices = g_list_sort (new_camera_devices, (GCompareFunc) hal_device_compare);
  diff_sorted_lists (monitor->last_camera_devices,
                     new_camera_devices, (GCompareFunc) hal_device_compare,
                     &added, &removed);

  for (l = removed; l != NULL; l = l->next)
    {
      HalDevice *d = l->data;

      udi = hal_device_get_udi (d);
      /*g_warning ("camera removing %s", udi);*/

      volume = find_camera_volume_by_udi (monitor, udi);
      if (volume != NULL)
        {
          g_gphoto2_volume_removed (volume);
          monitor->camera_volumes = g_list_remove (monitor->camera_volumes, volume);
          *removed_volumes = g_list_prepend (*removed_volumes, volume);
        }
    }

  for (l = added; l != NULL; l = l->next)
    {
      HalDevice *d = l->data;
      int usb_bus_num;
      int usb_device_num;
      gboolean found;
      GList *store_heads, *l;
      guint num_store_heads;

      /* Look for the device in the added volumes, so as
       * not to add devices that are both audio players, and cameras */
      found = FALSE;
      for (ll = *added_volumes; ll; ll = ll->next)
        {
          if (g_gphoto2_volume_has_udi (ll->data, hal_device_get_udi (d)) != FALSE)
            {
              found = TRUE;
              break;
            }
        }

      if (found)
        continue;

      usb_bus_num = hal_device_get_property_int (d, "usb.bus_number");
#if defined(__linux__)
      usb_device_num = hal_device_get_property_int (d, "usb.linux.device_number");
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
      usb_device_num = hal_device_get_property_int (d, "freebsd.unit");
#else
# error "Need OS specific tweaks"
#endif

      store_heads = get_stores_for_camera (usb_bus_num, usb_device_num);
      num_store_heads = g_list_length (store_heads);
      for (l = store_heads ; l != NULL; l = l->next)
        {
          char *store_path = (char *) l->data;
          GFile *activation_mount_root;
          gchar *uri;

          /* If we only have a single store, don't use the store name at all. The backend automatically
           * prepend the storename; this is to work around bugs with devices (like the iPhone) for which
           * the store name changes every time the camera is initialized (e.g. mounted).
           */
          if (num_store_heads == 1)
            {
              uri = g_strdup_printf ("gphoto2://[usb:%03d,%03d]", usb_bus_num, usb_device_num);
            }
          else
            {
              uri = g_strdup_printf ("gphoto2://[usb:%03d,%03d]/%s", usb_bus_num, usb_device_num,
                                     store_path[0] == '/' ? store_path + 1 : store_path);
            }
          activation_mount_root = g_file_new_for_uri (uri);
          g_free (uri);

          udi = hal_device_get_udi (d);
          volume = g_gphoto2_volume_new (G_VOLUME_MONITOR (monitor),
                                         d,
                                         monitor->pool,
                                         activation_mount_root);
          if (volume != NULL)
            {
              monitor->camera_volumes = g_list_prepend (monitor->camera_volumes, volume);
              *added_volumes = g_list_prepend (*added_volumes, g_object_ref (volume));
            }

          if (activation_mount_root != NULL)
            g_object_unref (activation_mount_root);
        }
      g_list_foreach (store_heads, (GFunc) g_free, NULL);
      g_list_free (store_heads);
    }

  g_list_free (added);
  g_list_free (removed);
  list_free (monitor->last_camera_devices);
  monitor->last_camera_devices = new_camera_devices;
}
#endif
