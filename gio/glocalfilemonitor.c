#include <config.h>

#include "glocalfilemonitor.h"

#if defined(HAVE_LINUX_INOTIFY_H) || defined(HAVE_SYS_INOTIFY_H)
#define USE_INOTIFY 1
#include "inotify/inotify-helper.h"
#endif

static void g_local_file_monitor_iface_init (GFileMonitorIface       *iface);

struct _GLocalFileMonitor
{
  GObject parent_instance;
  gchar *dirname;
  gchar *filename;
  gboolean cancelled;
  void *private; /* backend stuff goes here */
};

G_DEFINE_TYPE_WITH_CODE (GLocalFileMonitor, g_local_file_monitor, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE_MONITOR, g_local_file_monitor_iface_init))

static void
g_local_file_monitor_finalize (GObject* object)
{
  GLocalFileMonitor* local_monitor;
  
  local_monitor = G_LOCAL_FILE_MONITOR (object);

  if (local_monitor->dirname)
    g_free (local_monitor->dirname);
  if (local_monitor->filename)
    g_free (local_monitor->filename);
  
  if (G_OBJECT_CLASS (g_local_file_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_local_file_monitor_parent_class)->finalize) (object);
}

static void
g_local_file_monitor_dispose (GObject *object)
{
  GLocalFileMonitor* local_monitor;
  
  local_monitor = G_LOCAL_FILE_MONITOR (object);

  /* Make sure we cancel on last unref */
  if (!local_monitor->cancelled)
    g_file_monitor_cancel (G_FILE_MONITOR (object));
  
  if (G_OBJECT_CLASS (g_local_file_monitor_parent_class)->dispose)
    (*G_OBJECT_CLASS (g_local_file_monitor_parent_class)->dispose) (object);
}

static void
g_local_file_monitor_class_init (GLocalFileMonitorClass* klass)
{
  GObjectClass* gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_local_file_monitor_finalize;
  gobject_class->dispose = g_local_file_monitor_dispose;
}

static void
g_local_file_monitor_init (GLocalFileMonitor* local_monitor)
{
  local_monitor->private   = NULL;
  local_monitor->dirname   = NULL;
  local_monitor->filename  = NULL;
  local_monitor->cancelled = FALSE;
}

GFileMonitor*
g_local_file_monitor_start (const char* pathname)
{
  GLocalFileMonitor* local_monitor;

  local_monitor = g_object_new (G_TYPE_LOCAL_FILE_MONITOR, NULL);
  
  local_monitor->dirname = g_path_get_dirname (pathname);
  local_monitor->filename = g_path_get_basename (pathname);
  
#ifdef USE_INOTIFY
  {
    inotify_sub* sub;
    if (!ih_startup ())
      {
	g_object_unref (local_monitor);
	return NULL;
      }
    sub = ih_sub_new (local_monitor->dirname, local_monitor->filename, local_monitor);
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
  
  return G_FILE_MONITOR(local_monitor);
}

static gboolean
g_local_file_monitor_cancel (GFileMonitor* monitor)
{
  GLocalFileMonitor *local_monitor = G_LOCAL_FILE_MONITOR (monitor);
  
  if (local_monitor->cancelled)
    return TRUE;
  local_monitor->cancelled = TRUE;

  if (local_monitor->dirname)
    g_free (local_monitor->dirname);
  local_monitor->dirname = NULL;

  if (local_monitor->filename)
    g_free (local_monitor->filename);
  local_monitor->filename = NULL;

#ifdef USE_INOTIFY
  {
    inotify_sub* sub = local_monitor->private;
    ih_sub_cancel (sub);
    ih_sub_free (sub);
  }
#endif
  
  return TRUE;
}

static void
g_local_file_monitor_iface_init (GFileMonitorIface* iface)
{
  iface->cancel = g_local_file_monitor_cancel;
}
