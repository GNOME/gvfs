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
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "gdaemonvolumemonitor.h"
#include "gdaemonmount.h"
#include "gmounttracker.h"

struct _GDaemonVolumeMonitor {
  GVolumeMonitor parent;

  GMountTracker *mount_tracker;
  GList *mounts;
};

G_DEFINE_DYNAMIC_TYPE (GDaemonVolumeMonitor, g_daemon_volume_monitor, G_TYPE_VOLUME_MONITOR);

static GList *
get_mounts (GVolumeMonitor *volume_monitor)
{
  GDaemonVolumeMonitor *monitor;
  GList *l;

  monitor = G_DAEMON_VOLUME_MONITOR (volume_monitor);

  l = g_list_copy (monitor->mounts);
  g_list_foreach (l, (GFunc)g_object_ref, NULL);

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

static void
mount_added (GDaemonVolumeMonitor *daemon_monitor, GMountInfo *mount_info)
{
  GDaemonMount *mount;
  GVolume *volume;

  mount = find_mount_by_mount_info (daemon_monitor, mount_info);
  if (mount)
    {
      g_warning (G_STRLOC ": Mount was added twice!");
      return;
    }

  if (mount_info->user_visible)
    {
      mount = g_daemon_mount_new (mount_info, G_VOLUME_MONITOR (daemon_monitor));
      volume = g_volume_monitor_adopt_orphan_mount (G_MOUNT (mount));
      if (volume != NULL)
        g_daemon_mount_set_foreign_volume (mount, volume);
      daemon_monitor->mounts = g_list_prepend (daemon_monitor->mounts, mount);
      g_signal_emit_by_name (daemon_monitor, "mount_added", mount);
    }
}

static void
mount_removed (GDaemonVolumeMonitor *daemon_monitor, GMountInfo *mount_info)
{
  GDaemonMount *mount;

  mount = find_mount_by_mount_info (daemon_monitor, mount_info);
  if (!mount)
    {
      g_warning (G_STRLOC ": An unknown mount was removed!");
      return;
    }

  daemon_monitor->mounts = g_list_remove (daemon_monitor->mounts, mount);
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
  GVolume *volume;
  
  daemon_monitor->mount_tracker = g_mount_tracker_new (_g_daemon_vfs_get_async_bus ());

  g_signal_connect_swapped (daemon_monitor->mount_tracker, "mounted",
			    (GCallback) mount_added, daemon_monitor);
  g_signal_connect_swapped (daemon_monitor->mount_tracker, "unmounted",
			    (GCallback) mount_removed, daemon_monitor);

  /* Initialize with current list */
  mounts = g_mount_tracker_list_mounts (daemon_monitor->mount_tracker);

  for (l = mounts; l != NULL; l = l->next) {
    info = l->data;
    if (info->user_visible)
      {
        mount = g_daemon_mount_new (info, G_VOLUME_MONITOR (daemon_monitor));
        volume = g_volume_monitor_adopt_orphan_mount (G_MOUNT (mount));
        if (volume != NULL)
          g_daemon_mount_set_foreign_volume (mount, volume);
	daemon_monitor->mounts = g_list_prepend (daemon_monitor->mounts, mount);
      }
    
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

  g_list_foreach (monitor->mounts, (GFunc)g_object_unref, NULL);
  g_list_free (monitor->mounts);
  
  if (G_OBJECT_CLASS (g_daemon_volume_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_volume_monitor_parent_class)->finalize) (object);
}

static void
g_daemon_volume_monitor_class_finalize (GDaemonVolumeMonitorClass *klass)
{
}

static void
g_daemon_volume_monitor_class_init (GDaemonVolumeMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVolumeMonitorClass *monitor_class = G_VOLUME_MONITOR_CLASS (klass);
  
  gobject_class->finalize = g_daemon_volume_monitor_finalize;

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
}
