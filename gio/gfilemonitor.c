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
  int rate_limit_msec;

  /* Rate limiting change events */
  guint32 last_sent_change_time; /* Some monitonic clock in msecs */
  GFile *last_sent_change_file;
  guint last_sent_change_timeout;

  /* Virtual CHANGES_DONE_HINT emission */
  GSource *last_recieved_change_timeout;
  GFile *last_recieved_change_file;
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
g_file_monitor_finalize (GObject *object)
{
  GFileMonitor *monitor;

  monitor = G_FILE_MONITOR (object);

  if (monitor->priv->last_sent_change_file)
    g_object_unref (monitor->priv->last_sent_change_file);

  if (monitor->priv->last_sent_change_timeout != 0)
    g_source_remove (monitor->priv->last_sent_change_timeout);

  if (monitor->priv->last_recieved_change_file)
    g_object_unref (monitor->priv->last_recieved_change_file);

  if (monitor->priv->last_recieved_change_timeout)
    g_source_destroy (monitor->priv->last_recieved_change_timeout);
  
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
  monitor->priv->rate_limit_msec = 800;
}


gboolean
g_file_monitor_is_cancelled (GFileMonitor *monitor)
{
  return monitor->priv->cancelled;
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
g_file_monitor_set_rate_limit (GFileMonitor *monitor,
			       int           limit_msecs)
{
  monitor->priv->rate_limit_msec = limit_msecs;
}

static guint32
time_difference (guint32 from, guint32 to)
{
  if (from > to)
    return 0;
  return to - from;
}

static void
remove_last_event (GFileMonitor *monitor, gboolean emit_first)
{
  if (monitor->priv->last_sent_change_file == NULL)
    return;
  
  if (emit_first)
    g_signal_emit (monitor, signals[CHANGED], 0,
		   monitor->priv->last_sent_change_file, NULL,
		   G_FILE_MONITOR_EVENT_CHANGED);
  
  if (monitor->priv->last_sent_change_file)
    {
      g_object_unref (monitor->priv->last_sent_change_file);
      monitor->priv->last_sent_change_file = NULL;
    }
  if (monitor->priv->last_sent_change_timeout)
    {
      g_source_remove (monitor->priv->last_sent_change_timeout);
      monitor->priv->last_sent_change_timeout = 0;
    }
}

static gboolean
delayed_changed_event_timeout (gpointer data)
{
  GFileMonitor *monitor = data;

  monitor->priv->last_sent_change_timeout = 0;

  remove_last_event (monitor, TRUE);
  
  return FALSE;
}

static void
schedule_delayed_change_timeout (GFileMonitor *monitor, GFile *file, guint32 time_since_last)
{
  guint32 time_left;
  
  if (monitor->priv->last_sent_change_timeout == 0) /* Only set the timeout once */
    {
      time_left = monitor->priv->rate_limit_msec - time_since_last;
      monitor->priv->last_sent_change_timeout = 
	g_timeout_add (time_left, delayed_changed_event_timeout, monitor);
    }
}

static void
remove_last_recived_event (GFileMonitor *monitor, gboolean emit_first)
{
  if (monitor->priv->last_recieved_change_file == NULL)
    return;

  if (emit_first)
    g_signal_emit (monitor, signals[CHANGED], 0,
		   monitor->priv->last_recieved_change_file, NULL,
		   G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT);

  if (monitor->priv->last_recieved_change_timeout)
    {
      g_source_destroy (monitor->priv->last_recieved_change_timeout);
      monitor->priv->last_recieved_change_timeout = NULL;
    }
}

static gboolean
virtual_changes_done_timeout (gpointer data)
{
  GFileMonitor *monitor = data;

  monitor->priv->last_recieved_change_timeout = NULL;
  
  remove_last_recived_event (monitor, TRUE);
  
  return FALSE;
}

static void
schedule_virtual_change_done_timeout (GFileMonitor *monitor, GFile *file)
{
  GSource *source;
  
  source = g_timeout_source_new_seconds (3);
  
  g_source_set_callback (source, virtual_changes_done_timeout, monitor, NULL);
  g_source_attach (source, NULL);
  monitor->priv->last_recieved_change_timeout = source;
  monitor->priv->last_recieved_change_file = g_object_ref (file);
  g_source_unref (source);
}

void
g_file_monitor_emit_event (GFileMonitor *monitor,
			   GFile *file,
			   GFile *other_file,
			   GFileMonitorEvent event_type)
{
  guint32 time_now, since_last;
  gboolean emit_now;

  if (event_type != G_FILE_MONITOR_EVENT_CHANGED)
    {
      remove_last_event (monitor, TRUE);
      remove_last_recived_event (monitor, TRUE);
      g_signal_emit (monitor, signals[CHANGED], 0, file, other_file, event_type);
    }
  else
    {
      time_now = g_thread_gettime() / (1000 * 1000);
      emit_now = TRUE;
      
      if (monitor->priv->last_sent_change_file)
	{
	  since_last = time_difference (monitor->priv->last_sent_change_time, time_now);
	  if (since_last > monitor->priv->rate_limit_msec)
	    {
	      /* Its been enought time so that we can emit the stored one, but
	       * we instead report the change we just got and forget the old one.
	       */
	      remove_last_event (monitor, FALSE);
	    }
	  else
	    {
	      /* We ignore this change, but arm a timer so that we can fire it later if we
		 don't get any other events (that kill this timeout) */
	      emit_now = FALSE;
	      schedule_delayed_change_timeout (monitor, file, since_last);
	    }
	}
      
      if (emit_now)
	{
	  g_signal_emit (monitor, signals[CHANGED], 0, file, other_file, event_type);
	  
	  monitor->priv->last_sent_change_time = time_now;
	  monitor->priv->last_sent_change_file = g_object_ref (file);
	  monitor->priv->last_sent_change_timeout = 0;
	}

      /* Schedule a virtual change done. This is removed if we get a real one, and
	 postponed if we get more change events. */
      remove_last_recived_event (monitor, FALSE);
      schedule_virtual_change_done_timeout (monitor, file);
    }
}
