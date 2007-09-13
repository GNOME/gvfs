#include <config.h>
#include <glib/gi18n-lib.h>
#include <glib.h>

#include "ginputstream.h"
#include "gsimpleasyncresult.h"

G_DEFINE_TYPE (GInputStream, g_input_stream, G_TYPE_OBJECT);

struct _GInputStreamPrivate {
  guint closed : 1;
  guint pending : 1;
  GAsyncReadyCallback outstanding_callback;
};

static gssize   g_input_stream_real_skip         (GInputStream         *stream,
						  gsize                 count,
						  GCancellable         *cancellable,
						  GError              **error);
static void     g_input_stream_real_read_async   (GInputStream         *stream,
						  void                 *buffer,
						  gsize                 count,
						  int                   io_priority,
						  GCancellable         *cancellable,
						  GAsyncReadyCallback   callback,
						  gpointer              user_data);
static gssize   g_input_stream_real_read_finish  (GInputStream         *stream,
						  GAsyncResult         *result,
						  GError              **error);
static void     g_input_stream_real_skip_async   (GInputStream         *stream,
						  gsize                 count,
						  int                   io_priority,
						  GCancellable         *cancellable,
						  GAsyncReadyCallback   callback,
						  gpointer              data);
static gssize   g_input_stream_real_skip_finish  (GInputStream         *stream,
						  GAsyncResult         *result,
						  GError              **error);
static void     g_input_stream_real_close_async  (GInputStream         *stream,
						  int                   io_priority,
						  GCancellable         *cancellable,
						  GAsyncReadyCallback   callback,
						  gpointer              data);
static gboolean g_input_stream_real_close_finish (GInputStream         *stream,
						  GAsyncResult         *result,
						  GError              **error);

static void
g_input_stream_finalize (GObject *object)
{
  GInputStream *stream;

  stream = G_INPUT_STREAM (object);
  
  if (!stream->priv->closed)
    g_input_stream_close (stream, NULL, NULL);

  if (G_OBJECT_CLASS (g_input_stream_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_input_stream_parent_class)->finalize) (object);
}

static void
g_input_stream_class_init (GInputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GInputStreamPrivate));
  
  gobject_class->finalize = g_input_stream_finalize;
  
  klass->skip = g_input_stream_real_skip;
  klass->read_async = g_input_stream_real_read_async;
  klass->read_finish = g_input_stream_real_read_finish;
  klass->skip_async = g_input_stream_real_skip_async;
  klass->skip_finish = g_input_stream_real_skip_finish;
  klass->close_async = g_input_stream_real_close_async;
  klass->close_finish = g_input_stream_real_close_finish;
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
 * @cancellable: optional cancellable object
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Tries to read @count bytes from the stream into the buffer starting at
 * @buffer. Will block during this read.
 * 
 * If count is zero returns zero and does nothing. A value of @count
 * larger than %G_MAXSSIZE will cause a %G_IO_ERROR_INVALID_ARGUMENT error.
 *
 * On success, the number of bytes read into the buffer is returned.
 * It is not an error if this is not the same as the requested size, as it
 * can happen e.g. near the end of a file. Zero is returned on end of file
 * (or if @count is zero),  but never otherwise.
 *
 * If @cancellable is not NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the operation
 * was cancelled, the error G_IO_ERROR_CANCELLED will be returned. If an
 * operation was partially finished when the operation was cancelled the
 * partial result will be returned, without an error.
 *
 * On error -1 is returned and @error is set accordingly.
 * 
 * Return value: Number of bytes read, or -1 on error
 **/
gssize
g_input_stream_read  (GInputStream  *stream,
		      void          *buffer,
		      gsize          count,
		      GCancellable  *cancellable,
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
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   _("Too large count value passed to g_input_stream_read"));
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
  
  class = G_INPUT_STREAM_GET_CLASS (stream);

  if (class->read == NULL) 
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		   _("Input stream doesn't implement read"));
      return -1;
    }

  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  stream->priv->pending = TRUE;
  res = class->read (stream, buffer, count, cancellable, error);
  stream->priv->pending = FALSE;

  if (cancellable)
    g_pop_current_cancellable (cancellable);
  
  return res;
}

/**
 * g_input_stream_read_all:
 * @stream: a #GInputStream.
 * @buffer: a buffer to read data into (which should be at least count bytes long).
 * @count: the number of bytes that will be read from the stream
 * @bytes_read: location to store the number of bytes that was read from the stream
 * @cancellable: optional cancellable object
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Tries to read @count bytes from the stream into the buffer starting at
 * @buffer. Will block during this read.
 *
 * This function is similar to g_input_stream_read(), except it tries to
 * read as many bytes as requested, only stopping on an error or end of stream.
 *
 * On a successful read of @count bytes, or if we reached the end of the
 * stream,  TRUE is returned, and @bytes_read is set to the number of bytes
 * read into @buffer.
 * 
 * If there is an error during the operation FALSE is returned and @error
 * is set to indicate the error status, @bytes_read is updated to contain
 * the number of bytes read into @buffer before the error occured.
 *
 * Return value: TRUE on success, FALSE if there was an error
 **/
gboolean
g_input_stream_read_all (GInputStream              *stream,
			 void                      *buffer,
			 gsize                      count,
			 gsize                     *bytes_read,
			 GCancellable              *cancellable,
			 GError                   **error)
{
  gsize _bytes_read;
  gssize res;

  _bytes_read = 0;
  while (_bytes_read < count)
    {
      res = g_input_stream_read (stream, (char *)buffer + _bytes_read, count - _bytes_read,
				 cancellable, error);
      if (res == -1)
	{
	  *bytes_read = _bytes_read;
	  return FALSE;
	}
      
      if (res == 0)
	break;

      _bytes_read += res;
    }
  
  *bytes_read = _bytes_read;
  return TRUE;
}

/**
 * g_input_stream_skip:
 * @stream: a #GInputStream.
 * @count: the number of bytes that will be skipped from the stream
 * @cancellable: optional cancellable object
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
 * If @cancellable is not NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the operation
 * was cancelled, the error G_IO_ERROR_CANCELLED will be returned. If an
 * operation was partially finished when the operation was finished the
 * partial result will be returned, without an error.
 *
 * Return value: Number of bytes skipped, or -1 on error
 **/
gssize
g_input_stream_skip (GInputStream         *stream,
		     gsize                 count,
		     GCancellable         *cancellable,
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
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   _("Too large count value passed to g_input_stream_skip"));
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
  
  class = G_INPUT_STREAM_GET_CLASS (stream);

  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  stream->priv->pending = TRUE;
  res = class->skip (stream, count, cancellable, error);
  stream->priv->pending = FALSE;

  if (cancellable)
    g_pop_current_cancellable (cancellable);
  
  return res;
}

static gssize
g_input_stream_real_skip (GInputStream         *stream,
			  gsize                 count,
			  GCancellable         *cancellable,
			  GError              **error)
{
  GInputStreamClass *class;
  gssize ret;
  char *buffer;

  class = G_INPUT_STREAM_GET_CLASS (stream);

  /* TODO: Skip fallback uses too much memory, should do multiple calls */
  buffer = g_malloc (count);
  ret = class->read (stream, buffer, count, cancellable, error);
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
 * Once the stream is closed, all other operations will return %G_IO_ERROR_CLOSED.
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
 * close will still return %G_IO_ERROR_CLOSED all operations. Still, it
 * is important to check and report the error to the user.
 *
 * If @cancellable is not NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the operation
 * was cancelled, the error G_IO_ERROR_CANCELLED will be returned.
 * Cancelling a close will still leave the stream closed, but some streams
 * can use a faster close that doesn't block to e.g. check errors. 
 *
 * Return value: %TRUE on success, %FALSE on failure
 **/
gboolean
g_input_stream_close (GInputStream  *stream,
		      GCancellable  *cancellable,
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
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return FALSE;
    }
  
  res = TRUE;

  stream->priv->pending = TRUE;

  if (cancellable)
    g_push_current_cancellable (cancellable);

  if (class->close)
    res = class->close (stream, cancellable, error);

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
  GInputStream *stream = G_INPUT_STREAM (source_object);

  stream->priv->pending = FALSE;
  (*stream->priv->outstanding_callback) (source_object, res, user_data);
  g_object_unref (stream);
}

static void
async_ready_close_callback_wrapper (GObject *source_object,
				    GAsyncResult *res,
				    gpointer      user_data)
{
  GInputStream *stream = G_INPUT_STREAM (source_object);

  stream->priv->pending = FALSE;
  stream->priv->closed = TRUE;
  (*stream->priv->outstanding_callback) (source_object, res, user_data);
  g_object_unref (stream);
}

/**
 * g_input_stream_read_async:
 * @stream: A #GInputStream.
 * @buffer: a buffer to read data into (which should be at least count bytes long).
 * @count: the number of bytes that will be read from the stream
 * @io_priority: the io priority of the request
 * @cancellable: optional cancellable object
 * @callback: callback to call when the request is satisfied
 * @user_data: the data to pass to callback function
 *
 * Request an asynchronous read of @count bytes from the stream into the buffer
 * starting at @buffer. When the operation is finished @callback will be called,
 * giving the results.
 *
 * During an async request no other sync and async calls are allowed, and will
 * result in %G_IO_ERROR_PENDING errors. 
 *
 * A value of @count larger than %G_MAXSSIZE will cause a %G_IO_ERROR_INVALID_ARGUMENT error.
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
			   GCancellable        *cancellable,
			   GAsyncReadyCallback  callback,
			   gpointer             user_data)
{
  GInputStreamClass *class;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_IS_INPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);
  g_return_if_fail (buffer != NULL);

  if (count == 0)
    {
      simple = g_simple_async_result_new (G_OBJECT (stream),
					  callback,
					  user_data,
					  g_input_stream_read_async);
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      return;
    }
  
  if (((gssize) count) < 0)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (stream),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
					   _("Too large count value passed to g_input_stream_read_async"));
      return;
    }

  if (stream->priv->closed)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (stream),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_CLOSED,
					   _("Stream is already closed"));
      return;
    }
  
  if (stream->priv->pending)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (stream),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_PENDING,
					   _("Stream has outstanding operation"));
      return;
    }

  class = G_INPUT_STREAM_GET_CLASS (stream);
  
  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->read_async (stream, buffer, count, io_priority, cancellable,
		     async_ready_callback_wrapper, user_data);
}

gssize
g_input_stream_read_finish (GInputStream              *stream,
			    GAsyncResult              *result,
			    GError                   **error)
{
  GSimpleAsyncResult *simple;
  GInputStreamClass *class;

  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return -1;

      /* Special case read of 0 bytes */
      if (g_simple_async_result_get_source_tag (simple) == g_input_stream_read_async)
	return 0;
    }

  class = G_INPUT_STREAM_GET_CLASS (stream);
  return class->read_finish (stream, result, error);
}

/**
 * g_input_stream_skip_async:
 * @stream: A #GInputStream.
 * @count: the number of bytes that will be skipped from the stream
 * @io_priority: the io priority of the request
 * @cancellable: optional cancellable object
 * @callback: callback to call when the request is satisfied
 * @user_data: the data to pass to callback function
 *
 * Request an asynchronous skip of @count bytes from the stream into the buffer
 * starting at @buffer. When the operation is finished @callback will be called,
 * giving the results.
 *
 * During an async request no other sync and async calls are allowed, and will
 * result in %G_IO_ERROR_PENDING errors. 
 *
 * A value of @count larger than %G_MAXSSIZE will cause a %G_IO_ERROR_INVALID_ARGUMENT error.
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
			   GCancellable        *cancellable,
			   GAsyncReadyCallback  callback,
			   gpointer             user_data)
{
  GInputStreamClass *class;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_IS_INPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  if (count == 0)
    {
      simple = g_simple_async_result_new (G_OBJECT (stream),
					  callback,
					  user_data,
					  g_input_stream_skip_async);

      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      return;
    }
  
  if (((gssize) count) < 0)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (stream),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
					   _("Too large count value passed to g_input_stream_skip_async"));
      return;
    }

  if (stream->priv->closed)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (stream),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_CLOSED,
					   _("Stream is already closed"));
      return;
    }
  
  if (stream->priv->pending)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (stream),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_PENDING,
					   _("Stream has outstanding operation"));
      return;
    }

  class = G_INPUT_STREAM_GET_CLASS (stream);
  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->skip_async (stream, count, io_priority, cancellable,
		     async_ready_callback_wrapper, user_data);
}

gssize
g_input_stream_skip_finish (GInputStream              *stream,
			    GAsyncResult              *result,
			    GError                   **error)
{
  GSimpleAsyncResult *simple;
  GInputStreamClass *class;

  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return -1;

      /* Special case skip of 0 bytes */
      if (g_simple_async_result_get_source_tag (simple) == g_input_stream_skip_async)
	return 0;
    }

  class = G_INPUT_STREAM_GET_CLASS (stream);
  return class->skip_finish (stream, result, error);
}

/**
 * g_input_stream_close_async:
 * @stream: A #GInputStream.
 * @io_priority: the io priority of the request
 * @cancellable: optional cancellable object
 * @callback: callback to call when the request is satisfied
 * @user_data: the data to pass to callback function
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
			    GCancellable       *cancellable,
			    GAsyncReadyCallback callback,
			    gpointer            user_data)
{
  GInputStreamClass *class;
  GSimpleAsyncResult *simple;

  g_return_if_fail (G_IS_INPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  if (stream->priv->closed)
    {
      simple = g_simple_async_result_new (G_OBJECT (stream),
					  callback,
					  user_data,
					  g_input_stream_close_async);

      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      return;
    }

  if (stream->priv->pending)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (stream),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_PENDING,
					   _("Stream has outstanding operation"));
      return;
    }
  
  class = G_INPUT_STREAM_GET_CLASS (stream);
  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->close_async (stream, io_priority, cancellable,
		      async_ready_close_callback_wrapper, user_data);
}

gboolean
g_input_stream_close_finish (GInputStream              *stream,
			     GAsyncResult              *result,
			     GError                   **error)
{
  GSimpleAsyncResult *simple;
  GInputStreamClass *class;

  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return FALSE;

      /* Special case already closed */
      if (g_simple_async_result_get_source_tag (simple) == g_input_stream_close_async)
	return TRUE;
    }

  class = G_INPUT_STREAM_GET_CLASS (stream);
  return class->close_finish (stream, result, error);
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
  void              *buffer;
  gsize              count_requested;
  gssize             count_read;
} ReadData;

static void
read_async_thread (GSimpleAsyncResult *res,
		   GObject *object,
		   GCancellable *cancellable)
{
  ReadData *op;
  GInputStreamClass *class;
  GError *error = NULL;
 
  op = g_simple_async_result_get_op_res_gpointer (res);

  class = G_INPUT_STREAM_GET_CLASS (object);

  op->count_read = class->read (G_INPUT_STREAM (object),
				op->buffer, op->count_requested,
				cancellable, &error);
  if (op->count_read == -1)
    {
      g_simple_async_result_set_from_error (res, error);
      g_error_free (error);
    }
}

static void
g_input_stream_real_read_async (GInputStream *stream,
				void *buffer,
				gsize count,
				int io_priority,
				GCancellable *cancellable,
				GAsyncReadyCallback callback,
				gpointer user_data)
{
  GSimpleAsyncResult *res;
  ReadData *op;
  
  op = g_new (ReadData, 1);
  res = g_simple_async_result_new (G_OBJECT (stream), callback, user_data, g_input_stream_real_read_async);
  g_simple_async_result_set_op_res_gpointer (res, op, g_free);
  op->buffer = buffer;
  op->count_requested = count;
  
  g_simple_async_result_run_in_thread (res, read_async_thread, io_priority, cancellable);
  g_object_unref (res);
}

static gssize
g_input_stream_real_read_finish (GInputStream              *stream,
				 GAsyncResult              *result,
				 GError                   **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  ReadData *op;

  g_assert (g_simple_async_result_get_source_tag (simple) == 
	    g_input_stream_real_read_async);

  op = g_simple_async_result_get_op_res_gpointer (simple);

  return op->count_read;
}

typedef struct {
  gsize count_requested;
  gssize count_skipped;
} SkipData;


static void
skip_async_thread (GSimpleAsyncResult *res,
		   GObject *object,
		   GCancellable *cancellable)
{
  SkipData *op;
  GInputStreamClass *class;
  GError *error = NULL;
  
  class = G_INPUT_STREAM_GET_CLASS (object);
  op = g_simple_async_result_get_op_res_gpointer (res);
  op->count_skipped = class->skip (G_INPUT_STREAM (object),
				   op->count_requested,
				   cancellable, &error);
  if (op->count_skipped == -1)
    {
      g_simple_async_result_set_from_error (res, error);
      g_error_free (error);
    }
}

typedef struct {
  char *buffer;
  gpointer user_data;
  GAsyncReadyCallback callback;
} SkipFallbackAsyncData;

static void
skip_callback_wrapper (GObject *source_object,
		       GAsyncResult *res,
		       gpointer user_data)
{
  SkipFallbackAsyncData *data = user_data;
  SkipData *op;
  GSimpleAsyncResult *simple;
  GError *error = NULL;

  op = g_new0 (SkipData, 1);
  simple = g_simple_async_result_new (source_object,
				      data->callback, data->user_data,
				      g_input_stream_real_skip_async);

  g_simple_async_result_set_op_res_gpointer (simple, op, g_free);

  op->count_skipped =
    g_input_stream_read_finish (G_INPUT_STREAM (source_object), res, &error);

  if (op->count_skipped == -1)
    {
      g_simple_async_result_set_from_error (simple, error);
      g_error_free (error);
    }

  /* Complete immediately, not in idle, since we're already in a mainloop callout */
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
  
  g_free (data->buffer);
  g_free (data);
 }

static void
g_input_stream_real_skip_async (GInputStream        *stream,
				gsize                count,
				int                  io_priority,
				GCancellable        *cancellable,
				GAsyncReadyCallback  callback,
				gpointer             user_data)
{
  GInputStreamClass *class;
  SkipData *op;
  SkipFallbackAsyncData *data;
  GSimpleAsyncResult *res;

  class = G_INPUT_STREAM_GET_CLASS (stream);

  if (class->read_async == g_input_stream_real_read_async)
    {
      /* Read is thread-using async fallback. Make skip use
       * threads too, so that we can use a possible sync skip
       * implementation. */
      op = g_new0 (SkipData, 1);
      
      res = g_simple_async_result_new (G_OBJECT (stream), callback, user_data,
				       g_input_stream_real_skip_async);

      g_simple_async_result_set_op_res_gpointer (res, op, g_free);

      op->count_requested = count;

      g_simple_async_result_run_in_thread (res, skip_async_thread, io_priority, cancellable);
      g_object_unref (res);
    }
  else
    {
      /* TODO: Skip fallback uses too much memory, should do multiple read calls */
      
      /* There is a custom async read function, lets use that. */
      data = g_new (SkipFallbackAsyncData, 1);
      data->buffer = g_malloc (count);
      data->callback = callback;
      data->user_data = user_data;
      class->read_async (stream, data->buffer, count, io_priority, cancellable,
			 skip_callback_wrapper, data);
    }

}

static gssize
g_input_stream_real_skip_finish (GInputStream              *stream,
				 GAsyncResult              *result,
				 GError                   **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  SkipData *op;

  g_assert (g_simple_async_result_get_source_tag (simple) == g_input_stream_real_skip_async);
  op = g_simple_async_result_get_op_res_gpointer (simple);
  return op->count_skipped;
}

static void
close_async_thread (GSimpleAsyncResult *res,
		    GObject *object,
		    GCancellable *cancellable)
{
  GInputStreamClass *class;
  GError *error = NULL;
  gboolean result;

  /* Auto handling of cancelation disabled, and ignore
     cancellation, since we want to close things anyway, although
     possibly in a quick-n-dirty way. At least we never want to leak
     open handles */
  
  class = G_INPUT_STREAM_GET_CLASS (object);
  result = class->close (G_INPUT_STREAM (object), cancellable, &error);
  if (!result)
    {
      g_simple_async_result_set_from_error (res, error);
      g_error_free (error);
    }
}

static void
g_input_stream_real_close_async (GInputStream        *stream,
				 int                  io_priority,
				 GCancellable        *cancellable,
				 GAsyncReadyCallback  callback,
				 gpointer             user_data)
{
  GSimpleAsyncResult *res;
  
  res = g_simple_async_result_new (G_OBJECT (stream),
				   callback,
				   user_data,
				   g_input_stream_real_close_async);

  g_simple_async_result_set_handle_cancellation (res, FALSE);
  
  g_simple_async_result_run_in_thread (res,
				       close_async_thread,
				       io_priority,
				       cancellable);
  g_object_unref (res);
}

static gboolean
g_input_stream_real_close_finish (GInputStream              *stream,
				  GAsyncResult              *result,
				  GError                   **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  g_assert (g_simple_async_result_get_source_tag (simple) == g_input_stream_real_close_async);
  return TRUE;
}
