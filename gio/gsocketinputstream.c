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
#include "gioerror.h"
#include "gsocketinputstream.h"
#include "gcancellable.h"
#include "gasynchelper.h"

G_DEFINE_TYPE (GSocketInputStream, g_socket_input_stream, G_TYPE_INPUT_STREAM);

struct _GSocketInputStreamPrivate {
  int fd;
  gboolean close_fd_at_close;
};

static gssize   g_socket_input_stream_read        (GInputStream              *stream,
						   void                      *buffer,
						   gsize                      count,
						   GCancellable              *cancellable,
						   GError                   **error);
static gboolean g_socket_input_stream_close       (GInputStream              *stream,
						   GCancellable              *cancellable,
						   GError                   **error);
static void     g_socket_input_stream_read_async  (GInputStream              *stream,
						   void                      *buffer,
						   gsize                      count,
						   int                        io_priority,
						   GAsyncReadCallback         callback,
						   gpointer                   data,
						   GCancellable              *cancellable);
static void     g_socket_input_stream_skip_async  (GInputStream              *stream,
						   gsize                      count,
						   int                        io_priority,
						   GAsyncSkipCallback         callback,
						   gpointer                   data,
						   GCancellable              *cancellable);
static void     g_socket_input_stream_close_async (GInputStream              *stream,
						   int                        io_priority,
						   GAsyncCloseInputCallback   callback,
						   gpointer                   data,
						   GCancellable              *cancellable);

static void
g_socket_input_stream_finalize (GObject *object)
{
  GSocketInputStream *stream;
  
  stream = G_SOCKET_INPUT_STREAM (object);

  if (G_OBJECT_CLASS (g_socket_input_stream_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_socket_input_stream_parent_class)->finalize) (object);
}

static void
g_socket_input_stream_class_init (GSocketInputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GSocketInputStreamPrivate));
  
  gobject_class->finalize = g_socket_input_stream_finalize;

  stream_class->read = g_socket_input_stream_read;
  stream_class->close = g_socket_input_stream_close;
  stream_class->read_async = g_socket_input_stream_read_async;
  stream_class->skip_async = g_socket_input_stream_skip_async;
  stream_class->close_async = g_socket_input_stream_close_async;
}

static void
g_socket_input_stream_init (GSocketInputStream *socket)
{
  socket->priv = G_TYPE_INSTANCE_GET_PRIVATE (socket,
					      G_TYPE_SOCKET_INPUT_STREAM,
					      GSocketInputStreamPrivate);
}

GInputStream *
g_socket_input_stream_new (int fd,
			   gboolean close_fd_at_close)
{
  GSocketInputStream *stream;

  stream = g_object_new (G_TYPE_SOCKET_INPUT_STREAM, NULL);

  stream->priv->fd = fd;
  stream->priv->close_fd_at_close = close_fd_at_close;
  
  return G_INPUT_STREAM (stream);
}

static gssize
g_socket_input_stream_read (GInputStream *stream,
			    void         *buffer,
			    gsize         count,
			    GCancellable *cancellable,
			    GError      **error)
{
  GSocketInputStream *socket_stream;
  gssize res;
  struct pollfd poll_fds[2];
  int poll_ret;
  int cancel_fd;

  socket_stream = G_SOCKET_INPUT_STREAM (stream);

  cancel_fd = g_cancellable_get_fd (cancellable);
  if (cancel_fd != -1)
    {
      do
	{
	  poll_fds[0].events = POLLIN;
	  poll_fds[0].fd = socket_stream->priv->fd;
	  poll_fds[1].events = POLLIN;
	  poll_fds[1].fd = cancel_fd;
	  poll_ret = poll (poll_fds, 2, -1);
	}
      while (poll_ret == -1 && errno == EINTR);
      
      if (poll_ret == -1)
	{
	  g_set_error (error, G_IO_ERROR,
		       g_io_error_from_errno (errno),
		       _("Error reading from socket: %s"),
		       g_strerror (errno));
	  return -1;
	}
    }

  while (1)
    {
      if (g_cancellable_is_cancelled (cancellable))
	{
	  g_set_error (error,
		       G_IO_ERROR,
		       G_IO_ERROR_CANCELLED,
		       _("Operation was cancelled"));
	  break;
	}
      res = read (socket_stream->priv->fd, buffer, count);
      if (res == -1)
	{
	  if (errno == EINTR)
	    continue;
	  
	  g_set_error (error, G_IO_ERROR,
		       g_io_error_from_errno (errno),
		       _("Error reading from socket: %s"),
		       g_strerror (errno));
	}
      
      break;
    }

  return res;
}

static gboolean
g_socket_input_stream_close (GInputStream *stream,
			     GCancellable *cancellable,
			     GError      **error)
{
  GSocketInputStream *socket_stream;
  int res;

  socket_stream = G_SOCKET_INPUT_STREAM (stream);

  if (!socket_stream->priv->close_fd_at_close)
    return TRUE;
  
  while (1)
    {
      /* This might block during the close. Doesn't seem to be a way to avoid it though. */
      res = close (socket_stream->priv->fd);
      if (res == -1)
	{
	  g_set_error (error, G_IO_ERROR,
		       g_io_error_from_errno (errno),
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
  GAsyncReadCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  GSocketInputStream *stream;
} ReadAsyncData;

static gboolean
read_async_cb (ReadAsyncData *data,
	       GIOCondition condition,
	       int fd)
{
  GError *error = NULL;
  gssize count_read;

  /* We know that we can read from fd once without blocking */
  while (1)
    {
      if (g_cancellable_is_cancelled (data->cancellable))
	{
	  g_set_error (&error,
		       G_IO_ERROR,
		       G_IO_ERROR_CANCELLED,
		       _("Operation was cancelled"));
	  count_read = -1;
	  break;
	}
      count_read = read (data->stream->priv->fd, data->buffer, data->count);
      if (count_read == -1)
	{
	  if (errno == EINTR)
	    continue;
	  
	  g_set_error (&error, G_IO_ERROR,
		       g_io_error_from_errno (errno),
		       _("Error reading from socket: %s"),
		       g_strerror (errno));
	}
      break;
    }

  data->callback (G_INPUT_STREAM (data->stream),
		  data->buffer,
		  data->count,
		  count_read,
		  data->user_data,
		  error);
  if (error)
    g_error_free (error);

  return FALSE;
}

static void
read_async_data_free (gpointer _data)
{
  ReadAsyncData *data = _data;
  
  g_free (data);
}

static void
g_socket_input_stream_read_async (GInputStream        *stream,
				  void                *buffer,
				  gsize                count,
				  int                  io_priority,
				  GAsyncReadCallback   callback,
				  gpointer             user_data,
				  GCancellable        *cancellable)
{
  GSource *source;
  GSocketInputStream *socket_stream;
  ReadAsyncData *data;

  socket_stream = G_SOCKET_INPUT_STREAM (stream);

  data = g_new0 (ReadAsyncData, 1);
  data->count = count;
  data->buffer = buffer;
  data->callback = callback;
  data->user_data = user_data;
  data->cancellable = cancellable;
  data->stream = socket_stream;

  source = _g_fd_source_new (socket_stream->priv->fd,
			     POLLIN,
			     cancellable);
  
  g_source_set_callback (source, (GSourceFunc)read_async_cb, data, read_async_data_free);
  g_source_attach (source, NULL);
 
  g_source_unref (source);
}

static void
g_socket_input_stream_skip_async (GInputStream        *stream,
				  gsize                count,
				  int                  io_priority,
				  GAsyncSkipCallback   callback,
				  gpointer             data,
				  GCancellable        *cancellable)
{
  g_assert_not_reached ();
  /* TODO: Not implemented */
}

typedef struct {
  GInputStream *stream;
  GAsyncCloseInputCallback callback;
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
  GSocketInputStream *socket_stream;
  GError *error = NULL;
  gboolean result;
  int res;

  socket_stream = G_SOCKET_INPUT_STREAM (data->stream);

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
	  g_set_error (&error, G_IO_ERROR,
		       g_io_error_from_errno (errno),
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
g_socket_input_stream_close_async (GInputStream       *stream,
				   int                 io_priority,
				   GAsyncCloseInputCallback callback,
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
  g_source_attach (idle, NULL);
  g_source_unref (idle);
}
