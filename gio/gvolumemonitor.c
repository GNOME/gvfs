#include <config.h>
#include "gvolumemonitor.h"
#include <glib/gi18n-lib.h>

G_DEFINE_TYPE (GVolumeMonitor, g_volume_monitor, G_TYPE_OBJECT);

enum {
  VOLUME_MOUNTED,
  VOLUME_PRE_UNMOUNT,
  VOLUME_UNMOUNTED,
  DRIVE_CONNECTED,
  DRIVE_DISCONNECTED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };


static void
g_volume_monitor_finalize (GObject *object)
{
  GVolumeMonitor *monitor;

  monitor = G_VOLUME_MONITOR (object);

  if (G_OBJECT_CLASS (g_volume_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_volume_monitor_parent_class)->finalize) (object);
}

static void
g_volume_monitor_class_init (GVolumeMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_volume_monitor_finalize;

  signals[VOLUME_MOUNTED] = g_signal_new (I_("volume_mounted"),
					  G_TYPE_VOLUME_MONITOR,
					  G_SIGNAL_RUN_LAST,
					  G_STRUCT_OFFSET (GVolumeMonitorClass, volume_mounted),
					  NULL, NULL,
					  g_cclosure_marshal_VOID__OBJECT,
					  G_TYPE_NONE, 1, G_TYPE_VOLUME);
  
  signals[VOLUME_PRE_UNMOUNT] = g_signal_new (I_("volume_pre_unmount"),
					      G_TYPE_VOLUME_MONITOR,
					      G_SIGNAL_RUN_LAST,
					      G_STRUCT_OFFSET (GVolumeMonitorClass, volume_pre_unmount),
					      NULL, NULL,
					      g_cclosure_marshal_VOID__OBJECT,
					      G_TYPE_NONE, 1, G_TYPE_VOLUME);
  
  signals[VOLUME_UNMOUNTED] = g_signal_new (I_("volume_unmounted"),
					    G_TYPE_VOLUME_MONITOR,
					    G_SIGNAL_RUN_LAST,
					    G_STRUCT_OFFSET (GVolumeMonitorClass, volume_unmounted),
					    NULL, NULL,
					    g_cclosure_marshal_VOID__OBJECT,
					    G_TYPE_NONE, 1, G_TYPE_VOLUME);

  signals[DRIVE_CONNECTED] = g_signal_new (I_("drive_connected"),
					   G_TYPE_VOLUME_MONITOR,
					   G_SIGNAL_RUN_LAST,
					   G_STRUCT_OFFSET (GVolumeMonitorClass, drive_connected),
					   NULL, NULL,
					   g_cclosure_marshal_VOID__OBJECT,
					   G_TYPE_NONE, 1, G_TYPE_DRIVE);
  
  
  signals[DRIVE_DISCONNECTED] = g_signal_new (I_("drive_disconnected"),
					      G_TYPE_VOLUME_MONITOR,
					      G_SIGNAL_RUN_LAST,
					      G_STRUCT_OFFSET (GVolumeMonitorClass, drive_disconnected),
					      NULL, NULL,
					      g_cclosure_marshal_VOID__OBJECT,
					      G_TYPE_NONE, 1, G_TYPE_DRIVE);
}

static void
g_volume_monitor_init (GVolumeMonitor *monitor)
{
}


GList *
g_volume_monitor_get_mounted_volumes  (GVolumeMonitor *volume_monitor)
{
  GVolumeMonitorClass *class;

  class = G_VOLUME_MONITOR_GET_CLASS (volume_monitor);

  return class->get_mounted_volumes (volume_monitor);
}

GList *
g_volume_monitor_get_connected_drives (GVolumeMonitor *volume_monitor)
{
  GVolumeMonitorClass *class;

  class = G_VOLUME_MONITOR_GET_CLASS (volume_monitor);

  return class->get_connected_drives (volume_monitor);
}

