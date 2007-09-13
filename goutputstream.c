#include <config.h>
#include <glib/gi18n-lib.h>

#include "goutputstream.h"
#include "gioscheduler.h"

G_DEFINE_TYPE (GOutputStream, g_output_stream, G_TYPE_OBJECT);

static GObjectClass *parent_class = NULL;

struct _GOutputStreamPrivate {
  guint closed : 1;
  guint pending : 1;
  guint cancelled : 1;
  GMainContext *context;
  gint io_job_id;
};

static void g_output_stream_real_write_async (GOutputStream             *stream,
					      void                      *buffer,
					      gsize                      count,
					      int                        io_priority,
					      GAsyncWriteCallback        callback,
					      gpointer                   data,
					      GDestroyNotify             notify);
static void g_output_stream_real_flush_async (GOutputStream             *stream,
					      int                        io_priority,
					      GAsyncFlushCallback        callback,
					      gpointer                   data,
					      GDestroyNotify             notify);
static void g_output_stream_real_close_async (GOutputStream             *stream,
					      int                        io_priority,
					      GAsyncCloseOutputCallback  callback,
					      gpointer                   data,
					      GDestroyNotify             notify);
static void g_output_stream_real_cancel      (GOutputStream             *stream);

static void
g_output_stream_finalize (GObject *object)
{
  GOutputStream *stream;

  stream = G_OUTPUT_STREAM (object);
  
  if (!stream->priv->closed)
    g_output_stream_close (stream, NULL);
  
  if (stream->priv->context)
    {
      g_main_context_unref (stream->priv->context);
      stream->priv->context = NULL;
    }

  if (G_OBJECT_CLASS (parent_class)->finalize)
    (*G_OBJECT_CLASS (parent_class)->finalize) (object);
}

static void
g_output_stream_class_init (GOutputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  parent_class = g_type_class_peek_parent (klass);
  
  g_type_class_add_private (klass, sizeof (GOutputStreamPrivate));
  
  gobject_class->finalize = g_output_stream_finalize;
  
  klass->write_async = g_output_stream_real_write_async;
  klass->flush_async = g_output_stream_real_flush_async;
  klass->close_async = g_output_stream_real_close_async;
  klass->cancel = g_output_stream_real_cancel;
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
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Tries to write @count bytes from @buffer into the stream. Will block
 * during the operation.
 * 
 * If count is zero returns zero and does nothing. A value of @count
 * larger than %G_MAXSSIZE will cause a %G_VFS_ERROR_INVALID_ARGUMENT error.
 *
 * On success, the number of bytes written to the stream is returned.
 * It is not an error if this is not the same as the requested size, as it
 * can happen e.g. on a partial i/o error, but generally we try to write
 * as many bytes as requested. 
 * 
 * On error -1 is returned and @error is set accordingly.
 * 
 * Return value: Number of bytes written, or -1 on error
 **/
gssize
g_output_stream_write (GOutputStream *stream,
		       void          *buffer,
		       gsize          count,
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
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_INVALID_ARGUMENT,
		   _("Too large count value passed to g_output_stream_write"));
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
  
  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  if (class->write == NULL) 
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_INTERNAL_ERROR,
		   _("Output stream doesn't implement write"));
      return -1;
    }
  
  stream->priv->pending = TRUE;
  res = class->write (stream, buffer, count, error);
  stream->priv->pending = FALSE;
  
  return res; 
}

/**
 * g_output_stream_flush:
 * @stream: a #GOutputStream.
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Flushed any outstanding buffers in the stream. Will block during the operation.
 * Closing the stream will implicitly cause a flush.
 *
 * This function is optional for inherited classes.
 *
 * Return value: TRUE on success, FALSE on error
 **/
gboolean
g_output_stream_flush (GOutputStream    *stream,
		       GError          **error)
{
  GOutputStreamClass *class;
  gboolean res;

  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), FALSE);
  g_return_val_if_fail (stream != NULL, FALSE);

  if (stream->priv->closed)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Stream is already closed"));
      return FALSE;
    }

  if (stream->priv->pending)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return FALSE;
    }
  
  class = G_OUTPUT_STREAM_GET_CLASS (stream);
  
  stream->priv->pending = TRUE;
  res = TRUE;
  if (class->flush)
    res = class->flush (stream, error);
  stream->priv->pending = FALSE;

  return res;
}

/**
 * g_output_stream_close:
 * @stream: A #GOutputStream.
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Closes the stream, releasing resources related to it.
 *
 * Once the stream is closed, all other operations will return %G_VFS_ERROR_CLOSED.
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
 * close will still return %G_VFS_ERROR_CLOSED all operations.
 * 
 * Return value: %TRUE on success, %FALSE on failure
 **/
gboolean
g_output_stream_close (GOutputStream  *stream,
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
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return FALSE;
    }

  res = g_output_stream_flush (stream, error);

  stream->priv->pending = TRUE;
  
  if (!res)
    {
      /* flushing caused the error that we want to return,
       * but we still want to close the underlying stream if possible
       */
      if (class->close)
	class->close (stream, NULL);
    }
  else
    {
      res = TRUE;
      if (class->close)
	res = class->close (stream, error);
    }
  
  stream->priv->closed = TRUE;
  stream->priv->pending = FALSE;
  
  return res;
}

/**
 * g_output_stream_set_async_context:
 * @stream: A #GOutputStream.
 * @context: a #GMainContext (if %NULL, the default context will be used)
 *
 * Set the mainloop @context to be used for asynchronous i/o.
 * If not set, or if set to %NULL the default context will be used.
 **/
void
g_output_stream_set_async_context (GOutputStream *stream,
				   GMainContext *context)
{
  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  if (stream->priv->context)
    g_main_context_unref (stream->priv->context);
  
  stream->priv->context = context;
  
  if (context)
    g_main_context_ref (context);
}
  
/**
 * g_output_stream_set_async_context:
 * @stream: A #GOutputStream.
 *
 * Returns the mainloop used for async operation on this stream.
 * If you implement a stream you have to look at this to know what
 * context to use for async i/o.
 *
 * The context is set by the user by calling g_output_stream_set_async_context().
 *
 * Return value: A #GMainContext
 **/
GMainContext *
g_output_stream_get_async_context (GOutputStream *stream)
{
  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), NULL);
  g_return_val_if_fail (stream != NULL, NULL);

  if (stream->priv->context == NULL)
    {
      stream->priv->context = g_main_context_default ();
      g_main_context_ref (stream->priv->context);
    }
      
  return stream->priv->context;
}

typedef struct {
  GOutputStream      *stream;
  void               *buffer;
  gsize               bytes_requested;
  gssize              bytes_written;
  GError             *error;
  GAsyncWriteCallback callback;
  gpointer            data;
  GDestroyNotify      notify;
} WriteAsyncResult;

static gboolean
call_write_async_result (gpointer data)
{
  WriteAsyncResult *res = data;

  if (res->callback)
    res->callback (res->stream,
		   res->buffer,
		   res->bytes_requested,
		   res->bytes_written,
		   res->data,
		   res->error);

  return FALSE;
}

static void
write_async_result_free (gpointer data)
{
  WriteAsyncResult *res = data;

  if (res->notify)
    res->notify (res->data);

  if (res->error)
    g_error_free (res->error);

  g_object_unref (res->stream);
  
  g_free (res);
}

static void
queue_write_async_result (GOutputStream      *stream,
			  void               *buffer,
			  gsize               bytes_requested,
			  gssize              bytes_written,
			  GError             *error,
			  GAsyncWriteCallback callback,
			  gpointer            data,
			  GDestroyNotify      notify)
{
  GSource *source;
  WriteAsyncResult *res;

  res = g_new0 (WriteAsyncResult, 1);

  res->stream = g_object_ref (stream);
  res->buffer = buffer;
  res->bytes_requested = bytes_requested;
  res->bytes_written = bytes_written;
  res->error = error;
  res->callback = callback;
  res->data = data;
  res->notify = notify;
  
  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, call_write_async_result, res, write_async_result_free);
  g_source_attach (source, g_output_stream_get_async_context (stream));
  g_source_unref (source);
}

/**
 * g_output_stream_write_async:
 * @stream: A #GOutputStream.
 * @buffer: the buffer containing the data to write. 
 * @count: the number of bytes to write
 * @io_priority: the io priority of the request
 * @callback: callback to call when the request is satisfied
 * @data: the data to pass to callback function
 * @notify: a function to call when @data is no longer in use, or %NULL.
 *
 * Request an asynchronous write of @count bytes from @buffer into the stream.
 * When the operation is finished @callback will be called, giving the results.
 *
 * During an async request no other sync and async calls are allowed, and will
 * result in %G_VFS_ERROR_PENDING errors. 
 *
 * A value of @count larger than %G_MAXSSIZE will cause a %G_VFS_ERROR_INVALID_ARGUMENT error.
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
			     GAsyncWriteCallback  callback,
			     gpointer             data,
			     GDestroyNotify       notify)
{
  GOutputStreamClass *class;
  GError *error;

  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);
  g_return_if_fail (buffer != NULL);

  stream->priv->cancelled = FALSE;
  
  if (count == 0)
    {
      queue_write_async_result (stream, buffer, count, 0, NULL,
				callback, data, notify);
      return;
    }

  if (((gssize) count) < 0)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_INVALID_ARGUMENT,
		   _("Too large count value passed to g_input_stream_read_async"));
      queue_write_async_result (stream, buffer, count, -1, error,
				callback, data, notify);
      return;
    }

  if (stream->priv->closed)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Stream is already closed"));
      queue_write_async_result (stream, buffer, count, -1, error,
				callback, data, notify);
      return;
    }
  
  if (stream->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      queue_write_async_result (stream, buffer, count, -1, error,
				callback, data, notify);
      return;
    }

  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  stream->priv->pending = TRUE;
  return class->write_async (stream, buffer, count, io_priority, callback, data, notify);
}

typedef struct {
  GOutputStream      *stream;
  gboolean            result;
  GError             *error;
  GAsyncFlushCallback callback;
  gpointer            data;
  GDestroyNotify      notify;
} FlushAsyncResult;

static gboolean
call_flush_async_result (gpointer data)
{
  FlushAsyncResult *res = data;

  if (res->callback)
    res->callback (res->stream,
		   res->result,
		   res->data,
		   res->error);

  return FALSE;
}

static void
flush_async_result_free (gpointer data)
{
  FlushAsyncResult *res = data;

  if (res->notify)
    res->notify (res->data);

  if (res->error)
    g_error_free (res->error);

  g_object_unref (res->stream);
  
  g_free (res);
}

static void
queue_flush_async_result (GOutputStream      *stream,
			  gboolean            result,
			  GError             *error,
			  GAsyncFlushCallback callback,
			  gpointer            data,
			  GDestroyNotify      notify)
{
  GSource *source;
  FlushAsyncResult *res;

  res = g_new0 (FlushAsyncResult, 1);

  res->stream = g_object_ref (stream);
  res->result = result;
  res->error = error;
  res->callback = callback;
  res->data = data;
  res->notify = notify;
  
  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);
  g_source_set_callback (source, call_flush_async_result, res, flush_async_result_free);
  g_source_attach (source, g_output_stream_get_async_context (stream));
  g_source_unref (source);
}

void
g_output_stream_flush_async (GOutputStream       *stream,
			     int                  io_priority,
			     GAsyncFlushCallback  callback,
			     gpointer             data,
			     GDestroyNotify       notify)
{
  GOutputStreamClass *class;
  GError *error;

  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  stream->priv->cancelled = FALSE;
  
  if (stream->priv->closed)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Stream is already closed"));
      queue_flush_async_result (stream, FALSE, error,
				callback, data, notify);
      return;
    }
  
  if (stream->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      queue_flush_async_result (stream, FALSE, error,
				callback, data, notify);
      return;
    }

  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  stream->priv->pending = TRUE;
  return class->flush_async (stream, io_priority, callback, data, notify);
}

typedef struct {
  GOutputStream       *stream;
  gboolean            result;
  GError             *error;
  GAsyncCloseOutputCallback callback;
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
queue_close_async_result (GOutputStream       *stream,
			  gboolean            result,
			  GError             *error,
			  GAsyncCloseOutputCallback callback,
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
  g_source_attach (source, g_output_stream_get_async_context (stream));
  g_source_unref (source);
}

/**
 * g_output_stream_close_async:
 * @stream: A #GOutputStream.
 * @callback: callback to call when the request is satisfied
 * @data: the data to pass to callback function
 * @notify: a function to call when @data is no longer in use, or %NULL.
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
g_output_stream_close_async (GOutputStream       *stream,
			     int                  io_priority,
			     GAsyncCloseOutputCallback callback,
			     gpointer            data,
			     GDestroyNotify      notify)
{
  GOutputStreamClass *class;
  GError *error;

  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));
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
  
  class = G_OUTPUT_STREAM_GET_CLASS (stream);
  stream->priv->pending = TRUE;
  return class->close_async (stream, io_priority, callback, data, notify);
}

/**
 * g_output_stream_cancel:
 * @stream: A #GOutputStream.
 *
 * Tries to cancel an outstanding request for the stream. If it
 * succeeds the outstanding request callback will be called with
 * %G_VFS_ERROR_CANCELLED.
 *
 * Generally if a request is cancelled before its callback has been
 * called the cancellation will succeed and the callback will only
 * be called with %G_VFS_ERROR_CANCELLED. However, if multiple threads
 * are in use this cannot be guaranteed, and the cancel may not result
 * in a %G_VFS_ERROR_CANCELLED callback.
 *
 * The asyncronous methods have a default fallback that uses threads to implement
 * asynchronicity, so they are optional for inheriting classes. However, if you
 * override one you must override all.
 **/
void
g_output_stream_cancel (GOutputStream   *stream)
{
  GOutputStreamClass *class;

  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);
  
  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  stream->priv->cancelled = TRUE;
  
  class->cancel (stream);
}


gboolean
g_output_stream_is_cancelled (GOutputStream *stream)
{
  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), TRUE);
  g_return_val_if_fail (stream != NULL, TRUE);
  
  return stream->priv->cancelled;
}

/********************************************
 *   Default implementation of async ops    *
 ********************************************/

typedef struct {
  GOutputStream      *stream;
  void              *buffer;
  gsize              count_requested;
  gssize             count_written;
  GError            *error;
  GAsyncWriteCallback callback;
  gpointer           data;
  GDestroyNotify     notify;
} WriteAsyncOp;

static void
write_op_report (gpointer data)
{
  WriteAsyncOp *op = data;

  op->stream->priv->pending = FALSE;

  op->callback (op->stream,
		op->buffer,
		op->count_requested,
		op->count_written,
		op->data,
		op->error);

}

static void
write_op_free (gpointer data)
{
  WriteAsyncOp *op = data;

  g_object_unref (op->stream);

  if (op->error)
    g_error_free (op->error);

  if (op->notify)
    op->notify (op->data);

  g_free (op);
}


static void
write_op_func (GIOJob *job,
	       gpointer data)
{
  WriteAsyncOp *op = data;
  GOutputStreamClass *class;

  if (g_io_job_is_cancelled (job))
    {
      op->count_written = -1;
      g_set_error (&op->error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
    }
  else
    {
      class = G_OUTPUT_STREAM_GET_CLASS (op->stream);
      op->count_written = class->write (op->stream, op->buffer, op->count_requested, &op->error);
    }

  g_io_job_mark_done (job);
  g_io_job_send_to_mainloop (job, write_op_report,
			     op, write_op_free,
			     FALSE);
}

static void
write_op_cancel (gpointer data)
{
  WriteAsyncOp *op = data;
  GOutputStreamClass *class;

  class = G_OUTPUT_STREAM_GET_CLASS (op->stream);
  if (class->cancel_sync)
    class->cancel_sync (op->stream);
}

static void
g_output_stream_real_write_async (GOutputStream       *stream,
				  void                *buffer,
				  gsize                count,
				  int                  io_priority,
				  GAsyncWriteCallback  callback,
				  gpointer             data,
				  GDestroyNotify       notify)
{
  WriteAsyncOp *op;

  op = g_new0 (WriteAsyncOp, 1);

  op->stream = g_object_ref (stream);
  op->buffer = buffer;
  op->count_requested = count;
  op->callback = callback;
  op->data = data;
  op->notify = notify;
  
  stream->priv->io_job_id = g_schedule_io_job (write_op_func,
					       write_op_cancel,
					       op,
					       NULL,
					       io_priority,
					       g_output_stream_get_async_context (stream));
}

typedef struct {
  GOutputStream      *stream;
  gboolean            result;
  GError             *error;
  GAsyncFlushCallback callback;
  gpointer            data;
  GDestroyNotify      notify;
} FlushAsyncOp;

static void
flush_op_report (gpointer data)
{
  FlushAsyncOp *op = data;

  op->stream->priv->pending = FALSE;

  op->callback (op->stream,
		op->result,
		op->data,
		op->error);

}

static void
flush_op_free (gpointer data)
{
  FlushAsyncOp *op = data;

  g_object_unref (op->stream);

  if (op->error)
    g_error_free (op->error);

  if (op->notify)
    op->notify (op->data);

  g_free (op);
}


static void
flush_op_func (GIOJob *job,
	      gpointer data)
{
  FlushAsyncOp *op = data;
  GOutputStreamClass *class;

  if (g_io_job_is_cancelled (job))
    {
      op->result = FALSE;
      g_set_error (&op->error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
    }
  else
    {
      class = G_OUTPUT_STREAM_GET_CLASS (op->stream);
      op->result = TRUE;
      if (class->flush)
	op->result = class->flush (op->stream, &op->error);
    }

  g_io_job_mark_done (job);
  g_io_job_send_to_mainloop (job, flush_op_report,
			     op, flush_op_free,
			     FALSE);
}

static void
flush_op_cancel (gpointer data)
{
  FlushAsyncOp *op = data;
  GOutputStreamClass *class;

  class = G_OUTPUT_STREAM_GET_CLASS (op->stream);
  if (class->cancel_sync)
    class->cancel_sync (op->stream);
}

static void
g_output_stream_real_flush_async (GOutputStream       *stream,
				  int                  io_priority,
				  GAsyncFlushCallback  callback,
				  gpointer             data,
				  GDestroyNotify       notify)
{
  FlushAsyncOp *op;

  op = g_new0 (FlushAsyncOp, 1);

  op->stream = g_object_ref (stream);
  op->callback = callback;
  op->data = data;
  op->notify = notify;
  
  stream->priv->io_job_id = g_schedule_io_job (flush_op_func,
					       flush_op_cancel,
					       op,
					       NULL,
					       io_priority,
					       g_output_stream_get_async_context (stream));
}

typedef struct {
  GOutputStream     *stream;
  gboolean           res;
  GError            *error;
  GAsyncCloseOutputCallback callback;
  gpointer           data;
  GDestroyNotify     notify;
} CloseAsyncOp;

static void
close_op_report (gpointer data)
{
  CloseAsyncOp *op = data;

  op->stream->priv->pending = FALSE;

  op->callback (op->stream,
		op->res,
		op->data,
		op->error);
}

static void
close_op_free (gpointer data)
{
  CloseAsyncOp *op = data;

  g_object_unref (op->stream);

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
  GOutputStreamClass *class;

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
      class = G_OUTPUT_STREAM_GET_CLASS (op->stream);
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
  GOutputStreamClass *class;

  class = G_OUTPUT_STREAM_GET_CLASS (op->stream);
  if (class->cancel_sync)
    class->cancel_sync (op->stream);
}

static void
g_output_stream_real_close_async (GOutputStream       *stream,
				  int                  io_priority,
				  GAsyncCloseOutputCallback callback,
				  gpointer            data,
				  GDestroyNotify      notify)
{
  CloseAsyncOp *op;

  op = g_new0 (CloseAsyncOp, 1);

  op->stream = g_object_ref (stream);
  op->callback = callback;
  op->data = data;
  op->notify = notify;
  
  stream->priv->io_job_id = g_schedule_io_job (close_op_func,
					       close_op_cancel,
					       op,
					       NULL,
					       io_priority,
					       g_output_stream_get_async_context (stream));
}

static void
g_output_stream_real_cancel (GOutputStream   *stream)
{
  g_cancel_io_job (stream->priv->io_job_id);
}

