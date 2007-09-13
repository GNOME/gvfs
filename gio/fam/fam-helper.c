#include "config.h"
#include <fam.h>
#include <gio/gfilemonitor.h>
#include <gio/gdirectorymonitor.h>
#include <gio/gfilemonitorpriv.h>
#include <gio/gdirectorymonitorpriv.h>

#include "fam-helper.h"

static FAMConnection* fam_connection = NULL;
static gint fam_watch_id = 0;
G_LOCK_DEFINE_STATIC(fam_connection);

struct _fam_sub
{
  gchar *pathname;
  gboolean directory;
  gpointer user_data;
  gboolean cancelled;
  FAMRequest request;
};

static GFileMonitorEvent 
fam_event_to_file_monitor_event (enum FAMCodes code)
{
  switch (code)
    {
    case FAMChanged:
      return G_FILE_MONITOR_EVENT_CHANGED;
      break;
    case FAMDeleted:
      return G_FILE_MONITOR_EVENT_DELETED;
      break;
    case FAMCreated:
      return G_FILE_MONITOR_EVENT_CREATED;
      break;
    default:
      return -1;
      break;
    }
}

static gboolean
fam_do_iter_unlocked (void)
{
  while (fam_connection != NULL && FAMPending (fam_connection)) {
    FAMEvent ev;
    fam_sub* sub = NULL;
    gboolean cancelled;
    
    if (FAMNextEvent (fam_connection, &ev) != 1) {
      FAMClose (fam_connection);
      g_free (fam_connection);
      g_source_remove (fam_watch_id);
      fam_watch_id = 0;
      fam_connection = NULL;
      return FALSE;
    }
    
    sub = (fam_sub*)ev.userdata;
    cancelled = sub->cancelled;
    if (ev.code == FAMAcknowledge && cancelled)
      {
	g_free (sub);
	continue;
      }
    
    if (cancelled)
      continue;
    
    if (sub->directory)
      {
	GDirectoryMonitor* monitor = G_DIRECTORY_MONITOR (sub->user_data);
	GFileMonitorEvent eflags = fam_event_to_file_monitor_event (ev.code);
	gchar* path = NULL;
	
	/* unsupported event */
	if (eflags == -1)
	  continue;
	
	if (ev.filename[0] == '/')
	  path = g_strdup (ev.filename);
	else
	  path = g_strdup_printf ("%s/%s", sub->pathname, ev.filename);
	GFile* child = g_file_new_for_path (path);
	GFile* parent = g_file_get_parent (child);
	g_directory_monitor_emit_event (monitor, child, NULL, eflags);
	g_free (path);
	g_object_unref (child);
	g_object_unref (parent);
      } else {
      GFileMonitor* monitor = G_FILE_MONITOR (sub->user_data);
      GFileMonitorEvent eflags = fam_event_to_file_monitor_event (ev.code);
      gchar* path = NULL;
      
      if (eflags == -1)
	continue;
      path = g_strdup (ev.filename);
      GFile* child = g_file_new_for_path (path);
      g_file_monitor_emit_event (monitor, child, NULL, eflags);
      g_free (path);
      g_object_unref (child);
    }
  }
  
  return TRUE;
}

static gboolean
fam_callback (GIOChannel *source,
              GIOCondition condition,
              gpointer data)
{
  gboolean res;
  G_LOCK (fam_connection);
  
  res = fam_do_iter_unlocked ();
  
  G_UNLOCK (fam_connection);
  return res;
}

static gboolean
fam_helper_startup (void)
{
  GIOChannel *ioc;
  
  G_LOCK (fam_connection);
  
  if (fam_connection == NULL) {
    fam_connection = g_new0 (FAMConnection, 1);
    if (FAMOpen2 (fam_connection, "gvfs user") != 0) {
      g_warning ("FAMOpen failed, FAMErrno=%d\n", FAMErrno);
      g_free (fam_connection);
      fam_connection = NULL;
      G_UNLOCK (fam_connection);
      return FALSE;
    }
#ifdef HAVE_FAM_NO_EXISTS
    /* This is a gamin extension that avoids sending all the Exists event for dir monitors */
    FAMNoExists (fam_connection);
#endif
    ioc = g_io_channel_unix_new (FAMCONNECTION_GETFD(fam_connection));
    fam_watch_id = g_io_add_watch (ioc,
				   G_IO_IN | G_IO_HUP | G_IO_ERR,
				   fam_callback, fam_connection);
    g_io_channel_unref (ioc);
  }
  
  G_UNLOCK (fam_connection);
  
  return TRUE;
}

fam_sub*
fam_sub_add (const gchar* pathname,
             gboolean directory,
	     gpointer user_data)
{
  if (!fam_helper_startup ())
    return NULL;
  
  fam_sub* sub = g_new0 (fam_sub, 1);
  sub->pathname = g_strdup (pathname);
  sub->directory = directory;
  sub->user_data = user_data;
  
  G_LOCK (fam_connection);
  /* We need to queue up incoming messages to avoid blocking on write
   *  if there are many monitors being canceled */
  fam_do_iter_unlocked ();
  
  if (fam_connection == NULL) {
    G_UNLOCK (fam_connection);
    return NULL;
  }
  
  if (directory)
    FAMMonitorDirectory (fam_connection, pathname, &sub->request, sub);
  else
    FAMMonitorFile (fam_connection, pathname, &sub->request, sub);
  
  G_UNLOCK (fam_connection);
  return sub;
}

gboolean
fam_sub_cancel (fam_sub* sub)
{
  if (sub->cancelled)
    return TRUE;
  
  sub->cancelled = TRUE;
  
  G_LOCK (fam_connection);
  /* We need to queue up incoming messages to avoid blocking on write
   *  if there are many monitors being canceled */
  fam_do_iter_unlocked ();
  
  if (fam_connection == NULL) {
    G_UNLOCK (fam_connection);
    return FALSE;
  }
  
  FAMCancelMonitor (fam_connection, &sub->request);
  
  G_UNLOCK (fam_connection);
  
  return TRUE;
}
