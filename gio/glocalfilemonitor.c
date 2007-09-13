#include <config.h>

#include "glocalfilemonitor.h"

#if defined(HAVE_LINUX_INOTIFY_H) || defined(HAVE_SYS_INOTIFY_H)
#define USE_INOTIFY 1
#include "inotify/inotify-helper.h"
#endif

#ifdef HAVE_FAM
#include "fam/fam-helper.h"
#endif

static gboolean g_local_file_monitor_cancel (GFileMonitor* monitor);

struct _GLocalFileMonitor
{
  GFileMonitor parent_instance;
  gchar *dirname;
  gchar *filename;
  void *private; /* backend stuff goes here */
};

G_DEFINE_TYPE (GLocalFileMonitor, g_local_file_monitor, G_TYPE_FILE_MONITOR)

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
g_local_file_monitor_class_init (GLocalFileMonitorClass* klass)
{
  GObjectClass* gobject_class = G_OBJECT_CLASS (klass);
  GFileMonitorClass *file_monitor_class = G_FILE_MONITOR_CLASS (klass);
  
  gobject_class->finalize = g_local_file_monitor_finalize;

  file_monitor_class->cancel = g_local_file_monitor_cancel;
}

static void
g_local_file_monitor_init (GLocalFileMonitor* local_monitor)
{
  local_monitor->private   = NULL;
  local_monitor->dirname   = NULL;
  local_monitor->filename  = NULL;
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
    if (!_ih_startup ())
      {
	g_object_unref (local_monitor);
	return NULL;
      }
    sub = _ih_sub_new (local_monitor->dirname, local_monitor->filename, local_monitor);
    if (!sub)
      {
	/* error */
      }
    if (_ih_sub_add (sub) == FALSE)
      {
	/* error */
      }
    local_monitor->private = sub;
  }
#elif HAVE_FAM
  {
    fam_sub* sub;
    sub = fam_sub_add (pathname, FALSE, local_monitor);
    if (!sub)
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
  
  if (local_monitor->dirname)
    g_free (local_monitor->dirname);
  local_monitor->dirname = NULL;

  if (local_monitor->filename)
    g_free (local_monitor->filename);
  local_monitor->filename = NULL;

#ifdef USE_INOTIFY
  {
    inotify_sub* sub = local_monitor->private;
    _ih_sub_cancel (sub);
    _ih_sub_free (sub);
  }
#elif HAVE_FAM
  {
    fam_sub* sub = local_monitor->private;
    if (!fam_sub_cancel (sub))
      {
        /* error */
      }
  }
#endif

  
  return TRUE;
}
