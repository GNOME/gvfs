#include <config.h>

#include "gdaemonfilemonitor.h"

static gboolean g_daemon_file_monitor_cancel (GFileMonitor* monitor);

struct _GDaemonFileMonitor
{
  GFileMonitor parent_instance;
};

G_DEFINE_TYPE (GDaemonFileMonitor, g_daemon_file_monitor, G_TYPE_FILE_MONITOR)

static void
g_daemon_file_monitor_finalize (GObject* object)
{
  GDaemonFileMonitor* daemon_monitor;
  
  daemon_monitor = G_DAEMON_FILE_MONITOR (object);

  
  if (G_OBJECT_CLASS (g_daemon_file_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_file_monitor_parent_class)->finalize) (object);
}


static void
g_daemon_file_monitor_class_init (GDaemonFileMonitorClass* klass)
{
  GObjectClass* gobject_class = G_OBJECT_CLASS (klass);
  GFileMonitorClass *file_monitor_class = G_FILE_MONITOR_CLASS (klass);
  
  gobject_class->finalize = g_daemon_file_monitor_finalize;

  file_monitor_class->cancel = g_daemon_file_monitor_cancel;
}

static void
g_daemon_file_monitor_init (GDaemonFileMonitor* daemon_monitor)
{
}

GFileMonitor*
g_daemon_file_monitor_new (void)
{
  GDaemonFileMonitor* daemon_monitor;
  DaemonMonitorBackend backend;
  
  daemon_monitor = g_object_new (G_TYPE_DAEMON_FILE_MONITOR, NULL);
  
  return G_FILE_MONITOR (daemon_monitor);
}

static gboolean
g_daemon_file_monitor_cancel (GFileMonitor* monitor)
{
  GDaemonFileMonitor *daemon_monitor = G_DAEMON_FILE_MONITOR (monitor);
  

  return TRUE;
}
