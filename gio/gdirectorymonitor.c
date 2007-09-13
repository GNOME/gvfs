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

typedef struct {
  GFile *file;
  guint32 last_sent_change_time; /* 0 == not sent */
  guint32 send_delayed_change_at; /* 0 == never */
  guint32 send_virtual_changes_done_at; /* 0 == never */
} RateLimiter;

struct _GDirectoryMonitorPrivate {
  gboolean cancelled;
  int rate_limit_msec;

  GHashTable *rate_limiter;
  
  GSource *timeout;
  guint32 timeout_fires_at;
};

#define DEFAULT_RATE_LIMIT_MSECS 800
#define DEFAULT_VIRTUAL_CHANGES_DONE_DELAY_SECS 2

static guint signals[LAST_SIGNAL] = { 0 };

static void
rate_limiter_free (RateLimiter *limiter)
{
  g_object_unref (limiter->file);
  g_free (limiter);
}

static void
g_directory_monitor_finalize (GObject *object)
{
  GDirectoryMonitor *monitor;

  monitor = G_DIRECTORY_MONITOR (object);

  if (monitor->priv->timeout)
    {
      g_source_destroy (monitor->priv->timeout);
      g_source_unref (monitor->priv->timeout);
    }

  g_hash_table_destroy (monitor->priv->rate_limiter);
  
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

  monitor->priv->rate_limit_msec = DEFAULT_RATE_LIMIT_MSECS;
  monitor->priv->rate_limiter = g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal,
						       NULL, (GDestroyNotify) rate_limiter_free);
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
g_directory_monitor_set_rate_limit (GDirectoryMonitor *monitor,
				    int limit_msecs)
{
  monitor->priv->rate_limit_msec = limit_msecs;
}

gboolean
g_directory_monitor_is_cancelled (GDirectoryMonitor *monitor)
{
  return monitor->priv->cancelled;
}

static guint32
get_time_msecs (void)
{
  return g_thread_gettime() / (1000 * 1000);
}

static guint32
time_difference (guint32 from, guint32 to)
{
  if (from > to)
    return 0;
  return to - from;
}

static RateLimiter *
new_limiter (GDirectoryMonitor *monitor,
	     GFile *file)
{
  RateLimiter *limiter;

  limiter = g_new0 (RateLimiter, 1);
  limiter->file = g_object_ref (file);
  g_hash_table_insert (monitor->priv->rate_limiter, file, limiter);
  
  return limiter;
}

static void
rate_limiter_send_virtual_changes_done_now (GDirectoryMonitor *monitor, RateLimiter *limiter)
{
  if (limiter->send_virtual_changes_done_at != 0)
    {
      g_signal_emit (monitor, signals[CHANGED], 0, limiter->file, NULL, G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT);
      limiter->send_virtual_changes_done_at = 0;
    }
}

static void
rate_limiter_send_delayed_change_now (GDirectoryMonitor *monitor, RateLimiter *limiter, guint32 time_now)
{
  if (limiter->send_delayed_change_at != 0)
    {
      g_signal_emit (monitor, signals[CHANGED], 0, limiter->file, NULL, G_FILE_MONITOR_EVENT_CHANGED);
      limiter->send_delayed_change_at = 0;
      limiter->last_sent_change_time = time_now;
    }
}

typedef struct {
  guint32 min_time;
  guint32 time_now;
  GDirectoryMonitor *monitor;
} ForEachData;

static gboolean
calc_min_time (GDirectoryMonitor *monitor, RateLimiter *limiter, guint32 time_now, guint32 *min_time)
{
  gboolean delete_me;
  guint32 expire_at;

  delete_me = TRUE;

  if (limiter->last_sent_change_time != 0)
    {
      /* Set a timeout at 2*rate limit so that we can clear out the change from the hash eventualy */
      expire_at = limiter->last_sent_change_time + 2 * monitor->priv->rate_limit_msec;

      if (time_difference (time_now, expire_at) > 0)
	{
	  delete_me = FALSE;
	  *min_time = MIN (*min_time,
			   time_difference (time_now, expire_at));
	}
    }

  if (limiter->send_delayed_change_at != 0)
    {
      delete_me = FALSE;
      *min_time = MIN (*min_time,
		       time_difference (time_now, limiter->send_delayed_change_at));
    }

  if (limiter->send_virtual_changes_done_at != 0)
    {
      delete_me = FALSE;
      *min_time = MIN (*min_time,
		       time_difference (time_now, limiter->send_virtual_changes_done_at));
    }

  return delete_me;
}

static gboolean
foreach_rate_limiter_fire (gpointer  key,
			   gpointer  value,
			   gpointer  user_data)
{
  RateLimiter *limiter = value;
  ForEachData *data = user_data;

  if (limiter->send_delayed_change_at != 0 &&
      time_difference (data->time_now, limiter->send_delayed_change_at) == 0)
    rate_limiter_send_delayed_change_now (data->monitor, limiter, data->time_now);

  if (limiter->send_virtual_changes_done_at != 0 &&
      time_difference (data->time_now, limiter->send_virtual_changes_done_at) == 0)
    rate_limiter_send_virtual_changes_done_now (data->monitor, limiter);

  return calc_min_time (data->monitor, limiter, data->time_now, &data->min_time);
}

static gboolean 
rate_limiter_timeout (gpointer timeout_data)
{
  GDirectoryMonitor *monitor = timeout_data;
  ForEachData data;
  GSource *source;
  
  data.min_time = G_MAXUINT32;
  data.monitor = monitor;
  data.time_now = get_time_msecs ();
  g_hash_table_foreach_remove (monitor->priv->rate_limiter,
			       foreach_rate_limiter_fire,
			       &data);

  /* Remove old timeout */
  if (monitor->priv->timeout)
    {
      g_source_destroy (monitor->priv->timeout);
      g_source_unref (monitor->priv->timeout);
      monitor->priv->timeout_fires_at = 0;
    }
  
  /* Set up new timeout */
  if (data.min_time != G_MAXUINT32)
    {
      source = g_timeout_source_new (data.min_time + 1); /* + 1 to make sure we've really passed the time */
      g_source_set_callback (source, rate_limiter_timeout, monitor, NULL);
      g_source_attach (source, NULL);
      
      monitor->priv->timeout = source;
      monitor->priv->timeout_fires_at = data.time_now + data.min_time; 
    }
  
  return FALSE;
}

static gboolean
foreach_rate_limiter_update (gpointer  key,
			     gpointer  value,
			     gpointer  user_data)
{
  RateLimiter *limiter = value;
  ForEachData *data = user_data;

  return calc_min_time (data->monitor, limiter, data->time_now, &data->min_time);
}

static void
update_rate_limiter_timeout (GDirectoryMonitor *monitor, guint new_time)
{
  ForEachData data;
  GSource *source;
  
  if (monitor->priv->timeout_fires_at != 0 && new_time != 0 &&
      time_difference (new_time, monitor->priv->timeout_fires_at) == 0)
    return; /* Nothing to do, we already fire earlier than that */

  data.min_time = G_MAXUINT32;
  data.monitor = monitor;
  data.time_now = get_time_msecs ();
  g_hash_table_foreach_remove (monitor->priv->rate_limiter,
			       foreach_rate_limiter_update,
			       &data);

  /* Remove old timeout */
  if (monitor->priv->timeout)
    {
      g_source_destroy (monitor->priv->timeout);
      g_source_unref (monitor->priv->timeout);
      monitor->priv->timeout_fires_at = 0;
    }

  /* Set up new timeout */
  if (data.min_time != G_MAXUINT32)
    {
      source = g_timeout_source_new (data.min_time + 1);  /* + 1 to make sure we've really passed the time */
      g_source_set_callback (source, rate_limiter_timeout, monitor, NULL);
      g_source_attach (source, NULL);
      
      monitor->priv->timeout = source;
      monitor->priv->timeout_fires_at = data.time_now + data.min_time; 
    }
}

void
g_directory_monitor_emit_event (GDirectoryMonitor *monitor,
				GFile *child,
				GFile *other_file,
				GFileMonitorEvent event_type)
{
  guint32 time_now, since_last;
  gboolean emit_now;
  RateLimiter *limiter;

  limiter = g_hash_table_lookup (monitor->priv->rate_limiter, child);

  if (event_type != G_FILE_MONITOR_EVENT_CHANGED)
    {
      if (limiter)
	{
	  rate_limiter_send_delayed_change_now (monitor, limiter, get_time_msecs ());
	  if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
	    limiter->send_virtual_changes_done_at = 0;
	  else
	    rate_limiter_send_virtual_changes_done_now (monitor, limiter);
	  update_rate_limiter_timeout (monitor, 0);
	}
      g_signal_emit (monitor, signals[CHANGED], 0, child, other_file, event_type);
    }
  else
    {
      /* Changed event, rate limit */
      time_now = get_time_msecs ();
      emit_now = TRUE;
      
      if (limiter)
	{
	  since_last = time_difference (limiter->last_sent_change_time, time_now);
	  if (since_last < monitor->priv->rate_limit_msec)
	    {
	      /* We ignore this change, but arm a timer so that we can fire it later if we
		 don't get any other events (that kill this timeout) */
	      emit_now = FALSE;
	      if (limiter->send_delayed_change_at == 0)
		{
		  limiter->send_delayed_change_at = time_now + monitor->priv->rate_limit_msec;
		  update_rate_limiter_timeout (monitor, limiter->send_delayed_change_at);
		}
	    }
	}

      if (limiter == NULL)
	limiter = new_limiter (monitor, child);
      
      if (emit_now)
	{
	  g_signal_emit (monitor, signals[CHANGED], 0, child, other_file, event_type);

	  limiter->last_sent_change_time = time_now;
	  limiter->send_delayed_change_at = 0;
	  /* Set a timeout of 2*rate limit so that we can clear out the change from the hash eventualy */
	  update_rate_limiter_timeout (monitor, time_now + 2 * monitor->priv->rate_limit_msec);
	}

      /* Schedule a virtual change done. This is removed if we get a real one, and
	 postponed if we get more change events. */

      limiter->send_virtual_changes_done_at = time_now + DEFAULT_VIRTUAL_CHANGES_DONE_DELAY_SECS * 1000;
      update_rate_limiter_timeout (monitor, limiter->send_virtual_changes_done_at);
    }
}
