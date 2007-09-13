#include <config.h>

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gi18n.h>
#include <dbus-gmain.h>
#include <gvfsreadstream.h>
#include <gvfs/gsocketinputstream.h>
#include <gvfs/gsocketoutputstream.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsjobread.h>

G_DEFINE_TYPE (GVfsReadStream, g_vfs_read_stream, G_TYPE_OBJECT);

enum {
  PROP_0,
};

enum {
  NEW_JOB,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

struct _GVfsReadStreamPrivate
{
  GInputStream *command_stream;
  GOutputStream *reply_stream;
  int remote_fd;
  
  gpointer data; /* user data, i.e. GVfsHandle */
  GVfsJob *job;
  
  char command_buffer[G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_SIZE];
  int command_buffer_size;

  char reply_buffer[G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE];
  int reply_buffer_pos;
  
  char *output_buffer;
  gssize output_buffer_size;
  gssize output_buffer_pos;
  
};

static void
g_vfs_read_stream_finalize (GObject *object)
{
  GVfsReadStream *read_stream;

  read_stream = G_VFS_READ_STREAM (object);
  
  if (read_stream->priv->reply_stream)
    g_object_unref (read_stream->priv->reply_stream);
  read_stream->priv->reply_stream = NULL;

  if (read_stream->priv->command_stream)
    g_object_unref (read_stream->priv->command_stream);
  read_stream->priv->command_stream = NULL;
  
  if (read_stream->priv->remote_fd != -1)
    close (read_stream->priv->remote_fd);
  
  if (G_OBJECT_CLASS (g_vfs_read_stream_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_read_stream_parent_class)->finalize) (object);
}

static void
g_vfs_read_stream_class_init (GVfsReadStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GVfsReadStreamPrivate));
  
  gobject_class->finalize = g_vfs_read_stream_finalize;

  signals[NEW_JOB] =
    g_signal_new ("new_job",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GVfsReadStreamClass, new_job),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__POINTER,
		  G_TYPE_NONE, 1, G_TYPE_VFS_JOB);
  
}

static void
g_vfs_read_stream_init (GVfsReadStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
					      G_TYPE_VFS_READ_STREAM,
					      GVfsReadStreamPrivate);
  stream->priv->remote_fd = -1;
}

static void
got_command (GVfsReadStream *stream,
	     guint32 command,
	     guint32 arg)
{
  GVfsJob *job;

  g_print ("got_command %d %d\n", command, arg);
  
  job = NULL;
  switch (command)
    {
    case G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_READ:
      job = g_vfs_job_read_new (stream,
				stream->priv->data,
				arg);
      break;
    case G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_SEEK:
    default:
      /* TODO */
      g_warning ("Ignoring unknown command %d\n", command);
      break;
    }

  if (job)
    g_signal_emit (stream, signals[NEW_JOB], 0, job);
}

static void
command_read_cb (GInputStream *input_stream,
		 void         *buffer,
		 gsize         count_requested,
		 gssize        count_read,
		 gpointer      data,
		 GError       *error)
{
  GVfsReadStream *stream = G_VFS_READ_STREAM (data);
  GVfsDaemonSocketProtocolCommand *cmd;
  guint32 command;
  guint32 arg;
  
  if (count_read <= 0)
    {
      /* TODO: Handle errors (eof..) */
      return;
    }

  g_print ("command_read_cb: %d\n", count_read);
  stream->priv->command_buffer_size += count_read;

  if (stream->priv->command_buffer_size < G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_SIZE)
    {
      g_input_stream_read_async (stream->priv->command_stream,
				 stream->priv->command_buffer + stream->priv->command_buffer_size,
				 G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_SIZE - stream->priv->command_buffer_size,
				 0,
				 command_read_cb,
				 stream,
				 NULL);
      return;
    }

  cmd = (GVfsDaemonSocketProtocolCommand *)stream->priv->command_buffer;
  command = g_ntohl (cmd->command);
  arg = g_ntohl (cmd->arg);
  stream->priv->command_buffer_size = 0;
  got_command (stream, command, arg);
}

static void
request_command (GVfsReadStream *stream)
{
  stream->priv->command_buffer_size = 0;
  g_input_stream_read_async (stream->priv->command_stream,
			     stream->priv->command_buffer + stream->priv->command_buffer_size,
			     G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_SIZE - stream->priv->command_buffer_size,
			     0,
			     command_read_cb,
			     stream,
			     NULL);
}

static void
data_sent_cb (GOutputStream *stream,
	      void          *buffer,
	      gsize          bytes_requested,
	      gssize         bytes_written,
	      gpointer       data,
	      GError        *error)
{
  GVfsReadStream  *read_stream = data;

  g_print ("data_sent_cb: %p %d\n", read_stream, bytes_written);

  if (bytes_written <= 0)
    {
      /* TODO: handle errors */
      g_assert_not_reached ();
      return;
    }

  read_stream->priv->output_buffer_pos += bytes_written;

  if (read_stream->priv->output_buffer_pos < read_stream->priv->output_buffer_size)
    {
      g_output_stream_write_async (read_stream->priv->reply_stream,
				   read_stream->priv->output_buffer + read_stream->priv->output_buffer_pos,
				   read_stream->priv->output_buffer_size - read_stream->priv->output_buffer_pos,
				   0,
				   data_sent_cb, read_stream,
				   NULL);  
      
    }
  else
    {
      g_print ("sent all data\n");
    }
  
}

static void
reply_sent_cb (GOutputStream *stream,
	       void          *buffer,
	       gsize          bytes_requested,
	       gssize         bytes_written,
	       gpointer       data,
	       GError        *error)
{
  GVfsReadStream  *read_stream = data;

  g_print ("reply_sent_cb: %p %d\n", read_stream, bytes_written);

  if (bytes_written <= 0)
    {
      /* TODO: handle errors */
      g_assert_not_reached ();
      return;
    }

  read_stream->priv->reply_buffer_pos += bytes_written;

  if (read_stream->priv->reply_buffer_pos < G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE)
    {
      g_output_stream_write_async (read_stream->priv->reply_stream,
				   read_stream->priv->reply_buffer + read_stream->priv->reply_buffer_pos,
				   G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE - read_stream->priv->reply_buffer_pos,
				   0,
				   reply_sent_cb, read_stream,
				   NULL);  
      
    }
  else
    {
      read_stream->priv->output_buffer_pos = 0;
      g_output_stream_write_async (read_stream->priv->reply_stream,
				   read_stream->priv->output_buffer + read_stream->priv->output_buffer_pos,
				   read_stream->priv->output_buffer_size - read_stream->priv->output_buffer_pos,
				   0,
				   data_sent_cb, read_stream,
				   NULL);  
    }
  
}

/* Might be called on an i/o thread
 */
void
g_vfs_read_stream_send_data (GVfsReadStream  *read_stream,
			     char            *buffer,
			     gssize           count)
{
  GVfsDaemonSocketProtocolReply *reply;
  
  read_stream->priv->output_buffer = buffer;
  read_stream->priv->output_buffer_size = count;

  reply = (GVfsDaemonSocketProtocolReply *)read_stream->priv->reply_buffer;
  reply->type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA);
  reply->arg1 = g_htonl (count);
  reply->arg2 = g_htonl (0); /* TODO: seek generation */

  read_stream->priv->reply_buffer_pos = G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE;

  g_output_stream_write_async (read_stream->priv->reply_stream,
			       read_stream->priv->reply_buffer,
			       G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE,
			       0,
			       reply_sent_cb, read_stream,
			       NULL);  
}


GVfsReadStream *
g_vfs_read_stream_new (GError **error)
{
  GVfsReadStream *stream;
  int socket_fds[2];
  int ret;

  ret = socketpair (AF_UNIX, SOCK_STREAM, 0, socket_fds);
  if (ret == -1) 
    {
      g_set_error (error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error creating socket pair"));
      return NULL;
    }

  stream = g_object_new (G_TYPE_VFS_READ_STREAM, NULL);
  stream->priv->command_stream = g_socket_input_stream_new (socket_fds[0], TRUE);
  stream->priv->reply_stream = g_socket_output_stream_new (socket_fds[0], FALSE);
  stream->priv->remote_fd = socket_fds[1];

  request_command (stream);
  
  return stream;
}

int
g_vfs_read_stream_steal_remote_fd (GVfsReadStream *stream)
{
  int fd;
  fd = stream->priv->remote_fd;
  stream->priv->remote_fd = -1;
  return fd;
}

void
g_vfs_read_stream_set_user_data (GVfsReadStream *read_stream,
				 gpointer data)
{
  read_stream->priv->data = data;
}
