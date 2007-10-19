#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "gdaemonvolumemonitor.h"
#include "gdaemonvolume.h"
#include "gmounttracker.h"

struct _GDaemonVolumeMonitor {
  GVolumeMonitor parent;

  GMountTracker *mount_tracker;
  GList *volumes;
};

G_DEFINE_DYNAMIC_TYPE (GDaemonVolumeMonitor, g_daemon_volume_monitor, G_TYPE_VOLUME_MONITOR);

static GList *
get_mounted_volumes (GVolumeMonitor *volume_monitor)
{
  GDaemonVolumeMonitor *monitor;
  GList *l;

  monitor = G_DAEMON_VOLUME_MONITOR (volume_monitor);

  l = g_list_copy (monitor->volumes);
  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  return l;
}

static GList *
get_connected_drives (GVolumeMonitor *volume_monitor)
{
  /* TODO: Can daemon mounts have drives? */
  return NULL;
}

static GDaemonVolume *
find_volume_by_mount_info (GDaemonVolumeMonitor *daemon_monitor, GMountInfo *mount_info)
{
  GDaemonVolume *found_volume = NULL;
  GList         *l;

  for (l = daemon_monitor->volumes; l; l = g_list_next (l))
    {
      GDaemonVolume *existing_volume = l->data;
      GMountInfo    *existing_mount_info;

      existing_mount_info = g_daemon_volume_get_mount_info (existing_volume);
      if (g_mount_info_equal (mount_info, existing_mount_info))
	{
	  found_volume = existing_volume;
	  break;
	}
    }

  return found_volume;
}

static void
mount_added (GDaemonVolumeMonitor *daemon_monitor, GMountInfo *mount_info)
{
  GDaemonVolume *volume;

  volume = find_volume_by_mount_info (daemon_monitor, mount_info);
  if (volume)
    {
      g_warning (G_STRLOC ": Mount was added twice!");
      return;
    }

  volume = g_daemon_volume_new (G_VOLUME_MONITOR (daemon_monitor), g_mount_info_dup (mount_info));
  daemon_monitor->volumes = g_list_prepend (daemon_monitor->volumes, volume);
  g_signal_emit_by_name (daemon_monitor, "volume_mounted", volume);
}

static void
mount_removed (GDaemonVolumeMonitor *daemon_monitor, GMountInfo *mount_info)
{
  GDaemonVolume *volume;

  volume = find_volume_by_mount_info (daemon_monitor, mount_info);
  if (!volume)
    {
      g_warning (G_STRLOC ": An unknown mount was removed!");
      return;
    }

  daemon_monitor->volumes = g_list_remove (daemon_monitor->volumes, volume);
  g_signal_emit_by_name (daemon_monitor, "volume_unmounted", volume);
  g_object_unref (volume);
}

static void
g_daemon_volume_monitor_init (GDaemonVolumeMonitor *daemon_monitor)
{
  GList *mounts, *l;
  GDaemonVolume *volume;
  
  daemon_monitor->mount_tracker = g_mount_tracker_new (_g_daemon_vfs_get_async_bus ());

  g_signal_connect_swapped (daemon_monitor->mount_tracker, "mounted",
			    (GCallback) mount_added, daemon_monitor);
  g_signal_connect_swapped (daemon_monitor->mount_tracker, "unmounted",
			    (GCallback) mount_removed, daemon_monitor);

  /* Initialize with current list */
  mounts = g_mount_tracker_list_mounts (daemon_monitor->mount_tracker);

  for (l = mounts; l != NULL; l = l->next) {
    volume = g_daemon_volume_new (G_VOLUME_MONITOR (daemon_monitor), l->data);
    daemon_monitor->volumes = g_list_prepend (daemon_monitor->volumes, volume);
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

  g_list_foreach (monitor->volumes, (GFunc)g_object_unref, NULL);
  g_list_free (monitor->volumes);
  
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

  monitor_class->get_mounted_volumes = get_mounted_volumes;
  monitor_class->get_connected_drives = get_connected_drives;
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
