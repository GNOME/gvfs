#include <config.h>
#include <glib/gi18n-lib.h>
#include <glib.h>

#include "ginputstream.h"
#include "gioscheduler.h"
#include "gasynchelper.h"

G_DEFINE_TYPE (GInputStream, g_input_stream, G_TYPE_OBJECT);

struct _GInputStreamPrivate {
  guint closed : 1;
  guint pending : 1;
  gpointer outstanding_callback;
};

static gssize g_input_stream_real_skip        (GInputStream         *stream,
					       gsize                 count,
					       GCancellable         *cancellable,
					       GError              **error);
static void   g_input_stream_real_read_async  (GInputStream         *stream,
					       void                 *buffer,
					       gsize                 count,
					       int                   io_priority,
					       GAsyncReadCallback    callback,
					       gpointer              data,
					       GCancellable         *cancellable);
static void   g_input_stream_real_skip_async  (GInputStream         *stream,
					       gsize                 count,
					       int                   io_priority,
					       GAsyncSkipCallback    callback,
					       gpointer              data,
					       GCancellable         *cancellable);
static void   g_input_stream_real_close_async (GInputStream         *stream,
					       int                   io_priority,
					       GAsyncCloseInputCallback   callback,
					       gpointer              data,
					       GCancellable         *cancellable);


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
  klass->skip_async = g_input_stream_real_skip_async;
  klass->close_async = g_input_stream_real_close_async;
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

typedef struct {
  GAsyncResult       generic;
  void              *buffer;
  gsize              count_requested;
  gssize             count_read;
  GAsyncReadCallback callback;
} ReadAsyncResult;

static gboolean
call_read_async_result (gpointer data)
{
  ReadAsyncResult *res = data;

  if (res->callback)
    res->callback (res->generic.async_object,
		   res->buffer,
		   res->count_requested,
		   res->count_read,
		   res->generic.user_data,
		   res->generic.error);

  return FALSE;
}

static void
queue_read_async_result (GInputStream      *stream,
			 void              *buffer,
			 gsize              count_requested,
			 gssize             count_read,
			 GError            *error,
			 GAsyncReadCallback callback,
			 gpointer           data)
{
  ReadAsyncResult *res;

  res = g_new0 (ReadAsyncResult, 1);

  res->buffer = buffer;
  res->count_requested = count_requested;
  res->count_read = count_read;
  res->callback = callback;

  _g_queue_async_result ((GAsyncResult *)res, stream,
			 error, data,
			 call_read_async_result);
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
 * @user_data: the data to pass to callback function
 * @cancellable: optional cancellable object
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
			   GAsyncReadCallback   callback,
			   gpointer             user_data,
			   GCancellable        *cancellable)
{
  GInputStreamClass *class;
  GError *error;

  g_return_if_fail (G_IS_INPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);
  g_return_if_fail (buffer != NULL);

  if (count == 0)
    {
      queue_read_async_result (stream, buffer, count, 0, NULL,
			       callback, user_data);
      return;
    }
  
  if (((gssize) count) < 0)
    {
      error = NULL;
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   _("Too large count value passed to g_input_stream_read_async"));
      queue_read_async_result (stream, buffer, count, -1, error,
			       callback, user_data);
      return;
    }

  if (stream->priv->closed)
    {
      error = NULL;
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		   _("Stream is already closed"));
      queue_read_async_result (stream, buffer, count, -1, error,
			       callback, user_data);
      return;
    }
  
  if (stream->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      queue_read_async_result (stream, buffer, count, -1, error,
			       callback, user_data);
      return;
    }


  class = G_INPUT_STREAM_GET_CLASS (stream);
  
  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->read_async (stream, buffer, count, io_priority, read_async_callback_wrapper, user_data, cancellable);
}

typedef struct {
  GAsyncResult       generic;
  gsize              count_requested;
  gssize             count_skipped;
  GAsyncSkipCallback callback;
} SkipAsyncResult;

static gboolean
call_skip_async_result (gpointer data)
{
  SkipAsyncResult *res = data;

  if (res->callback)
    res->callback (res->generic.async_object,
		   res->count_requested,
		   res->count_skipped,
		   res->generic.user_data,
		   res->generic.error);

  return FALSE;
}

static void
queue_skip_async_result (GInputStream      *stream,
			 gsize              count_requested,
			 gssize             count_skipped,
			 GError            *error,
			 GAsyncSkipCallback callback,
			 gpointer           data)
{
  SkipAsyncResult *res;

  res = g_new0 (SkipAsyncResult, 1);

  res->count_requested = count_requested;
  res->count_skipped = count_skipped;
  res->callback = callback;

  _g_queue_async_result ((GAsyncResult *)res, stream,
			 error, data,
			 call_skip_async_result);
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
			   GAsyncSkipCallback   callback,
			   gpointer             user_data,
			   GCancellable        *cancellable)
{
  GInputStreamClass *class;
  GError *error;

  g_return_if_fail (G_IS_INPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  if (count == 0)
    {
      queue_skip_async_result (stream, count, 0, NULL,
			       callback, user_data);
      return;
    }
  
  if (((gssize) count) < 0)
    {
      error = NULL;
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   _("Too large count value passed to g_input_stream_skip_async"));
      queue_skip_async_result (stream, count, -1, error,
			       callback, user_data);
      return;
    }

  if (stream->priv->closed)
    {
      error = NULL;
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		   _("Stream is already closed"));
      queue_skip_async_result (stream, count, -1, error,
			       callback, user_data);
      return;
    }
  
  if (stream->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      queue_skip_async_result (stream, count, -1, error,
			       callback, user_data);
      return;
    }

  class = G_INPUT_STREAM_GET_CLASS (stream);
  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->skip_async (stream, count, io_priority, skip_async_callback_wrapper, user_data, cancellable);
}


typedef struct {
  GAsyncResult             generic;
  gboolean                 result;
  GAsyncCloseInputCallback callback;
} CloseAsyncResult;

static gboolean
call_close_async_result (gpointer data)
{
  CloseAsyncResult *res = data;

  if (res->callback)
    res->callback (res->generic.async_object,
		   res->result,
		   res->generic.user_data,
		   res->generic.error);

  return FALSE;
}

static void
queue_close_async_result (GInputStream       *stream,
			  gboolean            result,
			  GError             *error,
			  GAsyncCloseInputCallback callback,
			  gpointer            data)
{
  CloseAsyncResult *res;

  res = g_new0 (CloseAsyncResult, 1);

  res->result = result;
  res->callback = callback;

  _g_queue_async_result ((GAsyncResult *)res, stream,
			 error, data,
			 call_close_async_result);
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
			    GAsyncCloseInputCallback callback,
			    gpointer            user_data,
			    GCancellable       *cancellable)
{
  GInputStreamClass *class;
  GError *error;

  g_return_if_fail (G_IS_INPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  if (stream->priv->closed)
    {
      queue_close_async_result (stream, TRUE, NULL,
				callback, user_data);
      return;
    }

  if (stream->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      queue_close_async_result (stream, FALSE, error,
				callback, user_data);
      return;
    }
  
  class = G_INPUT_STREAM_GET_CLASS (stream);
  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->close_async (stream, io_priority, close_async_callback_wrapper, user_data, cancellable);
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
  GInputStream     *stream;
  GError            *error;
  gpointer           data;
} InputStreamOp;

static void
input_stream_op_free (gpointer data)
{
  InputStreamOp *op = data;

  if (op->error)
    g_error_free (op->error);

  g_free (op);
}

typedef struct {
  InputStreamOp      op;
  void              *buffer;
  gsize              count_requested;
  gssize             count_read;
  GAsyncReadCallback callback;
} ReadAsyncOp;

static void
read_op_report (gpointer data)
{
  ReadAsyncOp *op = data;

  op->callback (op->op.stream,
		op->buffer,
		op->count_requested,
		op->count_read,
		op->op.data,
		op->op.error);

}

static void
read_op_func (GIOJob *job,
	      GCancellable *c,
	      gpointer data)
{
  ReadAsyncOp *op = data;
  GInputStreamClass *class;

  if (g_cancellable_is_cancelled (c))
    {
      op->count_read = -1;
      g_set_error (&op->op.error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
    }
  else
    {
      class = G_INPUT_STREAM_GET_CLASS (op->op.stream);
      op->count_read = class->read (op->op.stream, op->buffer, op->count_requested,
				    c, &op->op.error);
    }

  g_io_job_send_to_mainloop (job, read_op_report,
			     op, input_stream_op_free,
			     FALSE);
}

static void
g_input_stream_real_read_async (GInputStream        *stream,
				void                *buffer,
				gsize                count,
				int                  io_priority,
				GAsyncReadCallback   callback,
				gpointer             data,
				GCancellable        *cancellable)
{
  ReadAsyncOp *op;

  op = g_new0 (ReadAsyncOp, 1);

  op->op.stream = stream;
  op->buffer = buffer;
  op->count_requested = count;
  op->callback = callback;
  op->op.data = data;
  
  g_schedule_io_job (read_op_func, op, NULL, io_priority,
		     cancellable);
}

typedef struct {
  InputStreamOp      op;
  gsize              count_requested;
  gssize             count_skipped;
  GAsyncSkipCallback callback;
} SkipAsyncOp;

static void
skip_op_report (gpointer data)
{
  SkipAsyncOp *op = data;

  op->callback (op->op.stream,
		op->count_requested,
		op->count_skipped,
		op->op.data,
		op->op.error);

}

static void
skip_op_func (GIOJob *job,
	      GCancellable *c,
	      gpointer data)
{
  SkipAsyncOp *op = data;
  GInputStreamClass *class;

  if (g_cancellable_is_cancelled (c))
    {
      op->count_skipped = -1;
      g_set_error (&op->op.error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
    }
  else
    {
      class = G_INPUT_STREAM_GET_CLASS (op->op.stream);
      op->count_skipped = class->skip (op->op.stream, op->count_requested,
				       c, &op->op.error);
    }

  g_io_job_send_to_mainloop (job, skip_op_report,
			     op, input_stream_op_free,
			     FALSE);
}

typedef struct {
  char *buffer;
  gpointer user_data;
  GAsyncSkipCallback callback;
} SkipFallbackAsyncData;

static void
skip_callback_wrapper (GInputStream *stream,
		       void         *buffer,
		       gsize         count_requested,
		       gssize        count_skipped,
		       gpointer      _data,
		       GError       *error)
{
  SkipFallbackAsyncData *data = _data;

  data->callback (stream, count_requested, count_skipped, data->user_data, error);
  g_free (data->buffer);
  g_free (data);
}

static void
g_input_stream_real_skip_async (GInputStream        *stream,
				gsize                count,
				int                  io_priority,
				GAsyncSkipCallback   callback,
				gpointer             user_data,
				GCancellable        *cancellable)
{
  GInputStreamClass *class;
  SkipAsyncOp *op;
  SkipFallbackAsyncData *data;

  class = G_INPUT_STREAM_GET_CLASS (stream);

  if (class->read_async == g_input_stream_real_read_async)
    {
      /* Read is thread-using async fallback. Make skip use
       * threads too, so that we can use a possible sync skip
       * implementation. */
      op = g_new0 (SkipAsyncOp, 1);
      
      op->op.stream = stream;
      op->count_requested = count;
      op->callback = callback;
      op->op.data = user_data;
      
      g_schedule_io_job (skip_op_func,
			 op,
			 NULL,
			 io_priority,
			 cancellable);
    }
  else
    {
      /* TODO: Skip fallback uses too much memory, should do multiple calls */
      /* Custom async read function, lets use that. */
      data = g_new (SkipFallbackAsyncData, 1);
      data->buffer = g_malloc (count);
      data->callback = callback;
      data->user_data = user_data;
      class->read_async (stream, data->buffer, count, io_priority,
			 skip_callback_wrapper, data, cancellable);
    }

}

typedef struct {
  InputStreamOp      op;
  gboolean           res;
  GAsyncCloseInputCallback callback;
} CloseAsyncOp;

static void
close_op_report (gpointer data)
{
  CloseAsyncOp *op = data;

  op->callback (op->op.stream,
		op->res,
		op->op.data,
		op->op.error);
}

static void
close_op_func (GIOJob *job,
	       GCancellable *c,
	       gpointer data)
{
  CloseAsyncOp *op = data;
  GInputStreamClass *class;

  if (g_cancellable_is_cancelled (c))
    {
      op->res = FALSE;
      g_set_error (&op->op.error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
    }
  else
    {
      class = G_INPUT_STREAM_GET_CLASS (op->op.stream);
      op->res = class->close (op->op.stream, c, &op->op.error);
    }

  g_io_job_send_to_mainloop (job, close_op_report,
			     op, input_stream_op_free,
			     FALSE);
}

static void
g_input_stream_real_close_async (GInputStream       *stream,
				 int                 io_priority,
				 GAsyncCloseInputCallback callback,
				 gpointer            data,
				 GCancellable       *cancellable)
{
  CloseAsyncOp *op;

  op = g_new0 (CloseAsyncOp, 1);

  op->op.stream = stream;
  op->callback = callback;
  op->op.data = data;
  
  g_schedule_io_job (close_op_func,
		     op,
		     NULL,
		     io_priority,
		     cancellable);
}
