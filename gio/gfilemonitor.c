#include <config.h>
#include <string.h>

#include "gfilemonitorpriv.h"
#include "gvfs-marshal.h"
#include "gvfs.h"

enum {
  CHANGED,
  LAST_SIGNAL
};

G_DEFINE_TYPE (GFileMonitor, g_file_monitor, G_TYPE_OBJECT);

struct _GFileMonitorPrivate {
  gboolean cancelled;
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
g_file_monitor_finalize (GObject *object)
{
  GFileMonitor *monitor;

  monitor = G_FILE_MONITOR (object);
  
  if (G_OBJECT_CLASS (g_file_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_monitor_parent_class)->finalize) (object);
}

static void
g_file_monitor_dispose (GObject *object)
{
  GFileMonitor *monitor;
  
  monitor = G_FILE_MONITOR (object);

  /* Make sure we cancel on last unref */
  if (!monitor->priv->cancelled)
    g_file_monitor_cancel (monitor);
  
  if (G_OBJECT_CLASS (g_file_monitor_parent_class)->dispose)
    (*G_OBJECT_CLASS (g_file_monitor_parent_class)->dispose) (object);
}

static void
g_file_monitor_class_init (GFileMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GFileMonitorPrivate));
  
  gobject_class->finalize = g_file_monitor_finalize;
  gobject_class->dispose = g_file_monitor_dispose;

  signals[CHANGED] =
    g_signal_new (I_("changed"),
		  G_TYPE_FILE_MONITOR,
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GFileMonitorClass, changed),
		  NULL, NULL,
		  _gvfs_marshal_VOID__OBJECT_OBJECT_INT,
		  G_TYPE_NONE,3,
		  G_TYPE_FILE,
		  G_TYPE_FILE,
		  G_TYPE_INT);
}

static void
g_file_monitor_init (GFileMonitor *monitor)
{
  monitor->priv = G_TYPE_INSTANCE_GET_PRIVATE (monitor,
					       G_TYPE_FILE_MONITOR,
					       GFileMonitorPrivate);
}


gboolean
g_file_monitor_cancel (GFileMonitor* monitor)
{
  GFileMonitorClass *class;
  
  if (monitor->priv->cancelled)
    return TRUE;
  
  monitor->priv->cancelled = TRUE;
  
  class = G_FILE_MONITOR_GET_CLASS (monitor);
  return (* class->cancel) (monitor);
}

void
g_file_monitor_emit_event (GFileMonitor *monitor,
			   GFile *file,
			   GFile *other_file,
			   GFileMonitorEvent event_type)
{
  g_signal_emit (monitor, signals[CHANGED], 0, file, other_file, event_type);
}
