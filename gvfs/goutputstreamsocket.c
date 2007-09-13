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
#include "goutputstreamsocket.h"
#include "gcancellable.h"
#include "gasynchelper.h"

G_DEFINE_TYPE (GOutputStreamSocket, g_output_stream_socket, G_TYPE_OUTPUT_STREAM);


struct _GOutputStreamSocketPrivate {
  int fd;
  gboolean close_fd_at_close;
};

static gssize   g_output_stream_socket_write       (GOutputStream              *stream,
						    void                       *buffer,
						    gsize                       count,
						    GCancellable               *cancellable,
						    GError                    **error);
static gboolean g_output_stream_socket_close       (GOutputStream              *stream,
						    GCancellable               *cancellable,
						    GError                    **error);
static void     g_output_stream_socket_write_async (GOutputStream              *stream,
						    void                       *buffer,
						    gsize                       count,
						    int                         io_priority,
						    GAsyncWriteCallback         callback,
						    gpointer                    data,
						    GCancellable               *cancellable);
static void     g_output_stream_socket_flush_async (GOutputStream              *stream,
						    int                         io_priority,
						    GAsyncFlushCallback         callback,
						    gpointer                    data,
						    GCancellable               *cancellable);
static void     g_output_stream_socket_close_async (GOutputStream              *stream,
						    int                         io_priority,
						    GAsyncCloseOutputCallback   callback,
						    gpointer                    data,
						    GCancellable               *cancellable);

static void
g_output_stream_socket_finalize (GObject *object)
{
  GOutputStreamSocket *stream;
  
  stream = G_OUTPUT_STREAM_SOCKET (object);

  if (G_OBJECT_CLASS (g_output_stream_socket_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_output_stream_socket_parent_class)->finalize) (object);
}

static void
g_output_stream_socket_class_init (GOutputStreamSocketClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GOutputStreamClass *stream_class = G_OUTPUT_STREAM_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GOutputStreamSocketPrivate));
  
  gobject_class->finalize = g_output_stream_socket_finalize;

  stream_class->write = g_output_stream_socket_write;
  stream_class->close = g_output_stream_socket_close;
  stream_class->write_async = g_output_stream_socket_write_async;
  stream_class->flush_async = g_output_stream_socket_flush_async;
  stream_class->close_async = g_output_stream_socket_close_async;
}

static void
g_output_stream_socket_init (GOutputStreamSocket *socket)
{
  socket->priv = G_TYPE_INSTANCE_GET_PRIVATE (socket,
					      G_TYPE_OUTPUT_STREAM_SOCKET,
					      GOutputStreamSocketPrivate);
}

GOutputStream *
g_output_stream_socket_new (int fd,
			   gboolean close_fd_at_close)
{
  GOutputStreamSocket *stream;

  stream = g_object_new (G_TYPE_OUTPUT_STREAM_SOCKET, NULL);

  stream->priv->fd = fd;
  stream->priv->close_fd_at_close = close_fd_at_close;
  
  return G_OUTPUT_STREAM (stream);
}

static gssize
g_output_stream_socket_write (GOutputStream *stream,
			      void          *buffer,
			      gsize          count,
			      GCancellable  *cancellable,
			      GError       **error)
{
  GOutputStreamSocket *socket_stream;
  gssize res;
  struct pollfd poll_fds[2];
  int poll_ret;
  int cancel_fd;

  socket_stream = G_OUTPUT_STREAM_SOCKET (stream);

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
    }
      
  while (1)
    {
      if (g_cancellable_is_cancelled (cancellable))
	{
	  g_set_error (error,
		       G_VFS_ERROR,
		       G_VFS_ERROR_CANCELLED,
		       _("Operation was cancelled"));
	  break;
	}
      res = write (socket_stream->priv->fd, buffer, count);
      if (res == -1)
	{
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
g_output_stream_socket_close (GOutputStream *stream,
			      GCancellable  *cancellable,
			      GError       **error)
{
  GOutputStreamSocket *socket_stream;
  int res;

  socket_stream = G_OUTPUT_STREAM_SOCKET (stream);

  if (!socket_stream->priv->close_fd_at_close)
    return TRUE;
  
  while (1)
    {
      /* This might block during the close. Doesn't seem to be a way to avoid it though. */
      res = close (socket_stream->priv->fd);
      if (res == -1)
	{
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
  GCancellable *cancellable;
  GOutputStreamSocket *stream;
} WriteAsyncData;

static void
write_async_cb (WriteAsyncData *data,
		GIOCondition condition,
		int fd)
{
  GError *error = NULL;
  gssize count_written;

  while (1)
    {
      if (g_cancellable_is_cancelled (data->cancellable))
	{
	  g_set_error (&error,
		       G_VFS_ERROR,
		       G_VFS_ERROR_CANCELLED,
		       _("Operation was cancelled"));
	  count_written = -1;
	  break;
	}
      
      count_written = write (data->stream->priv->fd, data->buffer, data->count);
      if (count_written == -1)
	{
	  if (errno == EINTR)
	    continue;
	  
	  g_set_error (&error, G_FILE_ERROR,
		       g_file_error_from_errno (errno),
		       _("Error reading from socket: %s"),
		       g_strerror (errno));
	}
      break;
    }

  data->callback (G_OUTPUT_STREAM (data->stream),
		  data->buffer,
		  data->count,
		  count_written,
		  data->user_data,
		  error);
  if (error)
    g_error_free (error);
}

static void
write_async_data_free (gpointer _data)
{
  WriteAsyncData *data = _data;

  g_free (data);
}

static void
g_output_stream_socket_write_async (GOutputStream      *stream,
				    void               *buffer,
				    gsize               count,
				    int                 io_priority,
				    GAsyncWriteCallback callback,
				    gpointer            user_data,
				    GCancellable       *cancellable)
{
  GSource *source;
  GOutputStreamSocket *socket_stream;
  WriteAsyncData *data;

  socket_stream = G_OUTPUT_STREAM_SOCKET (stream);

  data = g_new0 (WriteAsyncData, 1);
  data->count = count;
  data->buffer = buffer;
  data->callback = callback;
  data->user_data = user_data;
  data->cancellable = cancellable;
  data->stream = socket_stream;

  source = _g_fd_source_new (socket_stream->priv->fd,
			     POLLOUT,
			     g_output_stream_get_async_context (stream),
			     cancellable);
  
  g_source_set_callback (source, (GSourceFunc)write_async_cb, data, write_async_data_free);
  
  g_source_unref (source);
}

static void
g_output_stream_socket_flush_async (GOutputStream        *stream,
				    int                  io_priority,
				    GAsyncFlushCallback  callback,
				    gpointer             data,
				    GCancellable        *cancellable)
{
  g_assert_not_reached ();
  /* TODO: Not implemented */
}

typedef struct {
  GOutputStream *stream;
  GAsyncCloseOutputCallback callback;
  gpointer user_data;
} CloseAsyncData;

static void
close_async_data_free (gpointer _data)
{
  CloseAsyncData *data = _data;

  g_free (data);
}

static gboolean
close_async_cb (CloseAsyncData *data)
{
  GOutputStreamSocket *socket_stream;
  GError *error = NULL;
  gboolean result;
  int res;

  socket_stream = G_OUTPUT_STREAM_SOCKET (data->stream);

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
  if (error)
    g_error_free (error);
  
  return FALSE;
}

static void
g_output_stream_socket_close_async (GOutputStream       *stream,
				    int                 io_priority,
				    GAsyncCloseOutputCallback callback,
				    gpointer            user_data,
				    GCancellable       *cancellable)
{
  GSource *idle;
  CloseAsyncData *data;

  data = g_new0 (CloseAsyncData, 1);

  data->stream = stream;
  data->callback = callback;
  data->user_data = user_data;
  
  idle = g_idle_source_new ();
  g_source_set_callback (idle, (GSourceFunc)close_async_cb, data, close_async_data_free);
  g_source_attach (idle, g_output_stream_get_async_context (stream));
  g_source_unref (idle);
}
