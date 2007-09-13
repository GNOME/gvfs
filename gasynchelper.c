#include <config.h>

#include "gasynchelper.h"

static void
async_result_free (gpointer data)
{
  GAsyncResult *res = data;

  if (res->destroy_notify)
    res->destroy_notify (res->data);

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
		       GDestroyNotify  destroy_notify,
		       GMainContext   *context,
		       GSourceFunc     source_func)
{
  GSource *source;

  g_return_if_fail (G_IS_OBJECT (async_object));
  
  result->async_object = g_object_ref (async_object);
  result->error = error;
  result->destroy_notify = destroy_notify;

  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, source_func, result, async_result_free);
  g_source_attach (source, context);
  g_source_unref (source);
}
