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

typedef enum {
  BACKEND_NONE,
  BACKEND_INOTIFY,
  BACKEND_FAM,
} LocalMonitorBackend;

struct _GLocalFileMonitor
{
  GFileMonitor parent_instance;
  gchar *dirname;
  gchar *filename;
  LocalMonitorBackend active_backend;
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
g_local_file_monitor_new (const char* pathname)
{
  GLocalFileMonitor* local_monitor;
  LocalMonitorBackend backend;
  
  local_monitor = g_object_new (G_TYPE_LOCAL_FILE_MONITOR, NULL);
  
  local_monitor->dirname = g_path_get_dirname (pathname);
  local_monitor->filename = g_path_get_basename (pathname);

  backend = BACKEND_NONE;
  
#ifdef USE_INOTIFY
  if (backend == BACKEND_NONE)
    {
    inotify_sub* sub;
    if (_ih_startup ())
      {
	sub = _ih_sub_new (local_monitor->dirname, local_monitor->filename, local_monitor);
	if (sub)
	  {
	    if (_ih_sub_add (sub))
	      {
		local_monitor->private = sub;
		backend = BACKEND_INOTIFY;
	      }
	    else
	      _ih_sub_free (sub);
	  }
      }
  }
#endif

#ifdef HAVE_FAM
  if (backend == BACKEND_NONE)
    {
      fam_sub* sub;
      sub = _fam_sub_add (pathname, FALSE, local_monitor);
      if (sub)
	{
	  local_monitor->private = sub;
	  backend = BACKEND_FAM;
	}
    }
#endif
  
  local_monitor->active_backend = backend;


  if (backend != BACKEND_NONE)
    return G_FILE_MONITOR (local_monitor);

  g_object_unref (local_monitor);
  return NULL;
}

static gboolean
g_local_file_monitor_cancel (GFileMonitor* monitor)
{
  GLocalFileMonitor *local_monitor = G_LOCAL_FILE_MONITOR (monitor);
  
#ifdef USE_INOTIFY
  if (local_monitor->active_backend == BACKEND_INOTIFY)
    {
      inotify_sub* sub = local_monitor->private;
      if (sub)
	{
	  _ih_sub_cancel (sub);
	  _ih_sub_free (sub);
	  local_monitor->private = NULL;
	}
    }
#endif
  
#ifdef HAVE_FAM
  if (local_monitor->active_backend == BACKEND_FAM)
    {
      fam_sub* sub = local_monitor->private;
      if (sub)
	{
	  if (!_fam_sub_cancel (sub))
	    g_warning ("Unexpected error canceling fam monitor");
	  _fam_sub_free (sub);
	  local_monitor->private = NULL;
	}
    }
#endif

  return TRUE;
}
