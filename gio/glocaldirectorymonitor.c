#include <config.h>

#include "glocaldirectorymonitor.h"

#if defined(HAVE_LINUX_INOTIFY_H) || defined(HAVE_SYS_INOTIFY_H)
#define USE_INOTIFY 1
#include "inotify/inotify-helper.h"
#endif

static void g_local_directory_monitor_iface_init (GDirectoryMonitorIface       *iface);

struct _GLocalDirectoryMonitor
{
  GObject parent_instance;
  gchar *dirname;
  gboolean cancelled;
  void *private; /* backend stuff goes here */
};

G_DEFINE_TYPE_WITH_CODE (GLocalDirectoryMonitor, g_local_directory_monitor, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_DIRECTORY_MONITOR, g_local_directory_monitor_iface_init))

static void
g_local_directory_monitor_finalize (GObject* object)
{
  GLocalDirectoryMonitor* local_monitor;
  local_monitor = G_LOCAL_DIRECTORY_MONITOR (object);

  if (local_monitor->dirname)
    g_free (local_monitor->dirname);
  
  if (G_OBJECT_CLASS (g_local_directory_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_local_directory_monitor_parent_class)->finalize) (object);
}

static void
g_local_directory_monitor_dispose (GObject *object)
{
  GLocalDirectoryMonitor* local_monitor;
  
  local_monitor = G_LOCAL_DIRECTORY_MONITOR (object);

  /* Make sure we cancel on last unref */
  if (!local_monitor->cancelled)
    g_directory_monitor_cancel (G_DIRECTORY_MONITOR (object));
  
  if (G_OBJECT_CLASS (g_local_directory_monitor_parent_class)->dispose)
    (*G_OBJECT_CLASS (g_local_directory_monitor_parent_class)->dispose) (object);
}

static void
g_local_directory_monitor_class_init (GLocalDirectoryMonitorClass* klass)
{
  GObjectClass* gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_local_directory_monitor_finalize;
  gobject_class->dispose = g_local_directory_monitor_dispose;
}

static void
g_local_directory_monitor_init (GLocalDirectoryMonitor* local_monitor)
{
  local_monitor->private   = NULL;
  local_monitor->dirname   = NULL;
  local_monitor->cancelled = FALSE;
}

GDirectoryMonitor*
g_local_directory_monitor_start (const char* dirname)
{
  GLocalDirectoryMonitor* local_monitor;
  
  local_monitor = g_object_new (G_TYPE_LOCAL_DIRECTORY_MONITOR, NULL);
  
#ifdef USE_INOTIFY
  {
    inotify_sub* sub;
    
    if (!ih_startup ())
      {
	g_object_unref (local_monitor);
	return NULL;
      }
    sub = ih_sub_new (dirname, NULL, local_monitor);
    if (!sub)
      {
	/* error */
      }
    if (ih_sub_add (sub) == FALSE)
      {
	/* error */
      }
    local_monitor->private = sub;
  }
#endif
  
  local_monitor->dirname = g_strdup (dirname);
  
  return G_DIRECTORY_MONITOR (local_monitor);
}

static gboolean
g_local_directory_monitor_cancel (GDirectoryMonitor* monitor)
{
  GLocalDirectoryMonitor *local_monitor = G_LOCAL_DIRECTORY_MONITOR (monitor);
  
  if (local_monitor->cancelled)
    return TRUE;
  local_monitor->cancelled = TRUE;
  
  if (local_monitor->dirname)
    g_free (local_monitor->dirname);
  local_monitor->dirname = NULL;

#ifdef USE_INOTIFY
  {
    inotify_sub* sub = local_monitor->private;
    ih_sub_cancel (sub);
    ih_sub_free (sub);
  }
#endif
  
  g_object_unref (monitor);
  return TRUE;
}

static void
g_local_directory_monitor_iface_init (GDirectoryMonitorIface* iface)
{
  iface->cancel = g_local_directory_monitor_cancel;
}
