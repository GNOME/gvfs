#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "gdaemonvolumemonitor.h"
#include "gdaemonvolume.h"

struct _GDaemonVolumeMonitor {
  GVolumeMonitor parent;

  gpointer mount_monitor;

  GList *last_mountpoints;
  GList *last_mounts;

  GList *drives;
  GList *volumes;
};

static void update_drives (GDaemonVolumeMonitor *monitor);
static void update_volumes (GDaemonVolumeMonitor *monitor);

G_DEFINE_TYPE (GDaemonVolumeMonitor, g_daemon_volume_monitor, G_TYPE_VOLUME_MONITOR);

static void
g_daemon_volume_monitor_finalize (GObject *object)
{
  GDaemonVolumeMonitor *monitor;
  
  monitor = G_DAEMON_VOLUME_MONITOR (object);

  if (monitor->mount_monitor)
    _g_stop_monitoring_daemon_mounts (monitor->mount_monitor);

  g_list_foreach (monitor->last_mounts, (GFunc)_g_daemon_mount_free, NULL);
  g_list_free (monitor->last_mounts);

  g_list_foreach (monitor->volumes, (GFunc)g_object_unref, NULL);
  g_list_free (monitor->volumes);
  g_list_foreach (monitor->drives, (GFunc)g_object_unref, NULL);
  g_list_free (monitor->drives);
  
  if (G_OBJECT_CLASS (g_daemon_volume_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_volume_monitor_parent_class)->finalize) (object);
}

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
  GDaemonVolumeMonitor *monitor;
  GList *l;
  
  monitor = G_DAEMON_VOLUME_MONITOR (volume_monitor);

  l = g_list_copy (monitor->drives);
  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  return l;
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

static void
mountpoints_changed (gpointer user_data)
{
  GDaemonVolumeMonitor *daemon_monitor = user_data;

  /* Update both to make sure drives are created before volumes */
  update_drives (daemon_monitor);
  update_volumes (daemon_monitor);
}

static void
mounts_changed (gpointer user_data)
{
  GDaemonVolumeMonitor *daemon_monitor = user_data;

  /* Update both to make sure drives are created before volumes */
  update_drives (daemon_monitor);
  update_volumes (daemon_monitor);
}

static void
g_daemon_volume_monitor_init (GDaemonVolumeMonitor *daemon_monitor)
{

  daemon_monitor->mount_monitor = _g_monitor_daemon_mounts (mountpoints_changed,
							mounts_changed,
							daemon_monitor);
  update_drives (daemon_monitor);
  update_volumes (daemon_monitor);

}

GVolumeMonitor *
g_daemon_volume_monitor_new (void)
{
  GDaemonVolumeMonitor *monitor;

  monitor = g_object_new (G_TYPE_DAEMON_VOLUME_MONITOR, NULL);
  
  return G_VOLUME_MONITOR (monitor);
}

static void
diff_sorted_lists (GList *list1, GList *list2, GCompareFunc compare,
		   GList **added, GList **removed)
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

GDaemonDrive *
g_daemon_volume_monitor_lookup_drive_for_mountpoint (GDaemonVolumeMonitor *monitor,
						   const char *mountpoint)
{
  GList *l;

  for (l = monitor->drives; l != NULL; l = l->next)
    {
      GDaemonDrive *drive = l->data;

      if (g_daemon_drive_has_mountpoint (drive, mountpoint))
	return drive;
    }
  
  return NULL;
}

static GDaemonVolume *
find_volume_by_mountpoint (GDaemonVolumeMonitor *monitor,
			   const char *mountpoint)
{
  GList *l;

  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      GDaemonVolume *volume = l->data;

      if (g_daemon_volume_has_mountpoint (volume, mountpoint))
	return volume;
    }
  
  return NULL;
}

static void
update_drives (GDaemonVolumeMonitor *monitor)
{
  GList *new_mountpoints;
  GList *removed, *added;
  GList *l;
  GDaemonDrive *drive;
  
  if (_g_get_daemon_mount_points (&new_mountpoints))
    {
      new_mountpoints = g_list_sort (new_mountpoints, (GCompareFunc) _g_daemon_mount_point_compare);
      
      diff_sorted_lists (monitor->last_mountpoints,
			 new_mountpoints, (GCompareFunc) _g_daemon_mount_point_compare,
			 &added, &removed);
    
      for (l = removed; l != NULL; l = l->next)
	{
	  GDaemonMountPoint *mountpoint = l->data;
	  
	  drive = g_daemon_volume_monitor_lookup_drive_for_mountpoint (monitor, mountpoint->mount_path);
	  if (drive)
	    {
	      g_daemon_drive_disconnected (drive);
	      monitor->drives = g_list_remove (monitor->drives, drive);
	      g_signal_emit_by_name (monitor, "drive_disconnected", drive);
	      g_object_unref (drive);
	    }
	}
      
      for (l = added; l != NULL; l = l->next)
	{
	  GDaemonMountPoint *mountpoint = l->data;
	  
	  drive = g_daemon_drive_new (G_VOLUME_MONITOR (monitor), mountpoint);
	  if (drive)
	    {
	      monitor->drives = g_list_prepend (monitor->drives, drive);
	      g_signal_emit_by_name (monitor, "drive_connected", drive);
	    }
	}
      
      g_list_free (added);
      g_list_free (removed);
      g_list_foreach (monitor->last_mountpoints,
		      (GFunc)_g_daemon_mount_point_free, NULL);
      g_list_free (monitor->last_mountpoints);
      monitor->last_mountpoints = new_mountpoints;
    }
}

static void
update_volumes (GDaemonVolumeMonitor *monitor)
{
  GList *new_mounts;
  GList *removed, *added;
  GList *l;
  GDaemonVolume *volume;
  
  if (_g_get_daemon_mounts (&new_mounts))
    {
      new_mounts = g_list_sort (new_mounts, (GCompareFunc) _g_daemon_mount_compare);
      
      diff_sorted_lists (monitor->last_mounts,
			 new_mounts, (GCompareFunc) _g_daemon_mount_compare,
			 &added, &removed);
    
      for (l = removed; l != NULL; l = l->next)
	{
	  GDaemonMount *mount = l->data;
	  
	  volume = find_volume_by_mountpoint (monitor, mount->mount_path);
	  if (volume)
	    {
	      g_daemon_volume_unmounted (volume);
	      monitor->volumes = g_list_remove (monitor->volumes, volume);
	      g_signal_emit_by_name (monitor, "volume_unmounted", volume);
	      g_object_unref (volume);
	    }
	}
      
      for (l = added; l != NULL; l = l->next)
	{
	  GDaemonMount *mount = l->data;
	  
	  volume = g_daemon_volume_new (G_VOLUME_MONITOR (monitor), mount);
	  if (volume)
	    {
	      monitor->volumes = g_list_prepend (monitor->volumes, volume);
	      g_signal_emit_by_name (monitor, "volume_mounted", volume);
	    }
	}
      
      g_list_free (added);
      g_list_free (removed);
      g_list_foreach (monitor->last_mounts,
		      (GFunc)_g_daemon_mount_free, NULL);
      g_list_free (monitor->last_mounts);
      monitor->last_mounts = new_mounts;
    }
}
