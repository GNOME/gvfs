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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
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

#include <gio/gunixmounts.h>

G_LOCK_DEFINE_STATIC(gphoto2_vm);

static GGPhoto2VolumeMonitor *the_volume_monitor = NULL;

struct _GGPhoto2VolumeMonitor {
  GNativeVolumeMonitor parent;

  GUnixMountMonitor *mount_monitor;

  GUdevClient *gudev_client;

  GList *last_camera_devices;

  GList *camera_volumes;
};

static void on_uevent                (GUdevClient *client, 
                                      gchar *action,
                                      GUdevDevice *device,
                                      gpointer user_data);

static GList* get_stores_for_camera (const char *bus_num, const char *device_num);

G_DEFINE_TYPE (GGPhoto2VolumeMonitor, g_gphoto2_volume_monitor, G_TYPE_VOLUME_MONITOR)

static void
g_gphoto2_volume_monitor_dispose (GObject *object)
{
  G_LOCK (gphoto2_vm);
  the_volume_monitor = NULL;
  G_UNLOCK (gphoto2_vm);

  if (G_OBJECT_CLASS (g_gphoto2_volume_monitor_parent_class)->dispose)
    (*G_OBJECT_CLASS (g_gphoto2_volume_monitor_parent_class)->dispose) (object);
}

static void
g_gphoto2_volume_monitor_finalize (GObject *object)
{
  GGPhoto2VolumeMonitor *monitor;

  monitor = G_GPHOTO2_VOLUME_MONITOR (object);

  g_signal_handlers_disconnect_by_func (monitor->gudev_client, on_uevent, monitor);

  g_object_unref (monitor->gudev_client);

  g_list_free_full (monitor->last_camera_devices, g_object_unref);
  g_list_free_full (monitor->camera_volumes, g_object_unref);

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

  G_LOCK (gphoto2_vm);

  l = g_list_copy (monitor->camera_volumes);
  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  G_UNLOCK (gphoto2_vm);

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

static void
gudev_add_camera (GGPhoto2VolumeMonitor *monitor, GUdevDevice *device, gboolean do_emit)
{
    GGPhoto2Volume *volume;
    GList *store_heads, *l;
    guint num_store_heads;
    const char *usb_bus_num, *usb_device_num, *usb_serial_id, *device_path;
    gchar *prefix, *usb_serial_id_escaped;
    GFile *mount_prefix;
    gboolean serial_conflict = FALSE;

    device_path = g_udev_device_get_device_file (device);
    if (!device_path)
      {
        g_debug ("Ignoring device '%s' without a device file",
                 g_udev_device_get_sysfs_path (device));
        return;
      }

#ifdef HAVE_LIBMTP
    if (g_udev_device_get_property_as_boolean (device, "ID_MTP_DEVICE"))
      {
        g_debug ("gudev_add_camera: ignoring device, is MTP");
        return;
      }
#endif /* HAVE_LIBMTP */

    /*
     * We do not use ID_SERIAL_SHORT (the actualy device serial value) as
     * this field is not populated when an ID_SERIAL has to be synthesized.
     */
    usb_serial_id = g_udev_device_get_property (device, "ID_SERIAL");
    if (usb_serial_id == NULL)
      {
        g_warning ("device %s has no ID_SERIAL property, ignoring", device_path);
        return;
      }

    usb_bus_num = g_udev_device_get_property (device, "BUSNUM");
    if (usb_bus_num == NULL)
      {
        g_warning ("device %s has no BUSNUM property, ignoring", device_path);
        return;
      }

    usb_device_num = g_udev_device_get_property (device, "DEVNUM");
    if (usb_device_num == NULL)
      {
        g_warning ("device %s has no DEVNUM property, ignoring", device_path);
        return;
      }

    usb_serial_id_escaped = g_uri_escape_string (usb_serial_id, NULL, FALSE);
    prefix = g_strdup_printf ("gphoto2://%s", usb_serial_id_escaped);
    mount_prefix = g_file_new_for_uri (prefix);
    g_free (prefix);

    /*
     * We do not support plugging in multiple devices that lack proper serial
     * numbers. Linux will attempt to synthesize an ID based on the device
     * product information, which will avoid collisions between different
     * types of device, but two identical, serial-less, devices will still
     * conflict.
     */
    for (l = monitor->camera_volumes; l != NULL; l = l->next)
      {
        GGPhoto2Volume *volume = G_GPHOTO2_VOLUME (l->data);

        GFile *existing_root = g_volume_get_activation_root (G_VOLUME (volume));
        if (g_file_equal (existing_root, mount_prefix) ||
            g_file_has_prefix (existing_root, mount_prefix))
          {
            serial_conflict = TRUE;
          }
        g_object_unref (existing_root);
        if (serial_conflict)
          {
            break;
          }
      }
    g_object_unref (mount_prefix);
    if (serial_conflict)
      {
        g_warning ("device %s has an identical ID_SERIAL value to an "
                   "existing device. Multiple devices are not supported.",
                   g_udev_device_get_device_file (device));
        g_free (usb_serial_id_escaped);
        return;
      }

    g_debug ("gudev_add_camera: camera device %s (id: %s)",
             g_udev_device_get_device_file (device),
             usb_serial_id);

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
            uri = g_strdup_printf ("gphoto2://%s", usb_serial_id_escaped);
          }
        else
          {
            uri = g_strdup_printf ("gphoto2://%s/%s", usb_serial_id_escaped,
                                   store_path[0] == '/' ? store_path + 1 : store_path);
          }
        g_debug ("gudev_add_camera: ... adding URI for storage head: %s", uri);
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

    g_list_free_full (store_heads, g_free);
    g_free (usb_serial_id_escaped);
}

static void
gudev_remove_camera (GGPhoto2VolumeMonitor *monitor, GUdevDevice *device)
{
  GList *l, *ll;
  const gchar* sysfs_path;

  sysfs_path = g_udev_device_get_sysfs_path (device);

  g_debug ("gudev_remove_camera: %s", g_udev_device_get_device_file (device));

  for (l = monitor->camera_volumes; l != NULL; l = ll)
    {
      GGPhoto2Volume *volume = G_GPHOTO2_VOLUME (l->data);

      ll = l->next;

      if (g_gphoto2_volume_has_path (volume, sysfs_path))
        {
          g_debug ("gudev_remove_camera: found volume %s, deleting", sysfs_path);
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

  g_debug ("on_uevent: action=%s, device=%s", action, g_udev_device_get_device_file (device));

  if (g_strcmp0 (action, "add") == 0 && g_udev_device_has_property (device, "ID_GPHOTO2"))
    gudev_add_camera (monitor, device, TRUE);
  else if (g_strcmp0 (action, "remove") == 0)
    gudev_remove_camera (monitor, device);
  else
    g_debug ("on_uevent: discarding");
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
    g_list_free_full (usb_devices, g_object_unref);
}

static GObject *
g_gphoto2_volume_monitor_constructor (GType                  type,
                                  guint                  n_construct_properties,
                                  GObjectConstructParam *construct_properties)
{
  GObject *object;
  GGPhoto2VolumeMonitor *monitor;
  GGPhoto2VolumeMonitorClass *klass;
  GObjectClass *parent_class;

  G_LOCK (gphoto2_vm);
  if (the_volume_monitor != NULL)
    {
      object = G_OBJECT (g_object_ref (the_volume_monitor));
      G_UNLOCK (gphoto2_vm);
      return object;
    }
  G_UNLOCK (gphoto2_vm);

  object = NULL;

  /* Invoke parent constructor. */
  klass = G_GPHOTO2_VOLUME_MONITOR_CLASS (g_type_class_peek (G_TYPE_GPHOTO2_VOLUME_MONITOR));
  parent_class = G_OBJECT_CLASS (g_type_class_peek_parent (klass));
  object = parent_class->constructor (type,
                                      n_construct_properties,
                                      construct_properties);

  monitor = G_GPHOTO2_VOLUME_MONITOR (object);

  const char *subsystems[] = {"usb", NULL};
  monitor->gudev_client = g_udev_client_new (subsystems);

  g_signal_connect (monitor->gudev_client, 
                    "uevent", G_CALLBACK (on_uevent), 
                    monitor);

  gudev_coldplug_cameras (monitor);

  G_LOCK (gphoto2_vm);
  the_volume_monitor = monitor;
  G_UNLOCK (gphoto2_vm);

  return object;
}

static void
g_gphoto2_volume_monitor_init (GGPhoto2VolumeMonitor *monitor)
{
}

static gboolean
is_supported (void)
{
  /* Today's Linux desktops pretty much need udev to have anything working, so
   * assume it's there */
  return TRUE;
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

static GList *
get_stores_for_camera (const char *bus_num, const char *device_num)
{
  GList *l;
  CameraStorageInformation *storage_info;
  GPContext *context;
  GPPortInfo info;
  GPPortInfoList *il;
  int num_storage_info, n, rc;
  Camera *camera;
  char *port;
  guint i;

  il = NULL;
  camera = NULL;
  context = NULL;
  l = NULL;
  port = g_strdup_printf ("usb:%s,%s", bus_num, device_num);

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
  rc = gp_camera_get_storageinfo (camera, &storage_info, &num_storage_info, context);
  if (rc != 0) {
    /* Not all gphoto drivers implement get storage info (drivers for proprietary
       protocols often don't) */
    if (rc == GP_ERROR_NOT_SUPPORTED)
      l = g_list_prepend (l, g_strdup ("/"));
    goto out;
  }

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
