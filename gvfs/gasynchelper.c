#include <config.h>

#include "gasynchelper.h"

static void
async_result_free (gpointer data)
{
  GAsyncResult *res = data;

  if (res->error)
    g_error_free (res->error);

  g_object_unref (res->async_object);
  
  g_free (res);
}

void
_g_queue_async_result (GAsyncResult   *result,
		       gpointer        async_object,
		       GError         *error,
		       gpointer        data,
		       GMainContext   *context,
		       GSourceFunc     source_func)
{
  GSource *source;

  g_return_if_fail (G_IS_OBJECT (async_object));
  
  result->async_object = g_object_ref (async_object);
  result->data = data;
  result->error = error;

  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, source_func, result, async_result_free);
  g_source_attach (source, context);
  g_source_unref (source);
}

typedef struct 
{
  GSource source;
  GPollFD pollfd;
  GObject *object;
  GCancellable *cancellable;
  gulong cancelled_tag;
} FDSource;

static gboolean 
fd_source_prepare (GSource  *source,
		   gint     *timeout)
{
  FDSource *fd_source = (FDSource *)source;
  *timeout = -1;
  
  return g_cancellable_is_cancelled (fd_source->cancellable);
}

static gboolean 
fd_source_check (GSource  *source)
{
  FDSource *fd_source = (FDSource *)source;

  return
    g_cancellable_is_cancelled  (fd_source->cancellable) ||
    fd_source->pollfd.revents != 0;
}

static gboolean
fd_source_dispatch (GSource     *source,
		    GSourceFunc  callback,
		    gpointer     user_data)

{
  GFDSourceFunc func = (GFDSourceFunc)callback;
  FDSource *fd_source = (FDSource *)source;

  g_assert (func != NULL);

  (*func) (user_data, fd_source->object, fd_source->pollfd.fd);
  return FALSE;
}

static void 
fd_source_finalize (GSource *source)
{
  FDSource *fd_source = (FDSource *)source;

  if (fd_source->cancelled_tag)
    g_signal_handler_disconnect (fd_source->cancellable,
				 fd_source->cancelled_tag);

  if (fd_source->cancellable)
    g_object_unref (fd_source->cancellable);
  g_object_unref (fd_source->object);
}

static GSourceFuncs fd_source_funcs = {
  fd_source_prepare,
  fd_source_check,
  fd_source_dispatch,
  fd_source_finalize
};

/* Might be called on another thread */
static void
fd_source_cancelled_cb (GCancellable *cancellable,
			gpointer data)
{
  GMainContext *context = data;
  
  /* Wake up the mainloop in case we're waiting on async calls with FDSource */
  g_main_context_wakeup (context);
}

static void
cancel_closure_notify (gpointer	 data,
		       GClosure	*closure)
{
  GMainContext *context = data;
  g_main_context_unref (context);
}

GSource *
_g_fd_source_new (GObject *object,
		  int fd,
		  gushort events,
		  GMainContext *context,
		  GCancellable *cancellable)
{
  GSource *source;
  FDSource *fd_source;

  source = g_source_new (&fd_source_funcs, sizeof (FDSource));
  fd_source = (FDSource *)source;

  fd_source->object = g_object_ref (object);
  if (cancellable)
    fd_source->cancellable = g_object_ref (cancellable);
  
  fd_source->pollfd.fd = fd;
  fd_source->pollfd.events = events;
  g_source_add_poll (source, &fd_source->pollfd);

  g_source_attach (source, context);
 
  if (cancellable)
    fd_source->cancelled_tag =
      g_signal_connect_data (cancellable, "cancelled",
			     (GCallback)fd_source_cancelled_cb,
			     g_main_context_ref (context),
			     cancel_closure_notify,
			     0);
  
  return source;
}
