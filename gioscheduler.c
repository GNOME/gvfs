#include <config.h>

#include "gioscheduler.h"

struct _GIOJob {
  gint id;
  GIOJobFunc job_func;
  GIODataFunc cancel_func; /* Runs under job map lock */
  gpointer data;
  GDestroyNotify destroy_notify;
  
  GMainContext *callback_context;
  gint io_priority;
  gboolean cancelled;
};

static GHashTable *job_map = NULL;
static GThreadPool *job_thread_pool = NULL;
static gint next_job_id = 1;
/* Serializes access to the job_map hash, and handles
 * lifetime issues for the jobs. (i.e. other than the
 * io thread you can only access the job when the job_map
 * lock is held) */
G_LOCK_DEFINE_STATIC(job_map);

static void io_job_thread (gpointer       data,
			   gpointer       user_data);


static gint
g_io_job_compare (gconstpointer  a,
		  gconstpointer  b,
		  gpointer       user_data)
{
  const GIOJob *aa = a;
  const GIOJob *bb = b;

  /* Always run cancelled ops first, they are quick and should be gotten rid of */
  if (aa->cancelled && !bb->cancelled)
    return -1;
  if (!aa->cancelled && bb->cancelled)
    return 1;

  /* Lower value => higher priority */
  if (aa->io_priority < bb->io_priority)
    return -1;
  if (aa->io_priority == bb->io_priority)
    return 0;
  return 1;
}

static void
init_scheduler (void)
{
  if (job_map == NULL)
    {
      /* TODO: thread_pool_new can fail */
      job_thread_pool = g_thread_pool_new (io_job_thread,
					   NULL,
					   10,
					   FALSE,
					   NULL);
      g_thread_pool_set_sort_function (job_thread_pool,
				       g_io_job_compare,
				       NULL);
      job_map = g_hash_table_new (g_direct_hash, g_direct_equal);
    }
}

static void
io_job_thread (gpointer       data,
	       gpointer       user_data)
{
  GIOJob *job = data;

  job->job_func (job, job->data);

  /* Note: We can still get cancel calls here if the job didn't
   * mark itself done, which means we can't free the data until
   * after removal from the job_map.
   */
  
  g_io_job_mark_done (job);

  if (job->destroy_notify)
    job->destroy_notify (job->data);

  g_main_context_unref (job->callback_context);
  g_free (job);
}


gint
g_schedule_io_job (GIOJobFunc    job_func,
		   GIODataFunc   cancel_func,
		   gpointer      data,
		   GDestroyNotify notify,
		   gint          io_priority,
		   GMainContext *callback_context)
{
  GIOJob *job;
  gint id;

  if (callback_context == NULL)
    callback_context = g_main_context_default ();
  
  job = g_new0 (GIOJob, 1);
  job->id = g_atomic_int_exchange_and_add (&next_job_id, 1);
  job->job_func = job_func;
  job->cancel_func = cancel_func;
  job->data = data;
  job->destroy_notify = notify;
  job->io_priority = io_priority;
  job->callback_context = g_main_context_ref (callback_context);
  job->cancelled = FALSE;
  
  G_LOCK (job_map);
  init_scheduler ();

  g_hash_table_insert (job_map, GINT_TO_POINTER (job->id), job);
  
  /* TODO: We ignore errors */
  g_thread_pool_push (job_thread_pool, job, NULL);

  id = job->id;
  G_UNLOCK (job_map);

  return id;
}

  
void
g_cancel_io_job (gint id)
{
  GIOJob *job;
  
  G_LOCK (job_map);
  init_scheduler ();

  job = g_hash_table_lookup (job_map, GINT_TO_POINTER (id));

  if (job && !job->cancelled)
    {
      job->cancelled = TRUE;
      if (job->cancel_func)
	job->cancel_func (job->data);
    }
  
  G_UNLOCK (job_map);
}

/* Called with job_map lock held */
static void
foreach_job_cancel (gpointer       key,
		    gpointer       value,
		    gpointer       user_data)
{
  GIOJob *job = value;

  if (!job->cancelled)
    {
      job->cancelled = TRUE;
      if (job->cancel_func)
	job->cancel_func (job->data);
    }
}

void
g_cancel_all_io_jobs (void)
{
  G_LOCK (job_map);
  init_scheduler ();
  g_hash_table_foreach (job_map,
			foreach_job_cancel,
			NULL);
  
  
  G_UNLOCK (job_map);
}

typedef struct {
  GIODataFunc func;
  gpointer    data;
  GDestroyNotify notify;

  GMutex *ack_lock;
  GCond *ack_condition;
} MainLoopProxy;

static gboolean
mainloop_proxy_func (gpointer data)
{
  MainLoopProxy *proxy = data;

  proxy->func (proxy->data);

  if (proxy->ack_lock)
    {
      g_mutex_lock (proxy->ack_lock);
      g_cond_signal (proxy->ack_condition);
      g_mutex_unlock (proxy->ack_lock);
    }
  
  return FALSE;
}

static void
mainloop_proxy_free (MainLoopProxy *proxy)
{
  if (proxy->ack_lock)
    {
      g_mutex_free (proxy->ack_lock);
      g_cond_free (proxy->ack_condition);
    }
  
  g_free (proxy);
}

static void
mainloop_proxy_notify (gpointer data)
{
  MainLoopProxy *proxy = data;

  if (proxy->notify)
    proxy->notify (proxy->data);

  /* If nonblocking we free here, otherwise we free in io thread */
  if (proxy->ack_lock == NULL)
    mainloop_proxy_free (proxy);
}

void
g_io_job_send_to_mainloop (GIOJob        *job,
			   GIODataFunc    func,
			   gpointer       data,
			   GDestroyNotify notify,
			   gboolean       block)
{
  GSource *source;
  MainLoopProxy *proxy;
  guint id;

  proxy = g_new0 (MainLoopProxy, 1);
  proxy->func = func;
  proxy->data = data;
  proxy->notify = notify;
  if (block)
    {
      proxy->ack_lock = g_mutex_new ();
      proxy->ack_condition = g_cond_new ();
    }
  
  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);

  g_source_set_callback (source, mainloop_proxy_func, proxy, mainloop_proxy_notify);

  if (block)
    g_mutex_lock (proxy->ack_lock);
		  
  id = g_source_attach (source, job->callback_context);
  g_source_unref (source);

  if (block) {
	g_cond_wait (proxy->ack_condition, proxy->ack_lock);
	g_mutex_unlock (proxy->ack_lock);

	/* destroy notify didn't free proxy */
	mainloop_proxy_free (proxy);
  }
}

gboolean
g_io_job_is_cancelled (GIOJob *job)
{
  return job->cancelled;
}

/* Means you can't cancel it */
void
g_io_job_mark_done (GIOJob *job)
{
  G_LOCK (job_map);
  g_hash_table_remove (job_map, GINT_TO_POINTER (job->id));
  G_UNLOCK (job_map);
}
