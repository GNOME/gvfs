#include <config.h>
#include <glib/gi18n-lib.h>

#include "goutputstream.h"
#include "gsimpleasyncresult.h"

G_DEFINE_TYPE (GOutputStream, g_output_stream, G_TYPE_OBJECT);

struct _GOutputStreamPrivate {
  guint closed : 1;
  guint pending : 1;
  guint cancelled : 1;
  GAsyncReadyCallback outstanding_callback;
};

static void     g_output_stream_real_write_async  (GOutputStream        *stream,
						   void                 *buffer,
						   gsize                 count,
						   int                   io_priority,
						   GCancellable         *cancellable,
						   GAsyncReadyCallback   callback,
						   gpointer              data);
static gssize   g_output_stream_real_write_finish (GOutputStream        *stream,
						   GAsyncResult         *result,
						   GError              **error);
static void     g_output_stream_real_flush_async  (GOutputStream        *stream,
						   int                   io_priority,
						   GCancellable         *cancellable,
						   GAsyncReadyCallback   callback,
						   gpointer              data);
static gboolean g_output_stream_real_flush_finish (GOutputStream        *stream,
						   GAsyncResult         *result,
						   GError              **error);
static void     g_output_stream_real_close_async  (GOutputStream        *stream,
						   int                   io_priority,
						   GCancellable         *cancellable,
						   GAsyncReadyCallback   callback,
						   gpointer              data);
static gboolean g_output_stream_real_close_finish (GOutputStream        *stream,
						   GAsyncResult         *result,
						   GError              **error);

static void
g_output_stream_finalize (GObject *object)
{
  GOutputStream *stream;

  stream = G_OUTPUT_STREAM (object);
  
  if (!stream->priv->closed)
    g_output_stream_close (stream, NULL, NULL);
  
  if (G_OBJECT_CLASS (g_output_stream_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_output_stream_parent_class)->finalize) (object);
}

static void
g_output_stream_class_init (GOutputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GOutputStreamPrivate));
  
  gobject_class->finalize = g_output_stream_finalize;
  
  klass->write_async = g_output_stream_real_write_async;
  klass->write_finish = g_output_stream_real_write_finish;
  klass->flush_async = g_output_stream_real_flush_async;
  klass->flush_finish = g_output_stream_real_flush_finish;
  klass->close_async = g_output_stream_real_close_async;
  klass->close_finish = g_output_stream_real_close_finish;
}

static void
g_output_stream_init (GOutputStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
					      G_TYPE_OUTPUT_STREAM,
					      GOutputStreamPrivate);
}

/**
 * g_output_stream_write:
 * @stream: a #GOutputStream.
 * @buffer: the buffer containing the data to write. 
 * @count: the number of bytes to write
 * @cancellable: optional cancellable object
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Tries to write @count bytes from @buffer into the stream. Will block
 * during the operation.
 * 
 * If count is zero returns zero and does nothing. A value of @count
 * larger than %G_MAXSSIZE will cause a %G_IO_ERROR_INVALID_ARGUMENT error.
 *
 * On success, the number of bytes written to the stream is returned.
 * It is not an error if this is not the same as the requested size, as it
 * can happen e.g. on a partial i/o error, or if the there is not enough
 * storage in the stream. All writes either block until at least one byte
 * is written, so zero is never returned (unless @count is zero).
 * 
 * If @cancellable is not NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the operation
 * was cancelled, the error G_IO_ERROR_CANCELLED will be returned. If an
 * operation was partially finished when the operation was cancelled the
 * partial result will be returned, without an error.
 *
 * On error -1 is returned and @error is set accordingly.
 * 
 * Return value: Number of bytes written, or -1 on error
 **/
gssize
g_output_stream_write (GOutputStream *stream,
		       void          *buffer,
		       gsize          count,
		       GCancellable  *cancellable,
		       GError       **error)
{
  GOutputStreamClass *class;
  gssize res;

  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), -1);
  g_return_val_if_fail (stream != NULL, -1);
  g_return_val_if_fail (buffer != NULL, 0);

  if (count == 0)
    return 0;
  
  if (((gssize) count) < 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   _("Too large count value passed to g_output_stream_write"));
      return -1;
    }

  if (stream->priv->closed)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		   _("Stream is already closed"));
      return -1;
    }
  
  if (stream->priv->pending)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return -1;
    }
  
  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  if (class->write == NULL) 
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		   _("Output stream doesn't implement write"));
      return -1;
    }
  
  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  stream->priv->pending = TRUE;
  res = class->write (stream, buffer, count, cancellable, error);
  stream->priv->pending = FALSE;
  
  if (cancellable)
    g_pop_current_cancellable (cancellable);
  
  return res; 
}

/**
 * g_output_stream_write_all:
 * @stream: a #GOutputStream.
 * @buffer: the buffer containing the data to write. 
 * @count: the number of bytes to write
 * @bytes_written: location to store the number of bytes that was written to the stream
 * @cancellable: optional cancellable object
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Tries to write @count bytes from @buffer into the stream. Will block
 * during the operation.
 * 
 * This function is similar to g_output_stream_write(), except it tries to
 * read as many bytes as requested, only stopping on an error.
 *
 * On a successful write of @count bytes, TRUE is returned, and @bytes_written
 * is set to @count.
 * 
 * If there is an error during the operation FALSE is returned and @error
 * is set to indicate the error status, @bytes_written is updated to contain
 * the number of bytes written into the stream before the error occured.
 *
 * Return value: TRUE on success, FALSE if there was an error
 **/
gssize
g_output_stream_write_all (GOutputStream *stream,
			   void          *buffer,
			   gsize          count,
			   gsize         *bytes_written,
			   GCancellable  *cancellable,
			   GError       **error)
{
  gsize _bytes_written;
  gssize res;

  _bytes_written = 0;
  while (_bytes_written < count)
    {
      res = g_output_stream_write (stream, (char *)buffer + _bytes_written, count - _bytes_written,
				   cancellable, error);
      if (res == -1)
	{
	  *bytes_written = _bytes_written;
	  return FALSE;
	}
      
      if (res == 0)
	g_warning ("Write returned zero without error");

      _bytes_written += res;
    }
  
  *bytes_written = _bytes_written;
  return TRUE;
}

/**
 * g_output_stream_flush:
 * @stream: a #GOutputStream.
 * @cancellable: optional cancellable object
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Flushed any outstanding buffers in the stream. Will block during the operation.
 * Closing the stream will implicitly cause a flush.
 *
 * This function is optional for inherited classes.
 * 
 * If @cancellable is not NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the operation
 * was cancelled, the error G_IO_ERROR_CANCELLED will be returned.
 *
 * Return value: TRUE on success, FALSE on error
 **/
gboolean
g_output_stream_flush (GOutputStream    *stream,
		       GCancellable  *cancellable,
		       GError          **error)
{
  GOutputStreamClass *class;
  gboolean res;

  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), FALSE);
  g_return_val_if_fail (stream != NULL, FALSE);

  if (stream->priv->closed)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		   _("Stream is already closed"));
      return FALSE;
    }

  if (stream->priv->pending)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return FALSE;
    }
  
  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  res = TRUE;
  if (class->flush)
    {
      if (cancellable)
	g_push_current_cancellable (cancellable);
      
      stream->priv->pending = TRUE;
      res = class->flush (stream, cancellable, error);
      stream->priv->pending = FALSE;
      
      if (cancellable)
	g_pop_current_cancellable (cancellable);
    }
  
  return res;
}

/**
 * g_output_stream_close:
 * @stream: A #GOutputStream.
 * @cancellable: optional cancellable object
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Closes the stream, releasing resources related to it.
 *
 * Once the stream is closed, all other operations will return %G_IO_ERROR_CLOSED.
 * Closing a stream multiple times will not return an error.
 *
 * Closing a stream will automatically flush any outstanding buffers in the
 * stream.
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
 * close will still return %G_IO_ERROR_CLOSED all operations. Still, it
 * is important to check and report the error to the user, otherwise
 * there might be a loss of data as all data might not be written.
 * 
 * If @cancellable is not NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the operation
 * was cancelled, the error G_IO_ERROR_CANCELLED will be returned.
 * Cancelling a close will still leave the stream closed, but there some streams
 * can use a faster close that doesn't block to e.g. check errors. On
 * cancellation (as with any error) there is no guarantee that all written
 * data will reach the target. 
 *
 * Return value: %TRUE on success, %FALSE on failure
 **/
gboolean
g_output_stream_close (GOutputStream  *stream,
		       GCancellable   *cancellable,
		       GError        **error)
{
  GOutputStreamClass *class;
  gboolean res;

  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), -1);
  g_return_val_if_fail (stream != NULL, -1);

  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  if (stream->priv->closed)
    return TRUE;

  if (stream->priv->pending)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return FALSE;
    }

  res = g_output_stream_flush (stream, cancellable, error);

  stream->priv->pending = TRUE;
  
  if (cancellable)
    g_push_current_cancellable (cancellable);

  if (!res)
    {
      /* flushing caused the error that we want to return,
       * but we still want to close the underlying stream if possible
       */
      if (class->close)
	class->close (stream, cancellable, NULL);
    }
  else
    {
      res = TRUE;
      if (class->close)
	res = class->close (stream, cancellable, error);
    }
  
  if (cancellable)
    g_pop_current_cancellable (cancellable);
  
  stream->priv->closed = TRUE;
  stream->priv->pending = FALSE;
  
  return res;
}

static void
async_ready_callback_wrapper (GObject *source_object,
			      GAsyncResult *res,
			      gpointer      user_data)
{
  GOutputStream *stream = G_OUTPUT_STREAM (source_object);

  stream->priv->pending = FALSE;
  (*stream->priv->outstanding_callback) (source_object, res, user_data);
  g_object_unref (stream);
}

static void
async_ready_close_callback_wrapper (GObject *source_object,
				    GAsyncResult *res,
				    gpointer      user_data)
{
  GOutputStream *stream = G_OUTPUT_STREAM (source_object);

  stream->priv->pending = FALSE;
  stream->priv->closed = TRUE;
  (*stream->priv->outstanding_callback) (source_object, res, user_data);
  g_object_unref (stream);
}

static void
report_error (GOutputStream *stream,
	      GAsyncReadyCallback callback,
	      gpointer user_data,
	      GQuark domain,
	      gint code,
	      const gchar *format,
	      ...)
{
  GSimpleAsyncResult *simple;
  va_list args;

  simple = g_simple_async_result_new (G_OBJECT (stream),
				      callback,
				      user_data, NULL,
				      NULL, NULL);

  va_start (args, format);
  g_simple_async_result_set_error_va (simple, domain, code, format, args);
  va_end (args);
  g_simple_async_result_complete_in_idle (simple);
  g_object_unref (simple);
}


/**
 * g_output_stream_write_async:
 * @stream: A #GOutputStream.
 * @buffer: the buffer containing the data to write. 
 * @count: the number of bytes to write
 * @io_priority: the io priority of the request
 * @callback: callback to call when the request is satisfied
 * @user_data: the data to pass to callback function
 * @cancellable: optional cancellable object
 *
 * Request an asynchronous write of @count bytes from @buffer into the stream.
 * When the operation is finished @callback will be called, giving the results.
 *
 * During an async request no other sync and async calls are allowed, and will
 * result in %G_IO_ERROR_PENDING errors. 
 *
 * A value of @count larger than %G_MAXSSIZE will cause a %G_IO_ERROR_INVALID_ARGUMENT error.
 *
 * On success, the number of bytes written will be passed to the
 * callback. It is not an error if this is not the same as the requested size, as it
 * can happen e.g. on a partial i/o error, but generally we try to write
 * as many bytes as requested. 
 *
 * Any outstanding i/o request with higher priority (lower numerical value) will
 * be executed before an outstanding request with lower priority. Default
 * priority is G_%PRIORITY_DEFAULT.
 *
 * The asyncronous methods have a default fallback that uses threads to implement
 * asynchronicity, so they are optional for inheriting classes. However, if you
 * override one you must override all.
 **/
void
g_output_stream_write_async (GOutputStream        *stream,
			     void                *buffer,
			     gsize                count,
			     int                  io_priority,
			     GCancellable        *cancellable,
			     GAsyncReadyCallback  callback,
			     gpointer             user_data)
{
  GOutputStreamClass *class;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);
  g_return_if_fail (buffer != NULL);

  if (count == 0)
    {
      simple = g_simple_async_result_new (G_OBJECT (stream),
					  callback,
					  user_data,
					  g_output_stream_write_async,
					  NULL, NULL);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      return;
    }

  if (((gssize) count) < 0)
    {
      report_error (stream,
		    callback,
		    user_data,
		    G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		    _("Too large count value passed to g_output_stream_write_async"));
      return;
    }

  if (stream->priv->closed)
    {
      report_error (stream,
		    callback,
		    user_data,
		    G_IO_ERROR, G_IO_ERROR_CLOSED,
		    _("Stream is already closed"));
      return;
    }
  
  if (stream->priv->pending)
    {
      report_error (stream,
		    callback,
		    user_data,
		    G_IO_ERROR, G_IO_ERROR_PENDING,
		    _("Stream has outstanding operation"));
      return;
    }

  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->write_async (stream, buffer, count, io_priority, cancellable,
		      async_ready_callback_wrapper, user_data);
}


gssize
g_output_stream_write_finish (GOutputStream *stream,
			      GAsyncResult *result,
			      GError **error)
{
  GSimpleAsyncResult *simple;
  GOutputStreamClass *class;

  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return -1;

      /* Special case writes of 0 bytes */
      if (g_simple_async_result_get_source_tag (simple) == g_output_stream_write_async)
	return 0;
    }
  
  class = G_OUTPUT_STREAM_GET_CLASS (stream);
  return class->write_finish (stream, result, error);
}

void
g_output_stream_flush_async (GOutputStream       *stream,
			     int                  io_priority,
			     GCancellable        *cancellable,
			     GAsyncReadyCallback  callback,
			     gpointer             user_data)
{
  GOutputStreamClass *class;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  if (stream->priv->closed)
    {
      report_error (stream,
		    callback,
		    user_data,
		    G_IO_ERROR, G_IO_ERROR_CLOSED,
		    _("Stream is already closed"));
      return;
    }
  
  if (stream->priv->pending)
    {
      report_error (stream,
		    callback,
		    user_data,
		    G_IO_ERROR, G_IO_ERROR_PENDING,
		    _("Stream has outstanding operation"));
      return;
    }

  class = G_OUTPUT_STREAM_GET_CLASS (stream);
  
  if (class->flush_async == NULL)
    {
      simple = g_simple_async_result_new (G_OBJECT (stream),
					  callback,
					  user_data,
					  g_output_stream_flush_async,
					  NULL, NULL);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      return;
    }
      
  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->flush_async (stream, io_priority, cancellable,
		      async_ready_callback_wrapper, user_data);
}

gboolean
g_output_stream_flush_finish (GOutputStream *stream,
			      GAsyncResult *result,
			      GError **error)
{
  GSimpleAsyncResult *simple;
  GOutputStreamClass *class;

  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return FALSE;

      /* Special case default implementation */
      if (g_simple_async_result_get_source_tag (simple) == g_output_stream_flush_async)
	return TRUE;
    }

  class = G_OUTPUT_STREAM_GET_CLASS (stream);
  return class->flush_finish (stream, result, error);
}


/**
 * g_output_stream_close_async:
 * @stream: A #GOutputStream.
 * @callback: callback to call when the request is satisfied
 * @user_data: the data to pass to callback function
 * @cancellable: optional cancellable object
 *
 * Requests an asynchronous closes of the stream, releasing resources related to it.
 * When the operation is finished @callback will be called, giving the results.
 *
 * For behaviour details see g_output_stream_close().
 *
 * The asyncronous methods have a default fallback that uses threads to implement
 * asynchronicity, so they are optional for inheriting classes. However, if you
 * override one you must override all.
 **/
void
g_output_stream_close_async (GOutputStream      *stream,
			     int                 io_priority,
			     GCancellable       *cancellable,
			     GAsyncReadyCallback callback,
			     gpointer            user_data)
{
  GOutputStreamClass *class;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);
  
  if (stream->priv->closed)
    {
      simple = g_simple_async_result_new (G_OBJECT (stream),
					  callback,
					  user_data,
					  g_output_stream_close_async,
					  NULL, NULL);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      return;
    }

  if (stream->priv->pending)
    {
      report_error (stream,
		    callback,
		    user_data,
		    G_IO_ERROR, G_IO_ERROR_PENDING,
		    _("Stream has outstanding operation"));
      return;
    }
  
  class = G_OUTPUT_STREAM_GET_CLASS (stream);
  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->close_async (stream, io_priority, cancellable,
		      async_ready_close_callback_wrapper, user_data);
}

gboolean
g_output_stream_close_finish (GOutputStream *stream,
			      GAsyncResult *result,
			      GError **error)
{
  GSimpleAsyncResult *simple;
  GOutputStreamClass *class;

  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return FALSE;

      /* Special case already closed */
      if (g_simple_async_result_get_source_tag (simple) == g_output_stream_close_async)
	return TRUE;
    }

  class = G_OUTPUT_STREAM_GET_CLASS (stream);
  return class->close_finish (stream, result, error);
}

gboolean
g_output_stream_is_closed (GOutputStream *stream)
{
  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), TRUE);
  g_return_val_if_fail (stream != NULL, TRUE);
  
  return stream->priv->closed;
}
  
gboolean
g_output_stream_has_pending (GOutputStream *stream)
{
  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), TRUE);
  g_return_val_if_fail (stream != NULL, TRUE);
  
  return stream->priv->pending;
}

void
g_output_stream_set_pending (GOutputStream              *stream,
			    gboolean                   pending)
{
  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);
  
  stream->priv->pending = pending;
}


/********************************************
 *   Default implementation of async ops    *
 ********************************************/

typedef struct {
  void               *buffer;
  gsize               count_requested;
  gssize              count_written;
} WriteData;

static void
write_async_thread (GSimpleAsyncResult *res,
		   gpointer op_data,
		   GObject *object,
		   GCancellable *cancellable)
{
  WriteData *op = op_data;
  GOutputStreamClass *class;
  GError *error = NULL;

  class = G_OUTPUT_STREAM_GET_CLASS (object);
  op->count_written = class->write (G_OUTPUT_STREAM (object), op->buffer, op->count_requested,
				    cancellable, &error);
  if (op->count_written == -1)
    {
      g_simple_async_result_set_from_error (res, error);
      g_error_free (error);
    }
}

static void
g_output_stream_real_write_async (GOutputStream       *stream,
				  void                *buffer,
				  gsize                count,
				  int                  io_priority,
				  GCancellable        *cancellable,
				  GAsyncReadyCallback  callback,
				  gpointer             user_data)
{
  GSimpleAsyncResult *res;
  WriteData *op;

  op = g_new0 (WriteData, 1);
  res = g_simple_async_result_new (G_OBJECT (stream), callback, user_data, g_output_stream_real_write_async, op, g_free);
  op->buffer = buffer;
  op->count_requested = count;
  
  g_simple_async_result_run_in_thread (res, write_async_thread, io_priority, cancellable);
  g_object_unref (res);
}

static gssize
g_output_stream_real_write_finish (GOutputStream *stream,
				   GAsyncResult *result,
				   GError **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  WriteData *op;

  g_assert (g_simple_async_result_get_source_tag (simple) == g_output_stream_real_write_async);

  op = g_simple_async_result_get_op_data (simple);
  return op->count_written;
}

static void
flush_async_thread (GSimpleAsyncResult *res,
		    gpointer op_data,
		    GObject *object,
		    GCancellable *cancellable)
{
  GOutputStreamClass *class;
  gboolean result;
  GError *error = NULL;

  class = G_OUTPUT_STREAM_GET_CLASS (object);
  result = TRUE;
  if (class->flush)
    result = class->flush (G_OUTPUT_STREAM (object), cancellable, &error);

  if (!result)
    {
      g_simple_async_result_set_from_error (res, error);
      g_error_free (error);
    }
}

static void
g_output_stream_real_flush_async (GOutputStream       *stream,
				  int                  io_priority,
				  GCancellable        *cancellable,
				  GAsyncReadyCallback  callback,
				  gpointer             user_data)
{
  GSimpleAsyncResult *res;

  res = g_simple_async_result_new (G_OBJECT (stream), callback, user_data, g_output_stream_real_write_async, NULL, NULL);
  
  g_simple_async_result_run_in_thread (res, flush_async_thread, io_priority, cancellable);
  g_object_unref (res);
}

static gboolean
g_output_stream_real_flush_finish (GOutputStream *stream,
				   GAsyncResult *result,
				   GError **error)
{
  return TRUE;
}

static void
close_async_thread (GSimpleAsyncResult *res,
		    gpointer op_data,
		    GObject *object,
		    GCancellable *cancellable)
{
  GOutputStreamClass *class;
  GError *error = NULL;
  gboolean result;

  /* Auto handling of cancelation disabled, and ignore
     cancellation, since we want to close things anyway, although
     possibly in a quick-n-dirty way. At least we never want to leak
     open handles */
  
  class = G_OUTPUT_STREAM_GET_CLASS (object);
  result = class->close (G_OUTPUT_STREAM (object), cancellable, &error);
  if (!result)
    {
      g_simple_async_result_set_from_error (res, error);
      g_error_free (error);
    }
}

static void
g_output_stream_real_close_async (GOutputStream      *stream,
				  int                 io_priority,
				  GCancellable       *cancellable,
				  GAsyncReadyCallback callback,
				  gpointer            user_data)
{
  GSimpleAsyncResult *res;
  
  res = g_simple_async_result_new (G_OBJECT (stream), callback, user_data, g_output_stream_real_close_async, NULL, NULL);

  g_simple_async_result_set_handle_cancellation (res, FALSE);
  
  g_simple_async_result_run_in_thread (res, close_async_thread, io_priority, cancellable);
  g_object_unref (res);
}

static gboolean
g_output_stream_real_close_finish (GOutputStream              *stream,
				   GAsyncResult              *result,
				   GError                   **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  g_assert (g_simple_async_result_get_source_tag (simple) == g_output_stream_real_close_async);
  return TRUE;
}
