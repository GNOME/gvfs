#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "gunionvolumemonitor.h"
#include "gunionvolume.h"
#include "gvolumepriv.h"
#ifdef G_OS_UNIX
#include "gunixvolumemonitor.h"
#endif

struct _GUnionVolumeMonitor {
  GVolumeMonitor parent;

  GList *monitors;
  GList *volumes;
  GList *drives;
};

static void g_union_volume_monitor_remove_monitor (GUnionVolumeMonitor *union_monitor,
						   GVolumeMonitor *child_monitor);


G_DEFINE_TYPE (GUnionVolumeMonitor, g_union_volume_monitor, G_TYPE_VOLUME_MONITOR);

G_LOCK_DEFINE_STATIC(the_volume_monitor);
static GUnionVolumeMonitor *the_volume_monitor = NULL;

static void
g_union_volume_monitor_finalize (GObject *object)
{
  GUnionVolumeMonitor *monitor;
  
  monitor = G_UNION_VOLUME_MONITOR (object);

  while (monitor->monitors != NULL)
    g_union_volume_monitor_remove_monitor (monitor,
					   monitor->monitors->data);
  
  if (G_OBJECT_CLASS (g_union_volume_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_union_volume_monitor_parent_class)->finalize) (object);
}

static void
g_union_volume_monitor_dispose (GObject *object)
{
  GUnionVolumeMonitor *monitor;
  
  monitor = G_UNION_VOLUME_MONITOR (object);

  G_LOCK (the_volume_monitor);
  the_volume_monitor = NULL;
  G_UNLOCK (the_volume_monitor);
  
  if (G_OBJECT_CLASS (g_union_volume_monitor_parent_class)->dispose)
    (*G_OBJECT_CLASS (g_union_volume_monitor_parent_class)->dispose) (object);
}

static GList *
get_mounted_volumes (GVolumeMonitor *volume_monitor)
{
  GUnionVolumeMonitor *monitor;
  GList *l;
  
  monitor = G_UNION_VOLUME_MONITOR (volume_monitor);

  l = g_list_copy (monitor->volumes);
  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  return l;
}

static GList *
get_connected_drives (GVolumeMonitor *volume_monitor)
{
  GUnionVolumeMonitor *monitor;
  GList *l;
  
  monitor = G_UNION_VOLUME_MONITOR (volume_monitor);

  l = g_list_copy (monitor->drives);
  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  return l;
}

static void
g_union_volume_monitor_class_init (GUnionVolumeMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVolumeMonitorClass *monitor_class = G_VOLUME_MONITOR_CLASS (klass);
  
  gobject_class->finalize = g_union_volume_monitor_finalize;
  gobject_class->dispose = g_union_volume_monitor_dispose;

  monitor_class->get_mounted_volumes = get_mounted_volumes;
  monitor_class->get_connected_drives = get_connected_drives;
}

static void
add_child_volume (GUnionVolumeMonitor *union_monitor,
		  GVolume *child_volume,
		  GVolumeMonitor *child_monitor)
{
  char *platform_id, *id;
  GList *l;
  GUnionVolume *union_volume;

  /* TODO: Add locking everywhere... */
  platform_id = g_volume_get_platform_id (child_volume);

  if (platform_id)
    {
      for (l = union_monitor->volumes; l != NULL; l = l->next)
	{
	  GVolume *current_child = l->data;
	  id = g_volume_get_platform_id (current_child);

	  if (id && strcmp (id, platform_id) == 0)
	    {
	      g_union_volume_add_volume (G_UNION_VOLUME (current_child), child_volume, child_monitor);
	      g_free (id);
	      g_free (platform_id);
	      return;
	    }
	  g_free (id);
	}
      g_free (platform_id);
    }

  union_volume = g_union_volume_new (child_volume, child_monitor);
  union_monitor->volumes = g_list_prepend (union_monitor->volumes,
					   union_volume);
  g_signal_emit_by_name (union_monitor,
			 "volume_mounted",
			 union_volume);
}

static GUnionVolume *
lookup_union_volume (GUnionVolumeMonitor *union_monitor,
		     GVolume *child_volume)
{
  GList *l;
  GUnionVolume *union_volume;
  
  for (l = union_monitor->volumes; l != NULL; l = l->next)
    {
      union_volume = l->data;
      
      if (g_union_volume_has_child_volume (union_volume, child_volume))
	return union_volume;
    }
  return NULL;
}

static void
remove_child_volume (GUnionVolumeMonitor *union_monitor,
		     GVolume *child_volume)
{
  GUnionVolume *union_volume;

  union_volume = lookup_union_volume (union_monitor, child_volume);
  if (g_union_volume_remove_volume (union_volume, child_volume))
    {
      union_monitor->volumes = g_list_remove (union_monitor->volumes,
					      union_volume);
      g_signal_emit_by_name (union_monitor,
			     "volume_unmounted",
			     union_volume);
      g_object_unref (union_volume);
    }
}

static void
add_child_drive (GUnionVolumeMonitor *union_monitor,
		 GDrive *child_drive)
{
  /* TODO */
  if (0)
    g_signal_emit_by_name (union_monitor,
			   "drive_conntected",
			   child_drive);
}

static void
remove_child_drive (GUnionVolumeMonitor *union_monitor,
		    GDrive *child_drive)
{
  /* TODO */
  if (0)
    g_signal_emit_by_name (union_monitor,
			   "drive_disconntected",
			   child_drive);
}

static void
child_volume_mounted (GVolumeMonitor *child_monitor,
		      GVolume *child_volume,
		      GUnionVolumeMonitor *union_monitor)
{
  add_child_volume (union_monitor,
		    child_volume,
		    child_monitor);
}

static void
child_volume_pre_unmount (GVolumeMonitor *child_monitor,
			  GVolume *child_volume,
			  GUnionVolumeMonitor *union_monitor)
{
  GUnionVolume *union_volume;

  union_volume = lookup_union_volume (union_monitor, child_volume);
  if (union_volume)
    g_signal_emit_by_name (union_monitor,
			   "volume_pre_unmount",
			   union_volume);
}

static void
child_volume_unmounted (GVolumeMonitor *child_monitor,
			GVolume *volume,
			GUnionVolumeMonitor *union_monitor)
{
  remove_child_volume (union_monitor, volume);
}

static void
child_drive_connected (GVolumeMonitor *child_monitor,
		       GDrive *drive,
		       GUnionVolumeMonitor *union_monitor)
{
  add_child_drive (union_monitor, drive);
}

static void
child_drive_disconnected (GVolumeMonitor *child_monitor,
			  GDrive *drive,
			  GUnionVolumeMonitor *union_monitor)
{
  remove_child_drive (union_monitor, drive);
}

static void
g_union_volume_monitor_add_monitor (GUnionVolumeMonitor *union_monitor,
				    GVolumeMonitor *volume_monitor)
{
  GList *volumes, *drives, *l;
  GVolume *volume;
  GDrive *drive;
  
  if (g_list_find (union_monitor->monitors, volume_monitor))
    return;

  union_monitor->monitors = g_list_prepend (union_monitor->monitors,
					    g_object_ref (volume_monitor));

  g_signal_connect (volume_monitor, "volume_mounted", (GCallback)child_volume_mounted, union_monitor);
  g_signal_connect (volume_monitor, "volume_pre_unmount", (GCallback)child_volume_pre_unmount, union_monitor);
  g_signal_connect (volume_monitor, "volume_unmounted", (GCallback)child_volume_unmounted, union_monitor);
  g_signal_connect (volume_monitor, "drive_connected", (GCallback)child_drive_connected, union_monitor);
  g_signal_connect (volume_monitor, "drive_disconnected", (GCallback)child_drive_disconnected, union_monitor);

  volumes = g_volume_monitor_get_mounted_volumes (volume_monitor);
  for (l = volumes; l != NULL; l = l->next)
    {
      volume = l->data;
      add_child_volume (union_monitor, volume, volume_monitor);
      g_object_unref (volume);
    }
  g_list_free (volumes);
  
  drives = g_volume_monitor_get_connected_drives (volume_monitor);
  for (l = drives; l != NULL; l = l->next)
    {
      drive = l->data;
      add_child_drive (union_monitor, drive);
      g_object_unref (drive);
    }
  g_list_free (drives);
}

static void
g_union_volume_monitor_remove_monitor (GUnionVolumeMonitor *union_monitor,
				       GVolumeMonitor *child_monitor)
{
  GList *l;
  GUnionVolume *union_volume;
  GVolume *volume;
  
  if (!g_list_find (union_monitor->monitors, child_monitor))
    return;

  for (l = union_monitor->volumes; l != NULL; l = l->next)
    {
      union_volume = l->data;
      
      volume = g_union_volume_get_child_for_monitor (union_volume, child_monitor);
      if (volume)
	{
	  remove_child_volume (union_monitor, volume);
	  g_object_unref (volume);
	}
    }
}

static void
g_union_volume_monitor_init (GUnionVolumeMonitor *union_monitor)
{
#ifdef G_OS_UNIX
  g_union_volume_monitor_add_monitor (union_monitor, g_unix_volume_monitor_new ());
#endif
}

static GUnionVolumeMonitor *
g_union_volume_monitor_new (void)
{
  GUnionVolumeMonitor *monitor;

  monitor = g_object_new (G_TYPE_UNION_VOLUME_MONITOR, NULL);
  
  return monitor;
}

GVolumeMonitor *
g_get_volume_monitor (void)
{
  GVolumeMonitor *vm;
  
  G_LOCK (the_volume_monitor);

  if (the_volume_monitor )
    vm = G_VOLUME_MONITOR (g_object_ref (the_volume_monitor));
  else
    {
      the_volume_monitor = g_union_volume_monitor_new ();
      vm = G_VOLUME_MONITOR (the_volume_monitor);
    }
  
  G_UNLOCK (the_volume_monitor);

  return vm;
}
