
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
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "gdaemonvolumemonitor.h"
#include "gdaemonmount.h"
#include "gdaemonvfs.h"
#include "gmounttracker.h"

G_LOCK_DEFINE_STATIC(daemon_vm);

static GDaemonVolumeMonitor *_the_daemon_volume_monitor;

struct _GDaemonVolumeMonitor {
  GVolumeMonitor parent;

  GMountTracker *mount_tracker;
  GList *mounts;
};

G_DEFINE_DYNAMIC_TYPE (GDaemonVolumeMonitor, g_daemon_volume_monitor, G_TYPE_VOLUME_MONITOR)

static GList *
get_mounts (GVolumeMonitor *volume_monitor)
{
  GDaemonVolumeMonitor *monitor;
  GList *l;

  G_LOCK (daemon_vm);

  monitor = G_DAEMON_VOLUME_MONITOR (volume_monitor);

  l = g_list_copy (monitor->mounts);
  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  G_UNLOCK (daemon_vm);

  return l;
}

static GList *
get_volumes (GVolumeMonitor *volume_monitor)
{
  /* TODO: Can daemon mounts have volumes? */
  return NULL;
}

static GList *
get_connected_drives (GVolumeMonitor *volume_monitor)
{
  /* TODO: Can daemon mounts have drives? */
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

static GDaemonMount *
find_mount_by_mount_info (GDaemonVolumeMonitor *daemon_monitor, GMountInfo *mount_info)
{
  GDaemonMount *found_mount = NULL;
  GList         *l;

  for (l = daemon_monitor->mounts; l; l = g_list_next (l))
    {
      GDaemonMount *existing_mount = l->data;
      GMountInfo   *existing_mount_info;

      existing_mount_info = g_daemon_mount_get_mount_info (existing_mount);
      if (g_mount_info_equal (mount_info, existing_mount_info))
	{
	  found_mount = existing_mount;
	  break;
	}
    }

  return found_mount;
}

GDaemonMount *
g_daemon_volume_monitor_find_mount_by_mount_info (GMountInfo *mount_info)
{
  GDaemonMount *daemon_mount;

  G_LOCK (daemon_vm);

  daemon_mount = NULL;
  if (_the_daemon_volume_monitor != NULL)
    {
      daemon_mount = find_mount_by_mount_info (_the_daemon_volume_monitor, mount_info);
      
      if (daemon_mount != NULL)
        g_object_ref (daemon_mount);
    }

  G_UNLOCK (daemon_vm);

  return daemon_mount;
}

static void
mount_added (GDaemonVolumeMonitor *daemon_monitor, GMountInfo *mount_info)
{
  GDaemonMount *mount;

  G_LOCK (daemon_vm);

  mount = find_mount_by_mount_info (daemon_monitor, mount_info);
  if (mount)
    {
      g_warning (G_STRLOC ": Mount was added twice!");
      
      G_UNLOCK (daemon_vm);
      return;
    }

  mount = g_daemon_mount_new (mount_info, G_VOLUME_MONITOR (daemon_monitor));
  daemon_monitor->mounts = g_list_prepend (daemon_monitor->mounts, mount);

  /* Ref for the signal emission, other ref is owned by volume monitor */
  g_object_ref (mount);
  
  G_UNLOCK (daemon_vm);

  if (mount)
    {
      /* Emit signal outside lock */
      g_signal_emit_by_name (daemon_monitor, "mount_added", mount);
      g_object_unref (mount);
    }
}

static void
mount_removed (GDaemonVolumeMonitor *daemon_monitor, GMountInfo *mount_info)
{
  GDaemonMount *mount;

  G_LOCK (daemon_vm);

  mount = find_mount_by_mount_info (daemon_monitor, mount_info);
  if (!mount)
    {
      g_warning (G_STRLOC ": An unknown mount was removed!");
      
      G_UNLOCK (daemon_vm);
      return;
    }

  daemon_monitor->mounts = g_list_remove (daemon_monitor->mounts, mount);
  
  G_UNLOCK (daemon_vm);

  g_signal_emit_by_name (daemon_monitor, "mount_removed", mount);
  g_signal_emit_by_name (mount, "unmounted");
  
  g_object_unref (mount);
}

static void
g_daemon_volume_monitor_init (GDaemonVolumeMonitor *daemon_monitor)
{
  GList *mounts, *l;
  GDaemonMount *mount;
  GMountInfo *info;

  _the_daemon_volume_monitor = daemon_monitor;

  daemon_monitor->mount_tracker = g_mount_tracker_new (_g_daemon_vfs_get_async_bus (), TRUE);

  g_signal_connect_swapped (daemon_monitor->mount_tracker, "mounted",
			    (GCallback) mount_added, daemon_monitor);
  g_signal_connect_swapped (daemon_monitor->mount_tracker, "unmounted",
			    (GCallback) mount_removed, daemon_monitor);

  /* Initialize with current list */
  mounts = g_mount_tracker_list_mounts (daemon_monitor->mount_tracker);

  for (l = mounts; l != NULL; l = l->next) {
    info = l->data;

    mount = g_daemon_mount_new (info, G_VOLUME_MONITOR (daemon_monitor));
    daemon_monitor->mounts = g_list_prepend (daemon_monitor->mounts, mount);

    g_mount_info_unref (info);
  }
  
  g_list_free (mounts);
}

static void
g_daemon_volume_monitor_finalize (GObject *object)
{
  GDaemonVolumeMonitor *monitor;
  
  monitor = G_DAEMON_VOLUME_MONITOR (object);

  g_signal_handlers_disconnect_by_func (monitor->mount_tracker, mount_added, monitor);
  g_signal_handlers_disconnect_by_func (monitor->mount_tracker, mount_removed, monitor);
  
  g_object_unref (monitor->mount_tracker);
  
  g_list_free_full (monitor->mounts, g_object_unref);
  
  if (G_OBJECT_CLASS (g_daemon_volume_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_volume_monitor_parent_class)->finalize) (object);
}

static void
g_daemon_volume_monitor_dispose (GObject *object)
{
  G_LOCK (daemon_vm);
  _the_daemon_volume_monitor = NULL;
  G_UNLOCK (daemon_vm);
  
  if (G_OBJECT_CLASS (g_daemon_volume_monitor_parent_class)->dispose)
    (*G_OBJECT_CLASS (g_daemon_volume_monitor_parent_class)->dispose) (object);
}

static void
g_daemon_volume_monitor_class_finalize (GDaemonVolumeMonitorClass *klass)
{
}

static gboolean
is_supported (void)
{
  GVfs *vfs;
  gboolean res;

  res = FALSE;

  /* Don't do anything if the default vfs is not DAEMON_VFS */
  vfs = g_vfs_get_default ();
  
  if (vfs != NULL && G_IS_DAEMON_VFS (vfs))
    res = TRUE;

  return res;
}

static void
g_daemon_volume_monitor_class_init (GDaemonVolumeMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVolumeMonitorClass *monitor_class = G_VOLUME_MONITOR_CLASS (klass);
  
  gobject_class->finalize = g_daemon_volume_monitor_finalize;
  gobject_class->dispose = g_daemon_volume_monitor_dispose;

  monitor_class->is_supported = is_supported;
  monitor_class->get_mounts = get_mounts;
  monitor_class->get_volumes = get_volumes;
  monitor_class->get_connected_drives = get_connected_drives;
  monitor_class->get_volume_for_uuid = get_volume_for_uuid;
  monitor_class->get_mount_for_uuid = get_mount_for_uuid;
}

GVolumeMonitor *
g_daemon_volume_monitor_new (void)
{
  GDaemonVolumeMonitor *monitor;

  monitor = g_object_new (G_TYPE_DAEMON_VOLUME_MONITOR, NULL);
  
  return G_VOLUME_MONITOR (monitor);
}

void
g_daemon_volume_monitor_register_types (GTypeModule *module)
{
  g_daemon_volume_monitor_register_type (G_TYPE_MODULE (module));
  g_io_extension_point_implement (G_VOLUME_MONITOR_EXTENSION_POINT_NAME,
				  G_TYPE_DAEMON_VOLUME_MONITOR,
				  "gvfs",
				  0);
}
