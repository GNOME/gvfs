#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <poll.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include "gvfserror.h"
#include "gsocketoutputstream.h"
#include "gcancellable.h"

G_DEFINE_TYPE (GSocketOutputStream, g_socket_output_stream, G_TYPE_OUTPUT_STREAM);


struct _GSocketOutputStreamPrivate {
  int fd;
  gboolean close_fd_at_close;
};

typedef struct _StreamSource StreamSource;

struct _StreamSource
{
  GSource  source;
  GPollFD  pollfd;
  GSocketOutputStream *stream;
};

typedef void (*StreamSourceFunc) (GSocketOutputStream  *stream,
				  gpointer             data);

static gssize   g_socket_output_stream_write       (GOutputStream              *stream,
						    void                       *buffer,
						    gsize                       count,
						    GCancellable               *cancellable,
						    GError                    **error);
static gboolean g_socket_output_stream_close       (GOutputStream              *stream,
						    GCancellable               *cancellable,
						    GError                    **error);
static void     g_socket_output_stream_write_async (GOutputStream              *stream,
						    void                       *buffer,
						    gsize                       count,
						    int                         io_priority,
						    GAsyncWriteCallback         callback,
						    gpointer                    data,
						    GDestroyNotify              notify);
static void     g_socket_output_stream_flush_async (GOutputStream              *stream,
						    int                         io_priority,
						    GAsyncFlushCallback         callback,
						    gpointer                    data,
						    GDestroyNotify              notify);
static void     g_socket_output_stream_close_async (GOutputStream              *stream,
						    int                         io_priority,
						    GAsyncCloseOutputCallback   callback,
						    gpointer                    data,
						    GDestroyNotify              notify);
static void     g_socket_output_stream_cancel      (GOutputStream              *stream);


static gboolean stream_source_prepare  (GSource     *source,
					gint        *timeout);
static gboolean stream_source_check    (GSource     *source);
static gboolean stream_source_dispatch (GSource     *source,
					GSourceFunc  callback,
					gpointer     user_data);
static void     stream_source_finalize (GSource     *source);

static GSourceFuncs stream_source_funcs = {
  stream_source_prepare,
  stream_source_check,
  stream_source_dispatch,
  stream_source_finalize
};

static gboolean 
stream_source_prepare (GSource  *source,
		       gint     *timeout)
{
  StreamSource *stream_source = (StreamSource *)source;
  *timeout = -1;
  
  return g_output_stream_is_cancelled (G_OUTPUT_STREAM (stream_source->stream));
}

static gboolean 
stream_source_check (GSource  *source)
{
  StreamSource *stream_source = (StreamSource *)source;

  return
    g_output_stream_is_cancelled (G_OUTPUT_STREAM (stream_source->stream)) ||
    stream_source->pollfd.revents != 0;
}

static gboolean
stream_source_dispatch (GSource     *source,
			GSourceFunc  callback,
			gpointer     user_data)

{
  StreamSourceFunc func = (StreamSourceFunc)callback;
  StreamSource *stream_source = (StreamSource *)source;

  g_assert (func != NULL);

  (*func) (stream_source->stream,
	   user_data);
  return FALSE;
}

static void 
stream_source_finalize (GSource *source)
{
  StreamSource *stream_source = (StreamSource *)source;

  g_object_unref (stream_source->stream);
}

static GSource *
create_stream_source (GSocketOutputStream *stream)
{
  GSource *source;
  StreamSource *stream_source;

  source = g_source_new (&stream_source_funcs, sizeof (StreamSource));
  stream_source = (StreamSource *)source;

  stream_source->stream = g_object_ref (stream);
  stream_source->pollfd.fd = stream->priv->fd;
  stream_source->pollfd.events = POLLOUT;
  g_source_add_poll (source, &stream_source->pollfd);

  return source;
}

static void
g_socket_output_stream_finalize (GObject *object)
{
  GSocketOutputStream *stream;
  
  stream = G_SOCKET_OUTPUT_STREAM (object);

  if (G_OBJECT_CLASS (g_socket_output_stream_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_socket_output_stream_parent_class)->finalize) (object);
}

static void
g_socket_output_stream_class_init (GSocketOutputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GOutputStreamClass *stream_class = G_OUTPUT_STREAM_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GSocketOutputStreamPrivate));
  
  gobject_class->finalize = g_socket_output_stream_finalize;

  stream_class->write = g_socket_output_stream_write;
  stream_class->close = g_socket_output_stream_close;
  stream_class->write_async = g_socket_output_stream_write_async;
  stream_class->flush_async = g_socket_output_stream_flush_async;
  stream_class->close_async = g_socket_output_stream_close_async;
  stream_class->cancel = g_socket_output_stream_cancel;
}

static void
g_socket_output_stream_init (GSocketOutputStream *socket)
{
  socket->priv = G_TYPE_INSTANCE_GET_PRIVATE (socket,
					      G_TYPE_SOCKET_OUTPUT_STREAM,
					      GSocketOutputStreamPrivate);
}

GOutputStream *
g_socket_output_stream_new (int fd,
			   gboolean close_fd_at_close)
{
  GSocketOutputStream *stream;

  stream = g_object_new (G_TYPE_SOCKET_OUTPUT_STREAM, NULL);

  stream->priv->fd = fd;
  stream->priv->close_fd_at_close = close_fd_at_close;
  
  return G_OUTPUT_STREAM (stream);
}

static gssize
g_socket_output_stream_write (GOutputStream *stream,
			      void          *buffer,
			      gsize          count,
			      GCancellable  *cancellable,
			      GError       **error)
{
  GSocketOutputStream *socket_stream;
  gssize res;
  struct pollfd poll_fds[2];
  int poll_ret;
  int cancel_fd;

  socket_stream = G_SOCKET_OUTPUT_STREAM (stream);

  cancel_fd = g_cancellable_get_fd (cancellable);
  if (cancel_fd != -1)
    {
      do
	{
	  poll_fds[0].events = POLLOUT;
	  poll_fds[0].fd = socket_stream->priv->fd;
	  poll_fds[1].events = POLLIN;
	  poll_fds[1].fd = cancel_fd;
	  poll_ret = poll (poll_fds, 2, -1);
	}
      while (poll_ret == -1 && errno == EINTR);
      
      if (poll_ret == -1)
	{
	  g_set_error (error, G_FILE_ERROR,
		       g_file_error_from_errno (errno),
		       _("Error writing to socket: %s"),
		       g_strerror (errno));
	  return -1;
	}
      
      if (poll_fds[1].revents)
	{
	  g_set_error (error,
		       G_VFS_ERROR,
		       G_VFS_ERROR_CANCELLED,
		       _("Operation was cancelled"));
	  return -1;
	}
    }
      
  while (1)
    {
      res = write (socket_stream->priv->fd, buffer, count);
      if (res == -1)
	{
	  if (g_cancellable_is_cancelled (cancellable))
	    {
	      g_set_error (error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      break;
	    }
	  
	  if (errno == EINTR)
	    continue;
	  
	  g_set_error (error, G_FILE_ERROR,
		       g_file_error_from_errno (errno),
		       _("Error writing to socket: %s"),
		       g_strerror (errno));
	}
      
      break;
    }
  
  return res;
}

static gboolean
g_socket_output_stream_close (GOutputStream *stream,
			      GCancellable  *cancellable,
			      GError       **error)
{
  GSocketOutputStream *socket_stream;
  int res;

  socket_stream = G_SOCKET_OUTPUT_STREAM (stream);

  if (!socket_stream->priv->close_fd_at_close)
    return TRUE;
  
  while (1)
    {
      /* This might block during the close. Doesn't seem to be a way to avoid it though. */
      res = close (socket_stream->priv->fd);
      if (res == -1)
	{
	  if (g_cancellable_is_cancelled (cancellable))
	    {
	      g_set_error (error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      break;
	    }
	  
	  if (errno == EINTR)
	    continue;
	  
	  g_set_error (error, G_FILE_ERROR,
		       g_file_error_from_errno (errno),
		       _("Error closing socket: %s"),
		       g_strerror (errno));
	}
      break;
    }

  return res != -1;
}

typedef struct {
  gsize count;
  void *buffer;
  GAsyncWriteCallback callback;
  gpointer user_data;
  GDestroyNotify notify;
} WriteAsyncData;

static void
write_async_cb (GSocketOutputStream  *stream,
		gpointer             _data)
{
  GSocketOutputStream *socket_stream;
  WriteAsyncData *data = _data;
  GError *error = NULL;
  gssize count_written;

  socket_stream = G_SOCKET_OUTPUT_STREAM (stream);
  
  if (g_output_stream_is_cancelled (G_OUTPUT_STREAM (stream)))
    {
      g_set_error (&error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      count_written = -1;
      goto out;
    }

  while (1)
    {
      count_written = write (socket_stream->priv->fd, data->buffer, data->count);
      if (count_written == -1)
	{
	  if (g_output_stream_is_cancelled (G_OUTPUT_STREAM (stream)))
	    {
	      g_set_error (&error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      break;
	    }
	  
	  if (errno == EINTR)
	    continue;
	  
	  g_set_error (&error, G_FILE_ERROR,
		       g_file_error_from_errno (errno),
		       _("Error reading from socket: %s"),
		       g_strerror (errno));
	}
      break;
    }

 out:
  data->callback (G_OUTPUT_STREAM (stream),
		  data->buffer,
		  data->count,
		  count_written,
		  data->user_data,
		  error);
}

static void
write_async_data_free (gpointer _data)
{
  WriteAsyncData *data = _data;

  if (data->notify)
    data->notify (data->user_data);
  
  g_free (data);
}

static void
g_socket_output_stream_write_async (GOutputStream      *stream,
				    void               *buffer,
				    gsize               count,
				    int                 io_priority,
				    GAsyncWriteCallback callback,
				    gpointer            user_data,
				    GDestroyNotify      notify)
{
  GSource *source;
  GSocketOutputStream *socket_stream;
  WriteAsyncData *data;

  socket_stream = G_SOCKET_OUTPUT_STREAM (stream);

  data = g_new0 (WriteAsyncData, 1);
  data->count = count;
  data->buffer = buffer;
  data->callback = callback;
  data->user_data = user_data;
  data->notify = notify;
  
  source = create_stream_source (socket_stream);
  g_source_set_callback (source, (GSourceFunc)write_async_cb, data, write_async_data_free);
  
  g_source_attach (source, g_output_stream_get_async_context (stream));
  g_source_unref (source);
}

static void
g_socket_output_stream_flush_async (GOutputStream        *stream,
				    int                  io_priority,
				    GAsyncFlushCallback  callback,
				    gpointer             data,
				    GDestroyNotify       notify)
{
  g_assert_not_reached ();
  /* TODO: Not implemented */
}

typedef struct {
  GOutputStream *stream;
  GAsyncCloseOutputCallback callback;
  gpointer user_data;
  GDestroyNotify notify;
} CloseAsyncData;

static void
close_async_data_free (gpointer _data)
{
  CloseAsyncData *data = _data;

  if (data->notify)
    data->notify (data->user_data);
  
  g_free (data);
}

static gboolean
close_async_cb (CloseAsyncData *data)
{
  GSocketOutputStream *socket_stream;
  GError *error = NULL;
  gboolean result;
  int res;

  socket_stream = G_SOCKET_OUTPUT_STREAM (data->stream);

  if (g_output_stream_is_cancelled (data->stream))
    {
      g_set_error (&error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      result = FALSE;
      goto out;
    }

  if (!socket_stream->priv->close_fd_at_close)
    {
      result = TRUE;
      goto out;
    }
  
  while (1)
    {
      res = close (socket_stream->priv->fd);
      if (res == -1)
	{
	  if (g_output_stream_is_cancelled (data->stream))
	    {
	      g_set_error (&error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      
	      break;
	    }
	  
	  if (errno == EINTR)
	    continue;
	  
	  g_set_error (&error, G_FILE_ERROR,
		       g_file_error_from_errno (errno),
		       _("Error closing socket: %s"),
		       g_strerror (errno));
	}
      break;
    }
  
  result = res != -1;
  
 out:
  data->callback (data->stream,
		  result,
		  data->user_data,
		  error);
  
  return FALSE;
}

static void
g_socket_output_stream_close_async (GOutputStream       *stream,
				    int                 io_priority,
				    GAsyncCloseOutputCallback callback,
				    gpointer            user_data,
				    GDestroyNotify      notify)
{
  GSource *idle;
  CloseAsyncData *data;

  data = g_new0 (CloseAsyncData, 1);

  data->stream = stream;
  data->callback = callback;
  data->user_data = user_data;
  data->notify = notify;
  
  idle = g_idle_source_new ();
  g_source_set_callback (idle, (GSourceFunc)close_async_cb, data, close_async_data_free);
  g_source_attach (idle, g_output_stream_get_async_context (stream));
  g_source_unref (idle);
}

static void
g_socket_output_stream_cancel (GOutputStream *stream)
{
  GSocketOutputStream *socket_stream;

  socket_stream = G_SOCKET_OUTPUT_STREAM (stream);
  
  /* Wake up the mainloop in case we're waiting on async calls with StreamSource */
  g_main_context_wakeup (g_output_stream_get_async_context (stream));
}
