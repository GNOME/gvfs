#include <config.h>
#include "goutputstream.h"
#include <glib/gi18n-lib.h>

G_DEFINE_TYPE (GOutputStream, g_output_stream, G_TYPE_OBJECT);

static GObjectClass *parent_class = NULL;

struct _GOutputStreamPrivate {
  /* TODO: Should be public for subclasses? */
  guint closed : 1;
  guint pending : 1;
  GMainContext *context;
};

static guint  g_output_stream_real_write_async (GOutputStream        *stream,
						void                 *buffer,
						gsize                 count,
						int                   io_priority,
						GAsyncWriteCallback    callback,
						gpointer              data,
						GDestroyNotify        notify);
static guint  g_output_stream_real_close_async (GOutputStream         *stream,
						GAsyncCloseCallback   callback,
						gpointer              data,
						GDestroyNotify        notify);
static void   g_output_stream_real_cancel      (GOutputStream         *stream,
					       guint                 tag);


static void
g_output_stream_finalize (GObject *object)
{
  GOutputStream *stream;

  stream = G_OUTPUT_STREAM (object);
  
  if (!stream->priv->closed)
    g_output_stream_close (stream, NULL);
  
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
  
  return class->write (stream, buffer, count, error);
}

/**
 * g_output_stream_flush:
 * @stream: a #GOutputStream.
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Flushed any outstanding buffers in the stream. Will block during the operation.
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

  if (class->flush)
    return class->flush (stream, error);
  else
    return TRUE;
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
  
  res = TRUE;
  
  if (class->close)
    res = class->close (stream, error);
  
  if (res)
    stream->priv->closed = TRUE;
  
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
 *
 * Return value: A tag that can be passed to g_output_stream_cancel()
 **/
guint
g_output_stream_write_async (GOutputStream        *stream,
			     void                *buffer,
			     gsize                count,
			     int                  io_priority,
			     GAsyncWriteCallback  callback,
			     gpointer             data,
			     GDestroyNotify       notify)
{
  GOutputStreamClass *class;

  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), 0);
  g_return_val_if_fail (stream != NULL, 0);
  g_return_val_if_fail (buffer != NULL, 0);
  
  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  return class->write_async (stream, buffer, count, io_priority, callback, data, notify);
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
 *
 * Return value: A tag that can be passed to g_output_stream_cancel()
 **/
guint
g_output_stream_close_async (GOutputStream       *stream,
			    GAsyncCloseCallback callback,
			    gpointer            data,
			    GDestroyNotify      notify)
{
  GOutputStreamClass *class;

  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), 0);
  g_return_val_if_fail (stream != NULL, 0);
  
  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  return class->close_async (stream, callback, data, notify);
}


/**
 * g_output_stream_cancel:
 * @stream: A #GOutputStream.
 * @tag: a value returned from an async request
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
g_output_stream_cancel (GOutputStream   *stream,
		       guint           tag)
{
  GOutputStreamClass *class;

  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);
  
  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  class->cancel (stream, tag);
}


static guint
g_output_stream_real_write_async (GOutputStream   *stream,
				  void                *buffer,
				  gsize                count,
				  int                  io_priority,
				  GAsyncWriteCallback   callback,
				  gpointer             data,
				  GDestroyNotify       notify)
{
  g_error ("TODO");
  return 0;
}

static guint
g_output_stream_real_close_async (GOutputStream       *stream,
				  GAsyncCloseCallback callback,
				  gpointer            data,
				  GDestroyNotify      notify)
{
  g_error ("TODO");
  return 0;
}

static void
g_output_stream_real_cancel (GOutputStream   *stream,
			     guint           tag)
{
  g_error ("TODO");
}

