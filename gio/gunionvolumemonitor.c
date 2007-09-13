#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "gunionvolumemonitor.h"

G_DEFINE_TYPE (GUnionVolumeMonitor, g_union_volume_monitor, G_TYPE_VOLUME_MONITOR);

G_LOCK_DEFINE_STATIC(the_volume_monitor);
static GUnionVolumeMonitor *the_volume_monitor = NULL;

static void
g_union_volume_monitor_finalize (GObject *object)
{
  GUnionVolumeMonitor *monitor;
  
  monitor = G_UNION_VOLUME_MONITOR (object);
  
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


static void
g_union_volume_monitor_class_init (GUnionVolumeMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_union_volume_monitor_finalize;
  gobject_class->dispose = g_union_volume_monitor_dispose;
}

static void
g_union_volume_monitor_init (GUnionVolumeMonitor *info)
{
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
