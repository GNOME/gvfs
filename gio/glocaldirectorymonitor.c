#include <config.h>

#include "glocaldirectorymonitor.h"

#if defined(HAVE_LINUX_INOTIFY_H) || defined(HAVE_SYS_INOTIFY_H)
#define USE_INOTIFY 1
#include "inotify/inotify-helper.h"
#endif

#ifdef HAVE_FAM
#include "fam/fam-helper.h"
#endif

#include "gunixmounts.h"
#include "gdirectorymonitorpriv.h"

static gboolean g_local_directory_monitor_cancel (GDirectoryMonitor* monitor);

typedef enum {
  BACKEND_NONE,
  BACKEND_INOTIFY,
  BACKEND_FAM,
} LocalMonitorBackend;

struct _GLocalDirectoryMonitor
{
  GDirectoryMonitor parent_instance;
  gchar *dirname;
  gpointer mount_monitor;
  gboolean was_mounted;
  LocalMonitorBackend active_backend;
  void *private; /* backend stuff goes here */
};

G_DEFINE_TYPE (GLocalDirectoryMonitor, g_local_directory_monitor, G_TYPE_DIRECTORY_MONITOR)

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
g_local_directory_monitor_class_init (GLocalDirectoryMonitorClass* klass)
{
  GObjectClass* gobject_class = G_OBJECT_CLASS (klass);
  GDirectoryMonitorClass *dir_monitor_class = G_DIRECTORY_MONITOR_CLASS (klass);
  
  gobject_class->finalize = g_local_directory_monitor_finalize;

  dir_monitor_class->cancel = g_local_directory_monitor_cancel;
}

static void
g_local_directory_monitor_init (GLocalDirectoryMonitor* local_monitor)
{
  local_monitor->private   = NULL;
  local_monitor->dirname   = NULL;
}

static void
mounts_changed (gpointer user_data)
{
  GLocalDirectoryMonitor *local_monitor = user_data;
  GUnixMount *mount;
  gboolean is_mounted;
  GFile *file;
  
  /* Emulate unmount detection */
  
  mount = _g_get_unix_mount_at (local_monitor->dirname);
  
  is_mounted = mount != NULL;
  
  if (mount)
    _g_unix_mount_free (mount);

  if (local_monitor->was_mounted != is_mounted)
    {
      if (local_monitor->was_mounted && !is_mounted)
	{
	  file = g_file_new_for_path (local_monitor->dirname);
	  g_directory_monitor_emit_event (G_DIRECTORY_MONITOR (local_monitor),
					  file, NULL,
					  G_FILE_MONITOR_EVENT_UNMOUNTED);
	  g_object_unref (file);
	}
      local_monitor->was_mounted = is_mounted;
    }
}

GDirectoryMonitor*
g_local_directory_monitor_new (const char* dirname,
			       GFileMonitorFlags flags)
{
  GLocalDirectoryMonitor* local_monitor;
  LocalMonitorBackend backend;
  
  local_monitor = g_object_new (G_TYPE_LOCAL_DIRECTORY_MONITOR, NULL);
  
  backend = BACKEND_NONE;
  
#ifdef USE_INOTIFY
  if (backend == BACKEND_NONE)
    {
      inotify_sub* sub;
      if (_ih_startup ())
	{
	  sub = _ih_sub_new (dirname, NULL, local_monitor);
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
      sub = _fam_sub_add (dirname, TRUE, local_monitor);
      if (sub)
	{
	  local_monitor->private = sub;
	  backend = BACKEND_FAM;
	}
    }
#endif
  
  local_monitor->dirname = g_strdup (dirname);

  if (backend == BACKEND_NONE)
    {
      g_object_unref (local_monitor);
      return NULL;
    }

  if (backend != BACKEND_INOTIFY &&
      ((flags & G_FILE_MONITOR_FLAGS_MONITOR_MOUNTS)))
    {
      GUnixMount *mount;

      /* Emulate unmount detection */

      mount = _g_get_unix_mount_at (local_monitor->dirname);
      
      local_monitor->was_mounted = mount != NULL;

      if (mount)
	_g_unix_mount_free (mount);
	
      local_monitor->mount_monitor = _g_monitor_unix_mounts (NULL, mounts_changed, local_monitor);
     
    }
  
  return G_DIRECTORY_MONITOR (local_monitor);
}

static gboolean
g_local_directory_monitor_cancel (GDirectoryMonitor* monitor)
{
  GLocalDirectoryMonitor *local_monitor = G_LOCAL_DIRECTORY_MONITOR (monitor);

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

  if (local_monitor->mount_monitor)
    {
      _g_stop_monitoring_unix_mounts (local_monitor->mount_monitor);
      local_monitor->mount_monitor = NULL;
    }

  return TRUE;
}

