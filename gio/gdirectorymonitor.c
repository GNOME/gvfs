#include <config.h>
#include <string.h>

#include "gdirectorymonitorpriv.h"
#include "gvfs-marshal.h"
#include "gfile.h"
#include "gvfs.h"

enum {
  CHANGED,
  LAST_SIGNAL
};

G_DEFINE_TYPE (GDirectoryMonitor, g_directory_monitor, G_TYPE_OBJECT);

struct _GDirectoryMonitorPrivate {
  gboolean cancelled;
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
g_directory_monitor_finalize (GObject *object)
{
  GDirectoryMonitor *monitor;

  monitor = G_DIRECTORY_MONITOR (object);
  
  if (G_OBJECT_CLASS (g_directory_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_directory_monitor_parent_class)->finalize) (object);
}

static void
g_directory_monitor_dispose (GObject *object)
{
  GDirectoryMonitor *monitor;
  
  monitor = G_DIRECTORY_MONITOR (object);

  /* Make sure we cancel on last unref */
  if (!monitor->priv->cancelled)
    g_directory_monitor_cancel (monitor);
  
  if (G_OBJECT_CLASS (g_directory_monitor_parent_class)->dispose)
    (*G_OBJECT_CLASS (g_directory_monitor_parent_class)->dispose) (object);
}

static void
g_directory_monitor_class_init (GDirectoryMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GDirectoryMonitorPrivate));
  
  gobject_class->finalize = g_directory_monitor_finalize;
  gobject_class->dispose = g_directory_monitor_dispose;

  signals[CHANGED] =
    g_signal_new (I_("changed"),
		  G_TYPE_DIRECTORY_MONITOR,
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GDirectoryMonitorClass, changed),
		  NULL, NULL,
		  _gvfs_marshal_VOID__OBJECT_OBJECT_INT,
		  G_TYPE_NONE,3,
		  G_TYPE_FILE,
		  G_TYPE_FILE,
		  G_TYPE_INT);
}

static void
g_directory_monitor_init (GDirectoryMonitor *monitor)
{
  monitor->priv = G_TYPE_INSTANCE_GET_PRIVATE (monitor,
					       G_TYPE_DIRECTORY_MONITOR,
					       GDirectoryMonitorPrivate);
}


gboolean
g_directory_monitor_cancel (GDirectoryMonitor* monitor)
{
  GDirectoryMonitorClass *class;
  
  if (monitor->priv->cancelled)
    return TRUE;
  
  monitor->priv->cancelled = TRUE;
  
  class = G_DIRECTORY_MONITOR_GET_CLASS (monitor);
  return (* class->cancel) (monitor);
}

void
g_directory_monitor_emit_event (GDirectoryMonitor *monitor,
				GFile *child,
				GFile *other_file,
				GFileMonitorEvent event_type)
{
  g_signal_emit (monitor, signals[CHANGED], 0, child, other_file, event_type);
}
