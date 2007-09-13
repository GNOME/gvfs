#include <config.h>
#include <glib/gi18n-lib.h>
#include <glib.h>

#include "ginputstream.h"
#include "gioscheduler.h"

G_DEFINE_TYPE (GInputStream, g_input_stream, G_TYPE_OBJECT);

static GObjectClass *parent_class = NULL;

struct _GInputStreamPrivate {
  guint closed : 1;
  guint pending : 1;
  guint cancelled : 1;
  GMainContext *context;
  gint io_job_id;
  gpointer outstanding_callback;
};

static gssize g_input_stream_real_skip        (GInputStream         *stream,
					       gsize                 count,
					       GError              **error);
static void   g_input_stream_real_read_async  (GInputStream         *stream,
					       void                 *buffer,
					       gsize                 count,
					       int                   io_priority,
					       GAsyncReadCallback    callback,
					       gpointer              data,
					       GDestroyNotify        notify);
static void   g_input_stream_real_skip_async  (GInputStream         *stream,
					       gsize                 count,
					       int                   io_priority,
					       GAsyncSkipCallback    callback,
					       gpointer              data,
					       GDestroyNotify        notify);
static void   g_input_stream_real_close_async (GInputStream         *stream,
					       int                   io_priority,
					       GAsyncCloseInputCallback   callback,
					       gpointer              data,
					       GDestroyNotify        notify);
static void   g_input_stream_real_cancel      (GInputStream         *stream);


static void
g_input_stream_finalize (GObject *object)
{
  GInputStream *stream;

  stream = G_INPUT_STREAM (object);
  
  if (!stream->priv->closed)
    g_input_stream_close (stream, NULL);

  if (stream->priv->context)
    {
      g_main_context_unref (stream->priv->context);
      stream->priv->context = NULL;
    }
  
  if (G_OBJECT_CLASS (parent_class)->finalize)
    (*G_OBJECT_CLASS (parent_class)->finalize) (object);
}

static void
g_input_stream_class_init (GInputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  parent_class = g_type_class_peek_parent (klass);
  
  g_type_class_add_private (klass, sizeof (GInputStreamPrivate));
  
  gobject_class->finalize = g_input_stream_finalize;
  
  klass->skip = g_input_stream_real_skip;
  klass->read_async = g_input_stream_real_read_async;
  klass->skip_async = g_input_stream_real_skip_async;
  klass->close_async = g_input_stream_real_close_async;
  klass->cancel = g_input_stream_real_cancel;
}

static void
g_input_stream_init (GInputStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
					      G_TYPE_INPUT_STREAM,
					      GInputStreamPrivate);
}

/**
 * g_input_stream_read:
 * @stream: a #GInputStream.
 * @buffer: a buffer to read data into (which should be at least count bytes long).
 * @count: the number of bytes that will be read from the stream
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Tries to read @count bytes from the stream into the buffer starting at
 * @buffer. Will block during this read.
 * 
 * If count is zero returns zero and does nothing. A value of @count
 * larger than %G_MAXSSIZE will cause a %G_VFS_ERROR_INVALID_ARGUMENT error.
 *
 * On success, the number of bytes read into the buffer is returned.
 * It is not an error if this is not the same as the requested size, as it
 * can happen e.g. near the end of a file, but generally we try to read
 * as many bytes as requested. Zero is returned on end of file
 * (or if @count is zero),  but never otherwise.
 * 
 * On error -1 is returned and @error is set accordingly.
 * 
 * Return value: Number of bytes read, or -1 on error
 **/
gssize
g_input_stream_read  (GInputStream  *stream,
		      void          *buffer,
		      gsize          count,
		      GError       **error)
{
  GInputStreamClass *class;
  gssize res;

  g_return_val_if_fail (G_IS_INPUT_STREAM (stream), -1);
  g_return_val_if_fail (stream != NULL, -1);
  g_return_val_if_fail (buffer != NULL, 0);

  if (count == 0)
    return 0;
  
  if (((gssize) count) < 0)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_INVALID_ARGUMENT,
		   _("Too large count value passed to g_input_stream_read"));
      return -1;
    }

  if (stream->priv->closed)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Stream is already closed"));
      return -1;
    }
  
  if (stream->priv->pending)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return -1;
    }
  
  class = G_INPUT_STREAM_GET_CLASS (stream);

  if (class->read == NULL) 
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_INTERNAL_ERROR,
		   _("Input stream doesn't implement read"));
      return -1;
    }

  stream->priv->pending = TRUE;
  res = class->read (stream, buffer, count, error);
  stream->priv->pending = FALSE;
  return res;
}

/**
 * g_input_stream_skip:
 * @stream: a #GInputStream.
 * @count: the number of bytes that will be skipped from the stream
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Tries to skip @count bytes from the stream. Will block during the operation.
 *
 * This is identical to g_input_stream_read(), from a behaviour standpoint,
 * but the bytes that are skipped are not returned to the user. Some
 * streams have an implementation that is more efficient than reading the data.
 *
 * This function is optional for inherited classes.
 *
 * Return value: Number of bytes skipped, or -1 on error
 **/
gssize
g_input_stream_skip (GInputStream         *stream,
		     gsize                 count,
		     GError              **error)
{
  GInputStreamClass *class;
  gssize res;

  g_return_val_if_fail (G_IS_INPUT_STREAM (stream), -1);
  g_return_val_if_fail (stream != NULL, -1);

  if (count == 0)
    return 0;

  if (((gssize) count) < 0)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_INVALID_ARGUMENT,
		   _("Too large count value passed to g_input_stream_skip"));
      return -1;
    }
  
  if (stream->priv->closed)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Stream is already closed"));
      return -1;
    }

  if (stream->priv->pending)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return -1;
    }
  
  class = G_INPUT_STREAM_GET_CLASS (stream);

  stream->priv->pending = TRUE;
  res = class->skip (stream, count, error);
  stream->priv->pending = FALSE;
  return res;
}

static gssize
g_input_stream_real_skip (GInputStream         *stream,
			  gsize                 count,
			  GError              **error)
{
  gssize ret;
  char *buffer;

  buffer = g_malloc (count);
  ret = g_input_stream_read (stream, buffer, count, error);
  g_free (buffer);

  return ret;
}

/**
 * g_input_stream_close:
 * @stream: A #GInputStream.
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Closes the stream, releasing resources related to it.
 *
 * Once the stream is closed, all other operations will return %G_VFS_ERROR_CLOSED.
 * Closing a stream multiple times will not return an error.
 *
 * Streams will be automatically closed when the last reference
 * is dropped, but you might want to call make sure resources
 * are released as early as possible.
 *
 * Some streams might keep the backing store of the stream (e.g. a file descriptor)
 * open after the stream is closed. See the documentation for the individual
 * stream for details.
 *
 * On failure the first error that happened will be reported, but the close
 * operation will finish as much as possible. A stream that failed to
 * close will still return %G_VFS_ERROR_CLOSED all operations.
 * 
 * Return value: %TRUE on success, %FALSE on failure
 **/
gboolean
g_input_stream_close (GInputStream  *stream,
		      GError       **error)
{
  GInputStreamClass *class;
  gboolean res;

  g_return_val_if_fail (G_IS_INPUT_STREAM (stream), -1);
  g_return_val_if_fail (stream != NULL, -1);

  class = G_INPUT_STREAM_GET_CLASS (stream);

  if (stream->priv->closed)
    return TRUE;

  if (stream->priv->pending)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return FALSE;
    }
  
  res = TRUE;

  stream->priv->pending = TRUE;
  
  if (class->close)
    res = class->close (stream, error);
  
  stream->priv->closed = TRUE;
  
  stream->priv->pending = FALSE;

  return res;
}

/**
 * g_input_stream_set_async_context:
 * @stream: A #GInputStream.
 * @context: a #GMainContext (if %NULL, the default context will be used)
 *
 * Set the mainloop @context to be used for asynchronous i/o.
 * If not set, or if set to %NULL the default context will be used.
 **/
void
g_input_stream_set_async_context (GInputStream *stream,
				  GMainContext *context)
{
  g_return_if_fail (G_IS_INPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  if (stream->priv->context)
    g_main_context_unref (stream->priv->context);
  
  stream->priv->context = context;
  
  if (context)
    g_main_context_ref (context);
}
  
/**
 * g_input_stream_get_async_context:
 * @stream: A #GInputStream.
 *
 * Returns the mainloop used for async operation on this stream.
 * If you implement a stream you have to look at this to know what
 * context to use for async i/o.
 *
 * The context is set by the user by calling g_input_stream_set_async_context().
 *
 * Return value: A #GMainContext
 **/
GMainContext *
g_input_stream_get_async_context (GInputStream *stream)
{
  g_return_val_if_fail (G_IS_INPUT_STREAM (stream), NULL);
  g_return_val_if_fail (stream != NULL, NULL);

  if (stream->priv->context == NULL)
    {
      stream->priv->context = g_main_context_default ();
      g_main_context_ref (stream->priv->context);
    }
      
  return stream->priv->context;
}

typedef struct {
  GInputStream      *stream;
  void              *buffer;
  gsize              count_requested;
  gssize             count_read;
  GError            *error;
  GAsyncReadCallback callback;
  gpointer           data;
  GDestroyNotify     notify;
} ReadAsyncResult;

static gboolean
call_read_async_result (gpointer data)
{
  ReadAsyncResult *res = data;

  if (res->callback)
    res->callback (res->stream,
		   res->buffer,
		   res->count_requested,
		   res->count_read,
		   res->data,
		   res->error);

  return FALSE;
}

static void
read_async_result_free (gpointer data)
{
  ReadAsyncResult *res = data;

  if (res->notify)
    res->notify (res->data);

  if (res->error)
    g_error_free (res->error);

  g_object_unref (res->stream);
  
  g_free (res);
}

static void
queue_read_async_result (GInputStream      *stream,
			 void              *buffer,
			 gsize              count_requested,
			 gssize             count_read,
			 GError            *error,
			 GAsyncReadCallback callback,
			 gpointer           data,
			 GDestroyNotify     notify)
{
  GSource *source;
  ReadAsyncResult *res;

  res = g_new0 (ReadAsyncResult, 1);

  res->stream = g_object_ref (stream);
  res->buffer = buffer;
  res->count_requested = count_requested;
  res->count_read = count_read;
  res->error = error;
  res->callback = callback;
  res->data = data;
  res->notify = notify;
  
  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, call_read_async_result, res, read_async_result_free);
  g_source_attach (source, g_input_stream_get_async_context (stream));
  g_source_unref (source);
}

static void
read_async_callback_wrapper (GInputStream *stream,
			     void         *buffer,
			     gsize         count_requested,
			     gssize        count_read,
			     gpointer      data,
			     GError       *error)
{
  GAsyncReadCallback real_callback = stream->priv->outstanding_callback;

  stream->priv->pending = FALSE;
  (*real_callback) (stream, buffer, count_requested, count_read, data, error);
  g_object_unref (stream);
}

/**
 * g_input_stream_read_async:
 * @stream: A #GInputStream.
 * @buffer: a buffer to read data into (which should be at least count bytes long).
 * @count: the number of bytes that will be read from the stream
 * @io_priority: the io priority of the request
 * @callback: callback to call when the request is satisfied
 * @data: the data to pass to callback function
 * @notify: a function to call when @data is no longer in use, or %NULL.
 *
 * Request an asynchronous read of @count bytes from the stream into the buffer
 * starting at @buffer. When the operation is finished @callback will be called,
 * giving the results.
 *
 * During an async request no other sync and async calls are allowed, and will
 * result in %G_VFS_ERROR_PENDING errors. 
 *
 * A value of @count larger than %G_MAXSSIZE will cause a %G_VFS_ERROR_INVALID_ARGUMENT error.
 *
 * On success, the number of bytes read into the buffer will be passed to the
 * callback. It is not an error if this is not the same as the requested size, as it
 * can happen e.g. near the end of a file, but generally we try to read
 * as many bytes as requested. Zero is returned on end of file
 * (or if @count is zero),  but never otherwise.
 *
 * Any outstanding i/o request with higher priority (lower numerical value) will
 * be executed before an outstanding request with lower priority. Default
 * priority is %G_PRIORITY_DEFAULT.
 *
 * The asyncronous methods have a default fallback that uses threads to implement
 * asynchronicity, so they are optional for inheriting classes. However, if you
 * override one you must override all.
 **/
void
g_input_stream_read_async (GInputStream        *stream,
			   void                *buffer,
			   gsize                count,
			   int                  io_priority,
			   GAsyncReadCallback   callback,
			   gpointer             data,
			   GDestroyNotify       notify)
{
  GInputStreamClass *class;
  GError *error;

  g_return_if_fail (G_IS_INPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);
  g_return_if_fail (buffer != NULL);

  stream->priv->cancelled = FALSE;

  if (count == 0)
    {
      queue_read_async_result (stream, buffer, count, 0, NULL,
			       callback, data, notify);
      return;
    }
  
  if (((gssize) count) < 0)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_INVALID_ARGUMENT,
		   _("Too large count value passed to g_input_stream_read_async"));
      queue_read_async_result (stream, buffer, count, -1, error,
			       callback, data, notify);
      return;
    }

  if (stream->priv->closed)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Stream is already closed"));
      queue_read_async_result (stream, buffer, count, -1, error,
			       callback, data, notify);
      return;
    }
  
  if (stream->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      queue_read_async_result (stream, buffer, count, -1, error,
			       callback, data, notify);
      return;
    }


  class = G_INPUT_STREAM_GET_CLASS (stream);
  
  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->read_async (stream, buffer, count, io_priority, read_async_callback_wrapper, data, notify);
}

typedef struct {
  GInputStream      *stream;
  gsize              count_requested;
  gssize             count_skipped;
  GError            *error;
  GAsyncSkipCallback callback;
  gpointer           data;
  GDestroyNotify     notify;
} SkipAsyncResult;

static gboolean
call_skip_async_result (gpointer data)
{
  SkipAsyncResult *res = data;

  if (res->callback)
    res->callback (res->stream,
		   res->count_requested,
		   res->count_skipped,
		   res->data,
		   res->error);

  return FALSE;
}

static void
skip_async_result_free (gpointer data)
{
  SkipAsyncResult *res = data;

  if (res->notify)
    res->notify (res->data);

  if (res->error)
    g_error_free (res->error);

  g_object_unref (res->stream);
  
  g_free (res);
}

static void
queue_skip_async_result (GInputStream      *stream,
			 gsize              count_requested,
			 gssize             count_skipped,
			 GError            *error,
			 GAsyncSkipCallback callback,
			 gpointer           data,
			 GDestroyNotify     notify)
{
  GSource *source;
  SkipAsyncResult *res;

  res = g_new0 (SkipAsyncResult, 1);

  res->stream = g_object_ref (stream);
  res->count_requested = count_requested;
  res->count_skipped = count_skipped;
  res->error = error;
  res->callback = callback;
  res->data = data;
  res->notify = notify;
  
  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, call_skip_async_result, res, skip_async_result_free);
  g_source_attach (source, g_input_stream_get_async_context (stream));
  g_source_unref (source);
}

static void
skip_async_callback_wrapper (GInputStream *stream,
			     gsize         count_requested,
			     gssize        count_skipped,
			     gpointer      data,
			     GError       *error)
{
  GAsyncSkipCallback real_callback = stream->priv->outstanding_callback;

  stream->priv->pending = FALSE;
  (*real_callback) (stream, count_requested, count_skipped, data, error);
  g_object_unref (stream);
}

/**
 * g_input_stream_skip_async:
 * @stream: A #GInputStream.
 * @count: the number of bytes that will be skipped from the stream
 * @io_priority: the io priority of the request
 * @callback: callback to call when the request is satisfied
 * @data: the data to pass to callback function
 * @notify: a function to call when @data is no longer in use, or %NULL.
 *
 * Request an asynchronous skip of @count bytes from the stream into the buffer
 * starting at @buffer. When the operation is finished @callback will be called,
 * giving the results.
 *
 * During an async request no other sync and async calls are allowed, and will
 * result in %G_VFS_ERROR_PENDING errors. 
 *
 * A value of @count larger than %G_MAXSSIZE will cause a %G_VFS_ERROR_INVALID_ARGUMENT error.
 *
 * On success, the number of bytes skipped will be passed to the
 * callback. It is not an error if this is not the same as the requested size, as it
 * can happen e.g. near the end of a file, but generally we try to skip
 * as many bytes as requested. Zero is returned on end of file
 * (or if @count is zero), but never otherwise.
 *
 * Any outstanding i/o request with higher priority (lower numerical value) will
 * be executed before an outstanding request with lower priority. Default
 * priority is %G_PRIORITY_DEFAULT.
 *
 * The asyncronous methods have a default fallback that uses threads to implement
 * asynchronicity, so they are optional for inheriting classes. However, if you
 * override one you must override all.
 **/
void
g_input_stream_skip_async (GInputStream        *stream,
			   gsize                count,
			   int                  io_priority,
			   GAsyncSkipCallback   callback,
			   gpointer             data,
			   GDestroyNotify       notify)
{
  GInputStreamClass *class;
  GError *error;

  g_return_if_fail (G_IS_INPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  stream->priv->cancelled = FALSE;
  
  if (count == 0)
    {
      queue_skip_async_result (stream, count, 0, NULL,
			       callback, data, notify);
      return;
    }
  
  if (((gssize) count) < 0)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_INVALID_ARGUMENT,
		   _("Too large count value passed to g_input_stream_skip_async"));
      queue_skip_async_result (stream, count, -1, error,
			       callback, data, notify);
      return;
    }

  if (stream->priv->closed)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Stream is already closed"));
      queue_skip_async_result (stream, count, -1, error,
			       callback, data, notify);
      return;
    }
  
  if (stream->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      queue_skip_async_result (stream, count, -1, error,
			       callback, data, notify);
      return;
    }

  class = G_INPUT_STREAM_GET_CLASS (stream);
  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->skip_async (stream, count, io_priority, skip_async_callback_wrapper, data, notify);
}


typedef struct {
  GInputStream       *stream;
  gboolean            result;
  GError             *error;
  GAsyncCloseInputCallback callback;
  gpointer            data;
  GDestroyNotify      notify;
} CloseAsyncResult;

static gboolean
call_close_async_result (gpointer data)
{
  CloseAsyncResult *res = data;

  if (res->callback)
    res->callback (res->stream,
		   res->result,
		   res->data,
		   res->error);

  return FALSE;
}

static void
close_async_result_free (gpointer data)
{
  CloseAsyncResult *res = data;

  if (res->notify)
    res->notify (res->data);

  if (res->error)
    g_error_free (res->error);
  
  g_object_unref (res->stream);
  
  g_free (res);
}

static void
queue_close_async_result (GInputStream       *stream,
			  gboolean            result,
			  GError             *error,
			  GAsyncCloseInputCallback callback,
			  gpointer            data,
			  GDestroyNotify      notify)
{
  GSource *source;
  CloseAsyncResult *res;

  res = g_new0 (CloseAsyncResult, 1);

  res->stream = g_object_ref (stream);
  res->result = result;
  res->error = error;
  res->callback = callback;
  res->data = data;
  res->notify = notify;
  
  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, call_close_async_result, res, close_async_result_free);
  g_source_attach (source, g_input_stream_get_async_context (stream));
  g_source_unref (source);
}

static void
close_async_callback_wrapper (GInputStream *stream,
			      gboolean      result,
			      gpointer      data,
			      GError       *error)
{
  GAsyncCloseInputCallback real_callback = stream->priv->outstanding_callback;

  stream->priv->pending = FALSE;
  stream->priv->closed = TRUE;
  (*real_callback) (stream, result, data, error);
  g_object_unref (stream);
}

/**
 * g_input_stream_close_async:
 * @stream: A #GInputStream.
 * @io_priority: the io priority of the request
 * @callback: callback to call when the request is satisfied
 * @data: the data to pass to callback function
 * @notify: a function to call when @data is no longer in use, or %NULL.
 *
 * Requests an asynchronous closes of the stream, releasing resources related to it.
 * When the operation is finished @callback will be called, giving the results.
 *
 * For behaviour details see g_input_stream_close().
 *
 * The asyncronous methods have a default fallback that uses threads to implement
 * asynchronicity, so they are optional for inheriting classes. However, if you
 * override one you must override all.
 **/
void
g_input_stream_close_async (GInputStream       *stream,
			    int                 io_priority,
			    GAsyncCloseInputCallback callback,
			    gpointer            data,
			    GDestroyNotify      notify)
{
  GInputStreamClass *class;
  GError *error;

  g_return_if_fail (G_IS_INPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  stream->priv->cancelled = FALSE;
  
  if (stream->priv->closed)
    {
      queue_close_async_result (stream, TRUE, NULL,
				callback, data, notify);
      return;
    }

  if (stream->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      queue_close_async_result (stream, FALSE, error,
				callback, data, notify);
      return;
    }
  
  class = G_INPUT_STREAM_GET_CLASS (stream);
  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->close_async (stream, io_priority, close_async_callback_wrapper, data, notify);
}

/**
 * g_input_stream_cancel:
 * @stream: A #GInputStream.
 *
 * Tries to cancel an outstanding request for the stream. If it
 * succeeds the outstanding request callback will be called with
 * %G_VFS_ERROR_CANCELLED.
 *
 * Generally if a request is cancelled before its callback has been
 * called the cancellation will succeed and the callback will only
 * be called with %G_VFS_ERROR_CANCELLED. However, this cannot be guaranteed,
 * especially if multiple threads are in use, so you might get a succeeding
 * callback and no %G_VFS_ERROR_CANCELLED callback even if you call cancel.
 *
 * The asyncronous methods have a default fallback that uses threads to implement
 * asynchronicity, so they are optional for inheriting classes. However, if you
 * override one you must override all.
 **/
void
g_input_stream_cancel (GInputStream *stream)
{
  GInputStreamClass *class;

  g_return_if_fail (G_IS_INPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);
  
  class = G_INPUT_STREAM_GET_CLASS (stream);

  stream->priv->cancelled = TRUE;
  
  class->cancel (stream);
}

gboolean
g_input_stream_is_cancelled (GInputStream *stream)
{
  g_return_val_if_fail (G_IS_INPUT_STREAM (stream), TRUE);
  g_return_val_if_fail (stream != NULL, TRUE);
  
  return stream->priv->cancelled;
}

gboolean
g_input_stream_is_closed (GInputStream *stream)
{
  g_return_val_if_fail (G_IS_INPUT_STREAM (stream), TRUE);
  g_return_val_if_fail (stream != NULL, TRUE);
  
  return stream->priv->closed;
}
  
gboolean
g_input_stream_has_pending (GInputStream *stream)
{
  g_return_val_if_fail (G_IS_INPUT_STREAM (stream), TRUE);
  g_return_val_if_fail (stream != NULL, TRUE);
  
  return stream->priv->pending;
}

void
g_input_stream_set_pending (GInputStream              *stream,
			    gboolean                   pending)
{
  g_return_if_fail (G_IS_INPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);
  
  stream->priv->pending = pending;
}

/********************************************
 *   Default implementation of async ops    *
 ********************************************/


typedef struct {
  GInputStream      *stream;
  void              *buffer;
  gsize              count_requested;
  gssize             count_read;
  GError            *error;
  GAsyncReadCallback callback;
  gpointer           data;
  GDestroyNotify     notify;
} ReadAsyncOp;

static void
read_op_report (gpointer data)
{
  ReadAsyncOp *op = data;

  op->callback (op->stream,
		op->buffer,
		op->count_requested,
		op->count_read,
		op->data,
		op->error);

}

static void
read_op_free (gpointer data)
{
  ReadAsyncOp *op = data;

  if (op->error)
    g_error_free (op->error);

  if (op->notify)
    op->notify (op->data);

  g_free (op);
}


static void
read_op_func (GIOJob *job,
	      gpointer data)
{
  ReadAsyncOp *op = data;
  GInputStreamClass *class;

  if (g_io_job_is_cancelled (job))
    {
      op->count_read = -1;
      g_set_error (&op->error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
    }
  else
    {
      class = G_INPUT_STREAM_GET_CLASS (op->stream);
      op->count_read = class->read (op->stream, op->buffer, op->count_requested, &op->error);
    }

  g_io_job_mark_done (job);
  g_io_job_send_to_mainloop (job, read_op_report,
			     op, read_op_free,
			     FALSE);
}

static void
read_op_cancel (gpointer data)
{
  ReadAsyncOp *op = data;
  GInputStreamClass *class;

  class = G_INPUT_STREAM_GET_CLASS (op->stream);
  if (class->cancel_sync)
    class->cancel_sync (op->stream);
}

static void
g_input_stream_real_read_async (GInputStream        *stream,
				void                *buffer,
				gsize                count,
				int                  io_priority,
				GAsyncReadCallback   callback,
				gpointer             data,
				GDestroyNotify       notify)
{
  ReadAsyncOp *op;

  op = g_new0 (ReadAsyncOp, 1);

  op->stream = stream;
  op->buffer = buffer;
  op->count_requested = count;
  op->callback = callback;
  op->data = data;
  op->notify = notify;
  
  stream->priv->io_job_id = g_schedule_io_job (read_op_func,
					       read_op_cancel,
					       op,
					       NULL,
					       io_priority,
					       g_input_stream_get_async_context (stream));
}

typedef struct {
  GInputStream      *stream;
  gsize              count_requested;
  gssize             count_skipped;
  GError            *error;
  GAsyncSkipCallback callback;
  gpointer           data;
  GDestroyNotify     notify;
} SkipAsyncOp;

static void
skip_op_report (gpointer data)
{
  SkipAsyncOp *op = data;

  op->callback (op->stream,
		op->count_requested,
		op->count_skipped,
		op->data,
		op->error);

}

static void
skip_op_free (gpointer data)
{
  SkipAsyncOp *op = data;

  if (op->error)
    g_error_free (op->error);

  if (op->notify)
    op->notify (op->data);

  g_free (op);
}


static void
skip_op_func (GIOJob *job,
	      gpointer data)
{
  SkipAsyncOp *op = data;
  GInputStreamClass *class;

  if (g_io_job_is_cancelled (job))
    {
      op->count_skipped = -1;
      g_set_error (&op->error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
    }
  else
    {
      class = G_INPUT_STREAM_GET_CLASS (op->stream);
      op->count_skipped = class->skip (op->stream, op->count_requested, &op->error);
    }

  g_io_job_mark_done (job);
  g_io_job_send_to_mainloop (job, skip_op_report,
			     op, skip_op_free,
			     FALSE);
}

static void
skip_op_cancel (gpointer data)
{
  SkipAsyncOp *op = data;
  GInputStreamClass *class;

  class = G_INPUT_STREAM_GET_CLASS (op->stream);
  if (class->cancel_sync)
    class->cancel_sync (op->stream);
}

static void
g_input_stream_real_skip_async (GInputStream        *stream,
				gsize                count,
				int                  io_priority,
				GAsyncSkipCallback   callback,
				gpointer             data,
				GDestroyNotify       notify)
{
  SkipAsyncOp *op;

  op = g_new0 (SkipAsyncOp, 1);

  op->stream = stream;
  op->count_requested = count;
  op->callback = callback;
  op->data = data;
  op->notify = notify;
  
  stream->priv->io_job_id = g_schedule_io_job (skip_op_func,
					       skip_op_cancel,
					       op,
					       NULL,
					       io_priority,
					       g_input_stream_get_async_context (stream));
}


typedef struct {
  GInputStream      *stream;
  gboolean           res;
  GError            *error;
  GAsyncCloseInputCallback callback;
  gpointer           data;
  GDestroyNotify     notify;
} CloseAsyncOp;

static void
close_op_report (gpointer data)
{
  CloseAsyncOp *op = data;

  op->callback (op->stream,
		op->res,
		op->data,
		op->error);
}

static void
close_op_free (gpointer data)
{
  CloseAsyncOp *op = data;

  if (op->error)
    g_error_free (op->error);

  if (op->notify)
    op->notify (op->data);

  g_free (op);
}


static void
close_op_func (GIOJob *job,
	      gpointer data)
{
  CloseAsyncOp *op = data;
  GInputStreamClass *class;

  if (g_io_job_is_cancelled (job))
    {
      op->res = FALSE;
      g_set_error (&op->error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
    }
  else
    {
      class = G_INPUT_STREAM_GET_CLASS (op->stream);
      op->res = class->close (op->stream, &op->error);
    }

  g_io_job_mark_done (job);
  g_io_job_send_to_mainloop (job, close_op_report,
			     op, close_op_free,
			     FALSE);
}

static void
close_op_cancel (gpointer data)
{
  CloseAsyncOp *op = data;
  GInputStreamClass *class;

  class = G_INPUT_STREAM_GET_CLASS (op->stream);
  if (class->cancel_sync)
    class->cancel_sync (op->stream);
}

static void
g_input_stream_real_close_async (GInputStream       *stream,
				 int                 io_priority,
				 GAsyncCloseInputCallback callback,
				 gpointer            data,
				 GDestroyNotify      notify)
{
  CloseAsyncOp *op;

  op = g_new0 (CloseAsyncOp, 1);

  op->stream = stream;
  op->callback = callback;
  op->data = data;
  op->notify = notify;
  
  stream->priv->io_job_id = g_schedule_io_job (close_op_func,
					       close_op_cancel,
					       op,
					       NULL,
					       io_priority,
					       g_input_stream_get_async_context (stream));
}

static void
g_input_stream_real_cancel (GInputStream *stream)
{
  g_cancel_io_job (stream->priv->io_job_id);
}

