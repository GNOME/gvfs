#include <config.h>
#include "gfileenumerator.h"
#include <glib/gi18n-lib.h>

G_DEFINE_TYPE (GFileEnumerator, g_file_enumerator, G_TYPE_OBJECT);

struct _GFileEnumeratorPrivate {
  /* TODO: Should be public for subclasses? */
  guint stopped : 1;
  guint pending : 1;
  guint cancelled : 1;
  GMainContext *context;
  gint io_job_id;
  gpointer outstanding_callback;
};

static void
g_file_enumerator_finalize (GObject *object)
{
  GFileEnumerator *enumerator;

  enumerator = G_FILE_ENUMERATOR (object);
  
  if (!enumerator->priv->stopped)
    g_file_enumerator_stop (enumerator, NULL);

  if (enumerator->priv->context)
    {
      g_main_context_unref (enumerator->priv->context);
      enumerator->priv->context = NULL;
    }
  
  if (G_OBJECT_CLASS (g_file_enumerator_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_enumerator_parent_class)->finalize) (object);
}

static void
g_file_enumerator_class_init (GFileEnumeratorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GFileEnumeratorPrivate));
  
  gobject_class->finalize = g_file_enumerator_finalize;
  
}

static void
g_file_enumerator_init (GFileEnumerator *enumerator)
{
  enumerator->priv = G_TYPE_INSTANCE_GET_PRIVATE (enumerator,
						  G_TYPE_FILE_ENUMERATOR,
						  GFileEnumeratorPrivate);
}

/**
 * g_file_enumerator_next_file:
 * @enumerator: a #GFileEnumerator.
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Returns information for the next file in the enumerated object.
 * Will block until the information is availible.
 *
 * On error, returns %NULL and sets @error to the error. If the
 * enumerator is at the end, %NULL will be returned and @error will
 * be unset.
 *
 * Return value: A GFileInfo or %NULL on error or end of enumerator
 **/
GFileInfo *
g_file_enumerator_next_file (GFileEnumerator *enumerator,
			     GError **error)
{
  GFileEnumeratorClass *class;
  GFileInfo *info;
  
  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), NULL);
  g_return_val_if_fail (enumerator != NULL, NULL);
  
  if (enumerator->priv->stopped)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Enumerator is stopped"));
      return NULL;
    }

  if (enumerator->priv->pending)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("File enumerator has outstanding operation"));
      return NULL;
    }
  
  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);

  enumerator->priv->pending = TRUE;
  info = (* class->next_file) (enumerator, error);
  enumerator->priv->pending = FALSE;
  
  return info;
}
  
/**
 * g_file_enumerator_stop:
 * @enumerator: a #GFileEnumerator.
 *
 * Releases all resources used by this enumerator, making the
 * enumerator return %G_VFS_ERROR_CLOSED on all calls.
 *
 * This will be automatically called when the last reference
 * is dropped, but you might want to call make sure resources
 * are released as early as possible.
 **/
gboolean
g_file_enumerator_stop (GFileEnumerator *enumerator,
			GError **error)
{
  GFileEnumeratorClass *class;

  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), FALSE);
  g_return_val_if_fail (enumerator != NULL, FALSE);
  
  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);

  if (enumerator->priv->stopped)
    return TRUE;
  
  if (enumerator->priv->pending)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("File enumerator has outstanding operation"));
      return FALSE;
    }
  
  enumerator->priv->pending = TRUE;
  (* class->stop) (enumerator, error);
  enumerator->priv->pending = FALSE;
  enumerator->priv->stopped = TRUE;

  return TRUE;
}

/**
 * g_file_enumerator_set_async_context:
 * @enumerator: A #GFileEnumerator.
 * @context: a #GMainContext (if %NULL, the default context will be used)
 *
 * Set the mainloop @context to be used for asynchronous i/o.
 * If not set, or if set to %NULL the default context will be used.
 **/
void
g_file_enumerator_set_async_context (GFileEnumerator *enumerator,
				     GMainContext *context)
{
  g_return_if_fail (G_IS_FILE_ENUMERATOR (enumerator));
  g_return_if_fail (enumerator != NULL);
  
  if (enumerator->priv->context)
    g_main_context_unref (enumerator->priv->context);
  
  enumerator->priv->context = context;
  
  if (context)
    g_main_context_ref (context);
}
  
/**
 * g_file_enumerator_get_async_context:
 * @enumerator: A #GFileEnumerator.
 *
 * Returns the mainloop used for async operation on this enumerator.
 * If you implement a enumerator you have to look at this to know what
 * context to use for async i/o.
 *
 * The context is set by the user by calling g_file_enumerator_set_async_context().
 *
 * Return value: A #GMainContext
 **/
GMainContext *
g_file_enumerator_get_async_context (GFileEnumerator *enumerator)
{
  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), NULL);
  g_return_val_if_fail (enumerator != NULL, NULL);

  if (enumerator->priv->context == NULL)
    {
      enumerator->priv->context = g_main_context_default ();
      g_main_context_ref (enumerator->priv->context);
    }
  
  return enumerator->priv->context;
}

typedef struct {
  GFileEnumerator        *enumerator;
  int                     num_files;
  GError                 *error;
  GAsyncNextFilesCallback callback;
  gpointer                data;
  GDestroyNotify          notify;
} NextAsyncResult;

static gboolean
call_next_async_result (gpointer data)
{
  NextAsyncResult *res = data;

  if (res->callback)
    res->callback (res->enumerator,
		   NULL,
		   res->num_files,
		   res->data,
		   res->error);

  return FALSE;
}

static void
next_async_result_free (gpointer data)
{
  NextAsyncResult *res = data;

  if (res->notify)
    res->notify (res->data);

  if (res->error)
    g_error_free (res->error);

  g_object_unref (res->enumerator);
  
  g_free (res);
}

static void
queue_next_async_result (GFileEnumerator *enumerator,
			 int num_files,
			 GError *error,
			 GAsyncNextFilesCallback callback,
			 gpointer data,
			 GDestroyNotify notify)
{
  GSource *source;
  NextAsyncResult *res;

  res = g_new0 (NextAsyncResult, 1);

  res->enumerator = g_object_ref (enumerator);
  res->num_files = num_files;
  res->error = error;
  res->callback = callback;
  res->data = data;
  res->notify = notify;
  
  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, call_next_async_result, res, next_async_result_free);
  g_source_attach (source, g_file_enumerator_get_async_context (enumerator));
  g_source_unref (source);
}

static void
next_async_callback_wrapper (GFileEnumerator *enumerator,
			     GFileInfo **files,
			     int num_files,
			     gpointer data,
			     GError *error)
{
  GAsyncNextFilesCallback real_callback = enumerator->priv->outstanding_callback;

  enumerator->priv->pending = FALSE;
  (*real_callback) (enumerator, files, num_files, data, error);
  g_object_unref (enumerator);
}

/**
 * g_file_enumerator_next_files_async:
 * @enumerator: a #GFileEnumerator.
 * @num_file: the number of file info objects to request
 * @io_priority: the io priority of the request
 * @callback: callback to call when the request is satisfied
 * @data: the data to pass to callback function
 * @notify: a function to call when @data is no longer in use, or %NULL.
 *
 * Request information for a number of files from the enumerator asynchronously.
 * When all i/o for the operation is finished the @callback will be called with
 * the requested information.
 *
 * The callback can be called with less than @num_files files in case of error
 * or at the end of the enumerator. In case of a partial error the callback will
 * be called with any succeeding items and no error, and on the next request the
 * error will be reported. If a request is cancelled the callback will be called
 * with %G_VFS_ERROR_CANCELLED.
 *
 * During an async request no other sync and async calls are allowed, and will
 * result in %G_VFS_ERROR_PENDING errors. 
 *
 * Any outstanding i/o request with higher priority (lower numerical value) will
 * be executed before an outstanding request with lower priority. Default
 * priority is %G_PRIORITY_DEFAULT.
 **/
void
g_file_enumerator_next_files_async (GFileEnumerator        *enumerator,
				    int                     num_files,
				    int                     io_priority,
				    GAsyncNextFilesCallback callback,
				    gpointer                data,
				    GDestroyNotify          notify)
{
  GFileEnumeratorClass *class;
  GError *error;

  g_return_if_fail (G_IS_FILE_ENUMERATOR (enumerator));
  g_return_if_fail (enumerator != NULL);
  g_return_if_fail (num_files < 0);

  enumerator->priv->cancelled = FALSE;

  if (num_files == 0)
    {
      queue_next_async_result (enumerator, 0, NULL,  callback, data, notify);
      return;
    }
  
  if (enumerator->priv->stopped)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("File enumerator is already closed"));
      queue_next_async_result (enumerator, -1, error,
			       callback, data, notify);
      return;
    }
  
  if (enumerator->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("File enumerator has outstanding operation"));
      queue_next_async_result (enumerator, -1, error,
			       callback, data, notify);
      return;
    }

  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);
  
  enumerator->priv->pending = TRUE;
  enumerator->priv->outstanding_callback = callback;
  g_object_ref (enumerator);
  (* class->next_files_async) (enumerator, num_files, io_priority, 
			       next_async_callback_wrapper, data, notify);
}


typedef struct {
  GFileEnumerator *             enumerator;
  gboolean                      result;
  GError *                      error;
  GAsyncStopEnumeratingCallback callback;
  gpointer                      data;
  GDestroyNotify                notify;
} StopAsyncResult;

static gboolean
call_stop_async_result (gpointer data)
{
  StopAsyncResult *res = data;

  if (res->callback)
    res->callback (res->enumerator,
		   res->result,
		   res->data,
		   res->error);

  return FALSE;
}

static void
stop_async_result_free (gpointer data)
{
  StopAsyncResult *res = data;

  if (res->notify)
    res->notify (res->data);

  if (res->error)
    g_error_free (res->error);

  g_object_unref (res->enumerator);
  
  g_free (res);
}

static void
queue_stop_async_result (GFileEnumerator *enumerator,
			 gboolean result,
			 GError *error,
			 GAsyncStopEnumeratingCallback callback,
			 gpointer data,
			 GDestroyNotify notify)
{
  GSource *source;
  StopAsyncResult *res;

  res = g_new0 (StopAsyncResult, 1);

  res->enumerator = g_object_ref (enumerator);
  res->result = result;
  res->error = error;
  res->callback = callback;
  res->data = data;
  res->notify = notify;
  
  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, call_stop_async_result, res, stop_async_result_free);
  g_source_attach (source, g_file_enumerator_get_async_context (enumerator));
  g_source_unref (source);
}

static void
stop_async_callback_wrapper (GFileEnumerator *enumerator,
			     gboolean result,
			     gpointer data,
			     GError *error)
{
  GAsyncStopEnumeratingCallback real_callback = enumerator->priv->outstanding_callback;

  enumerator->priv->pending = FALSE;
  enumerator->priv->stopped = TRUE;
  (*real_callback) (enumerator, result, data, error);
  g_object_unref (enumerator);
}

void
g_file_enumerator_stop_async (GFileEnumerator                *enumerator,
			      int                             io_priority,
			      GAsyncStopEnumeratingCallback   callback,
			      gpointer                        data,
			      GDestroyNotify                  notify)
{
  GFileEnumeratorClass *class;
  GError *error;

  g_return_if_fail (G_IS_FILE_ENUMERATOR (enumerator));

  enumerator->priv->cancelled = FALSE;

  if (enumerator->priv->stopped)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("File enumerator is already stopped"));
      queue_stop_async_result (enumerator, FALSE, error,
			       callback, data, notify);
      return;
    }
  
  if (enumerator->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("File enumerator has outstanding operation"));
      queue_stop_async_result (enumerator, FALSE, error,
			       callback, data, notify);
      return;
    }

  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);
  
  enumerator->priv->pending = TRUE;
  enumerator->priv->outstanding_callback = callback;
  g_object_ref (enumerator);
  (* class->stop_async) (enumerator, io_priority,
			 stop_async_callback_wrapper, data, notify);
}


/**
 * g_file_enumerator_cancel:
 * @enumerator: a #GFileEnumerator.
 * @tag: a value returned from an async request
 *
 * Tries to cancel an outstanding request for file information. If it
 * succeeds the outstanding request callback will be called with
 * %G_VFS_ERROR_CANCELLED.
 *
 * Generally if a request is cancelled before its callback has been
 * called the cancellation will succeed and the callback will only
 * be called with %G_VFS_ERROR_CANCELLED. However, this cannot be guaranteed,
 * especially if multiple threads are in use, so you might get a succeeding
 * callback and no %G_VFS_ERROR_CANCELLED callback even if you call cancel.
 **/
void
g_file_enumerator_cancel (GFileEnumerator  *enumerator)
{
  GFileEnumeratorClass *class;

  g_return_if_fail (G_IS_FILE_ENUMERATOR (enumerator));
  g_return_if_fail (enumerator != NULL);

  enumerator->priv->cancelled = TRUE;
  
  class = G_FILE_ENUMERATOR_GET_CLASS (enumerator);
  (* class->cancel) (enumerator);
}

gboolean
g_file_enumerator_is_cancelled (GFileEnumerator *enumerator)
{
  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), TRUE);
  g_return_val_if_fail (enumerator != NULL, TRUE);
  
  return enumerator->priv->cancelled;
}

gboolean
g_file_enumerator_is_stopped (GFileEnumerator *enumerator)
{
  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), TRUE);
  g_return_val_if_fail (enumerator != NULL, TRUE);
  
  return enumerator->priv->stopped;
}
  
gboolean
g_file_enumerator_has_pending (GFileEnumerator *enumerator)
{
  g_return_val_if_fail (G_IS_FILE_ENUMERATOR (enumerator), TRUE);
  g_return_val_if_fail (enumerator != NULL, TRUE);
  
  return enumerator->priv->pending;
}

void
g_file_enumerator_set_pending (GFileEnumerator              *enumerator,
			       gboolean                   pending)
{
  g_return_if_fail (G_IS_FILE_ENUMERATOR (enumerator));
  g_return_if_fail (enumerator != NULL);
  
  enumerator->priv->pending = pending;
}
