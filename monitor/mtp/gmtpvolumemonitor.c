/* GIO - GLib Input, Output and Streaming Library
 *   Volume Monitor for MTP Backend
 *
 * Copyright (C) 2012 Philip Langdale <philipl@overt.org>
 * - Based on ggphoto2volume.c
 *   - Copyright (C) 2006-2007 Red Hat, Inc.
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
 * Public License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <limits.h>
#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "gmtpvolumemonitor.h"
#include "gmtpvolume.h"

#include <gio/gunixmounts.h>

G_LOCK_DEFINE_STATIC(vm_lock);

static GMtpVolumeMonitor *the_volume_monitor = NULL;

struct _GMtpVolumeMonitor {
  GNativeVolumeMonitor parent;

  GUnixMountMonitor *mount_monitor;

  GUdevClient *gudev_client;

  GList *last_devices;

  GList *device_volumes;
};

static void on_uevent (GUdevClient *client, 
                       gchar *action,
                       GUdevDevice *device,
                       gpointer user_data);

G_DEFINE_TYPE (GMtpVolumeMonitor, g_mtp_volume_monitor, G_TYPE_VOLUME_MONITOR)

static void
list_free (GList *objects)
{
  g_list_free_full (objects, g_object_unref);
}

static void
g_mtp_volume_monitor_dispose (GObject *object)
{
  G_LOCK (vm_lock);
  the_volume_monitor = NULL;
  G_UNLOCK (vm_lock);

  (*G_OBJECT_CLASS (g_mtp_volume_monitor_parent_class)->dispose) (object);
}

static void
g_mtp_volume_monitor_finalize (GObject *object)
{
  GMtpVolumeMonitor *monitor;

  monitor = G_MTP_VOLUME_MONITOR (object);

  g_signal_handlers_disconnect_by_func (monitor->gudev_client, on_uevent, monitor);

  g_object_unref (monitor->gudev_client);

  list_free (monitor->last_devices);
  list_free (monitor->device_volumes);

  (*G_OBJECT_CLASS (g_mtp_volume_monitor_parent_class)->finalize) (object);
}

static GList *
get_mounts (GVolumeMonitor *volume_monitor)
{
  return NULL;
}

static GList *
get_volumes (GVolumeMonitor *volume_monitor)
{
  GMtpVolumeMonitor *monitor;
  GList *l;

  monitor = G_MTP_VOLUME_MONITOR (volume_monitor);

  G_LOCK (vm_lock);

  l = g_list_copy (monitor->device_volumes);
  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  G_UNLOCK (vm_lock);

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
gudev_add_device (GMtpVolumeMonitor *monitor, GUdevDevice *device, gboolean do_emit)
{
  GList *l;
  GMtpVolume *volume;
  const char *usb_serial_id, *device_path;
  char *uri, *usb_serial_id_escaped;
  GFile *activation_mount_root;
  gboolean serial_conflict = FALSE;

  device_path = g_udev_device_get_device_file (device);
  if (!device_path) {
    g_debug ("Ignoring device '%s' without a device file",
             g_udev_device_get_sysfs_path (device));
    return;
  }

  /*
   * We do not use ID_SERIAL_SHORT (the actualy device serial value) as
   * this field is not populated when an ID_SERIAL has to be synthesized.
   */
  usb_serial_id = g_udev_device_get_property (device, "ID_SERIAL");
  if (usb_serial_id == NULL) {
    g_warning ("device %s has no ID_SERIAL property, ignoring", device_path);
    return;
  }

  usb_serial_id_escaped = g_uri_escape_string (usb_serial_id, NULL, FALSE);
  uri = g_strdup_printf ("mtp://%s", usb_serial_id_escaped);
  activation_mount_root = g_file_new_for_uri (uri);
  g_free (uri);
  g_free (usb_serial_id_escaped);

  /*
   * We do not support plugging in multiple devices that lack proper serial
   * numbers. Linux will attempt to synthesize an ID based on the device
   * product information, which will avoid collisions between different
   * types of device, but two identical, serial-less, devices will still
   * conflict.
   */
  for (l = monitor->device_volumes; l != NULL; l = l->next) {
    GMtpVolume *volume = G_MTP_VOLUME (l->data);

    GFile *existing_root = g_volume_get_activation_root (G_VOLUME (volume));
    if (g_file_equal (activation_mount_root, existing_root)) {
      serial_conflict = TRUE;
    }
    g_object_unref (existing_root);

    if (serial_conflict) {
      g_warning ("device %s has an identical ID_SERIAL value to an "
                 "existing device. Multiple devices are not supported.",
                 g_udev_device_get_device_file (device));
      g_object_unref (activation_mount_root);
      return;
    }
  }

  volume = g_mtp_volume_new (G_VOLUME_MONITOR (monitor),
                             device,
                             monitor->gudev_client,
                             activation_mount_root);
  if (volume != NULL) {
    monitor->device_volumes = g_list_prepend (monitor->device_volumes, volume);
    if (do_emit)
      g_signal_emit_by_name (monitor, "volume_added", volume);
  }

  if (activation_mount_root != NULL)
    g_object_unref (activation_mount_root);
}

static void
gudev_remove_device (GMtpVolumeMonitor *monitor, GUdevDevice *device)
{
  GList *l, *ll;
  const gchar* sysfs_path;

  sysfs_path = g_udev_device_get_sysfs_path (device);

  g_debug ("gudev_remove_device: %s", g_udev_device_get_device_file (device));

  for (l = monitor->device_volumes; l != NULL; l = ll) {
    GMtpVolume *volume = G_MTP_VOLUME (l->data);

    ll = l->next;

    if (g_mtp_volume_has_path (volume, sysfs_path)) {
      g_debug ("gudev_remove_device: found volume %s, deleting", sysfs_path);
      g_signal_emit_by_name (monitor, "volume_removed", volume);
      g_signal_emit_by_name (volume, "removed");
      g_mtp_volume_removed (volume);
      monitor->device_volumes = g_list_remove (monitor->device_volumes, volume);
      g_object_unref (volume);
    }
  }
}

static void
on_uevent (GUdevClient *client, gchar *action, GUdevDevice *device, gpointer user_data)
{
  GMtpVolumeMonitor *monitor = G_MTP_VOLUME_MONITOR (user_data);

  g_debug ("on_uevent: action=%s, device=%s", action, g_udev_device_get_device_file(device));

  if (g_strcmp0 (action, "add") == 0 && g_udev_device_has_property (device, "ID_MTP_DEVICE"))
    gudev_add_device (monitor, device, TRUE);
  else if (g_strcmp0 (action, "remove") == 0)
    gudev_remove_device (monitor, device);
  else
    g_debug ("on_uevent: discarding");
}

static void
gudev_coldplug_devices (GMtpVolumeMonitor *monitor)
{
  GList *usb_devices, *l;

  usb_devices = g_udev_client_query_by_subsystem (monitor->gudev_client, "usb");
  for (l = usb_devices; l != NULL; l = l->next) {
    GUdevDevice *d = l->data;
    if (g_udev_device_has_property (d, "ID_MTP_DEVICE"))
        gudev_add_device (monitor, d, FALSE);
  }
  g_list_free_full(usb_devices, g_object_unref);
}

static GObject *
g_mtp_volume_monitor_constructor (GType                  type,
                                  guint                  n_construct_properties,
                                  GObjectConstructParam *construct_properties)
{
  GObject *object;
  GMtpVolumeMonitor *monitor;
  GMtpVolumeMonitorClass *klass;
  GObjectClass *parent_class;

  G_LOCK (vm_lock);
  if (the_volume_monitor != NULL) {
    object = G_OBJECT (g_object_ref (the_volume_monitor));
    G_UNLOCK (vm_lock);
    return object;
  }
  G_UNLOCK (vm_lock);

  /*g_warning ("creating vm singleton");*/

  object = NULL;

  /* Invoke parent constructor. */
  klass = G_MTP_VOLUME_MONITOR_CLASS (g_type_class_peek (G_TYPE_MTP_VOLUME_MONITOR));
  parent_class = G_OBJECT_CLASS (g_type_class_peek_parent (klass));
  object = parent_class->constructor (type,
                                      n_construct_properties,
                                      construct_properties);

  monitor = G_MTP_VOLUME_MONITOR (object);

  const char *subsystems[] = { "usb", NULL };
  monitor->gudev_client = g_udev_client_new (subsystems);

  g_signal_connect (monitor->gudev_client, 
                    "uevent", G_CALLBACK (on_uevent), 
                    monitor);

  gudev_coldplug_devices (monitor);

  G_LOCK (vm_lock);
  the_volume_monitor = monitor;
  G_UNLOCK (vm_lock);

  return object;
}

static void
g_mtp_volume_monitor_init (GMtpVolumeMonitor *monitor)
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
g_mtp_volume_monitor_class_init (GMtpVolumeMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVolumeMonitorClass *monitor_class = G_VOLUME_MONITOR_CLASS (klass);

  gobject_class->constructor = g_mtp_volume_monitor_constructor;
  gobject_class->finalize = g_mtp_volume_monitor_finalize;
  gobject_class->dispose = g_mtp_volume_monitor_dispose;

  monitor_class->get_mounts = get_mounts;
  monitor_class->get_volumes = get_volumes;
  monitor_class->get_connected_drives = get_connected_drives;
  monitor_class->get_volume_for_uuid = get_volume_for_uuid;
  monitor_class->get_mount_for_uuid = get_mount_for_uuid;
  monitor_class->is_supported = is_supported;
}

/**
 * g_mtp_volume_monitor_new:
 *
 * Returns:  a new #GVolumeMonitor.
 **/
GVolumeMonitor *
g_mtp_volume_monitor_new (void)
{
  GMtpVolumeMonitor *monitor;

  monitor = g_object_new (G_TYPE_MTP_VOLUME_MONITOR, NULL);

  return G_VOLUME_MONITOR (monitor);
}
