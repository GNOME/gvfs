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
#include <gvfsreadchannel.h>
#include <gvfs/ginputstreamsocket.h>
#include <gvfs/goutputstreamsocket.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsdaemonutils.h>
#include <gvfsjobread.h>
#include <gvfsjobseekread.h>
#include <gvfsjobcloseread.h>

G_DEFINE_TYPE (GVfsReadChannel, g_vfs_read_channel, G_TYPE_OBJECT);

enum {
  PROP_0,
};

enum {
  NEW_JOB,
  CLOSED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

typedef struct
{
  GVfsReadChannel *read_channel;
  GInputStream *command_stream;
  char buffer[G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE];
  int buffer_size;
} RequestReader;

struct _GVfsReadChannelPrivate
{
  GVfsBackend *backend;
  gboolean connection_closed;
  GInputStream *command_stream;
  GOutputStream *reply_stream;
  int remote_fd;
  int seek_generation;
  
  GVfsBackendHandle backend_handle;
  GVfsJob *current_job;
  guint32 current_job_seq_nr;

  RequestReader *request_reader;
  
  char reply_buffer[G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE];
  int reply_buffer_pos;
  
  char *output_data; /* Owned by job */
  gsize output_data_size;
  gsize output_data_pos;
};

static void start_request_reader (GVfsReadChannel *channel);

static void
g_vfs_read_channel_finalize (GObject *object)
{
  GVfsReadChannel *read_channel;

  read_channel = G_VFS_READ_CHANNEL (object);

  if (read_channel->priv->current_job)
    g_object_unref (read_channel->priv->current_job);
  read_channel->priv->current_job = NULL;
  
  if (read_channel->priv->reply_stream)
    g_object_unref (read_channel->priv->reply_stream);
  read_channel->priv->reply_stream = NULL;

  if (read_channel->priv->request_reader)
    read_channel->priv->request_reader->read_channel = NULL;
  read_channel->priv->request_reader = NULL;

  if (read_channel->priv->command_stream)
    g_object_unref (read_channel->priv->command_stream);
  read_channel->priv->command_stream = NULL;
  
  if (read_channel->priv->remote_fd != -1)
    close (read_channel->priv->remote_fd);

  g_assert (read_channel->priv->backend_handle == NULL);
  
  if (G_OBJECT_CLASS (g_vfs_read_channel_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_read_channel_parent_class)->finalize) (object);
}

static void
g_vfs_read_channel_class_init (GVfsReadChannelClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GVfsReadChannelPrivate));
  
  gobject_class->finalize = g_vfs_read_channel_finalize;

  signals[NEW_JOB] =
    g_signal_new ("new_job",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GVfsReadChannelClass, new_job),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__POINTER,
		  G_TYPE_NONE, 1, G_TYPE_VFS_JOB);
  
  signals[CLOSED] =
    g_signal_new ("closed",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GVfsReadChannelClass, closed),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
  
}

static void
g_vfs_read_channel_init (GVfsReadChannel *channel)
{
  channel->priv = G_TYPE_INSTANCE_GET_PRIVATE (channel,
					       G_TYPE_VFS_READ_CHANNEL,
					       GVfsReadChannelPrivate);
  channel->priv->remote_fd = -1;
}

static void
g_vfs_read_channel_connection_closed (GVfsReadChannel *channel)
{
  if (channel->priv->connection_closed)
    return;
  channel->priv->connection_closed = TRUE;
  
  if (channel->priv->current_job == NULL &&
      channel->priv->backend_handle != NULL)
    {
      channel->priv->current_job = g_vfs_job_close_read_new (channel, channel->priv->backend_handle, channel->priv->backend);
      channel->priv->current_job_seq_nr = 0;
      g_signal_emit (channel, signals[NEW_JOB], 0, channel->priv->current_job);
    }
  /* Otherwise we'll close when current_job is finished */
}

static void
got_command (GVfsReadChannel *channel,
	     guint32 command,
	     guint32 seq_nr,
	     guint32 arg1,
	     guint32 arg2)
{
  GVfsJob *job;
  GError *error;
  GSeekType seek_type;

  g_print ("got_command %d %d %d %d\n", command, seq_nr, arg1, arg2);

  if (channel->priv->current_job != NULL)
    {
      if (command != G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL)
	{
	  g_warning ("Ignored non-cancel request with outstanding request");
	  return;
	}

      if (arg1 == channel->priv->current_job_seq_nr)
	g_vfs_job_cancel (channel->priv->current_job);
      return;
    }
  
  job = NULL;
  switch (command)
    {
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_READ:
      job = g_vfs_job_read_new (channel,
				channel->priv->backend_handle,
				arg1,
				channel->priv->backend);
      break;
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CLOSE:
      job = g_vfs_job_close_read_new (channel,
				      channel->priv->backend_handle,
				      channel->priv->backend);
      break;
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_CUR:
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_END:
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_SET:
      seek_type = G_SEEK_SET;
      if (command == G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_END)
	seek_type = G_SEEK_END;
      else if (command == G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_CUR)
	seek_type = G_SEEK_CUR;
      
      channel->priv->seek_generation++;
      job = g_vfs_job_seek_read_new (channel,
				     channel->priv->backend_handle,
				     seek_type,
				     ((goffset)arg1) | (((goffset)arg2) << 32),
				     channel->priv->backend);
      break;
      
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL:
      /* Ignore cancel with no outstanding job */
      break;
      
    default:
      error = NULL;
      g_set_error (&error, G_VFS_ERROR,
		   G_VFS_ERROR_INTERNAL_ERROR,
		   "Unknown stream command %d\n", command);
      g_vfs_read_channel_send_error (channel, error);
      g_error_free (error);
      break;
    }

  if (job)
    {
      channel->priv->current_job = job;
      channel->priv->current_job_seq_nr = seq_nr;
      g_signal_emit (channel, signals[NEW_JOB], 0, job);
    }
}

static void
command_read_cb (GInputStream *input_stream,
		 void         *buffer,
		 gsize         count_requested,
		 gssize        count_read,
		 gpointer      data,
		 GError       *error)
{
  RequestReader *reader = data;
  GVfsDaemonSocketProtocolRequest *cmd;
  guint32 seq_nr;
  guint32 command;
  guint32 arg1, arg2;

  if (reader->read_channel == NULL)
    {
      /* ReadChannel was finalized */
      g_object_unref (reader->command_stream);
      g_free (reader);
      return;
    }
  
  if (count_read <= 0)
    {
      reader->read_channel->priv->request_reader = NULL;
      g_vfs_read_channel_connection_closed (reader->read_channel);
      g_object_unref (reader->command_stream);
      g_free (reader);
      return;
    }

  reader->buffer_size += count_read;

  if (reader->buffer_size < G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE)
    {
      g_input_stream_read_async (reader->command_stream,
				 reader->buffer + reader->buffer_size,
				 G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE - reader->buffer_size,
				 0,
				 command_read_cb,
				 reader,
				 NULL);
      return;
    }

  cmd = (GVfsDaemonSocketProtocolRequest *)reader->buffer;
  command = g_ntohl (cmd->command);
  arg1 = g_ntohl (cmd->arg1);
  arg2 = g_ntohl (cmd->arg2);
  seq_nr = g_ntohl (cmd->seq_nr);
  reader->buffer_size = 0;

  got_command (reader->read_channel, command, seq_nr, arg1, arg2);
  
  /* Request more commands, so can get cancel requests */

  reader->buffer_size = 0;
  g_input_stream_read_async (reader->command_stream,
			     reader->buffer + reader->buffer_size,
			     G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE - reader->buffer_size,
			     0,
			     command_read_cb,
			     reader,
			     NULL);
}

static void
start_request_reader (GVfsReadChannel *channel)
{
  RequestReader *reader;

  reader = g_new0 (RequestReader, 1);
  reader->read_channel = channel;
  reader->command_stream = g_object_ref (channel->priv->command_stream);
  reader->buffer_size = 0;
  
  g_input_stream_read_async (reader->command_stream,
			     reader->buffer + reader->buffer_size,
			     G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE - reader->buffer_size,
			     0,
			     command_read_cb,
			     reader,
			     NULL);

  channel->priv->request_reader = reader;
}


static void
send_reply_cb (GOutputStream *output_stream,
	       void          *buffer,
	       gsize          bytes_requested,
	       gssize         bytes_written,
	       gpointer       data,
	       GError        *error)
{
  GVfsReadChannel *channel = data;
  GVfsJob *job;

  g_print ("send_reply_cb: %d\n", bytes_written);

  if (bytes_written <= 0)
    {
      g_vfs_read_channel_connection_closed (channel);
      goto error_out;
    }

  if (channel->priv->reply_buffer_pos < G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE)
    {
      channel->priv->reply_buffer_pos += bytes_written;

      /* Write more of reply header if needed */
      if (channel->priv->reply_buffer_pos < G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE)
	{
	  g_output_stream_write_async (channel->priv->reply_stream,
				       channel->priv->reply_buffer + channel->priv->reply_buffer_pos,
				       G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE - channel->priv->reply_buffer_pos,
				       0,
				       send_reply_cb, channel,
				       NULL);  
	  return;
	}
      bytes_written = 0;
    }

  channel->priv->output_data_pos += bytes_written;

  /* Write more of output_data if needed */
  if (channel->priv->output_data != NULL &&
      channel->priv->output_data_pos < channel->priv->output_data_size)
    {
      g_output_stream_write_async (channel->priv->reply_stream,
				   channel->priv->output_data + channel->priv->output_data_pos,
				   channel->priv->output_data_size - channel->priv->output_data_pos,
				   0,
				   send_reply_cb, channel,
				   NULL);
      return;
    }

 error_out:
  
  /* Sent full reply */
  channel->priv->output_data = NULL;

  job = channel->priv->current_job;
  channel->priv->current_job = NULL;
  g_vfs_job_emit_finished (job);

  if (G_IS_VFS_JOB_CLOSE_READ (job))
    {
      g_signal_emit (channel, signals[CLOSED], 0);
      channel->priv->backend_handle = NULL;
    }
  else if (channel->priv->connection_closed)
    {
      channel->priv->current_job = g_vfs_job_close_read_new (channel, channel->priv->backend_handle,
							    channel->priv->backend);
      channel->priv->current_job_seq_nr = 0;
      g_signal_emit (channel, signals[NEW_JOB], 0, channel->priv->current_job);
    }

  g_object_unref (job);
  g_print ("Sent reply\n");
}

/* Might be called on an i/o thread */
static void
send_reply (GVfsReadChannel *channel,
	    gboolean use_header,
	    char *data,
	    gsize data_len)
{
  
  channel->priv->output_data = data;
  channel->priv->output_data_size = data_len;
  channel->priv->output_data_pos = 0;

  if (use_header)
    {
      channel->priv->reply_buffer_pos = 0;

      g_output_stream_write_async (channel->priv->reply_stream,
				   channel->priv->reply_buffer,
				   G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE,
				   0,
				   send_reply_cb, channel,
				   NULL);  
    }
  else
    {
      channel->priv->reply_buffer_pos = G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE;

      g_output_stream_write_async (channel->priv->reply_stream,
				   channel->priv->output_data,
				   channel->priv->output_data_size,
				   0,
				   send_reply_cb, channel,
				   NULL);  
    }
}

/* Might be called on an i/o thread
 */
void
g_vfs_read_channel_send_error (GVfsReadChannel  *read_channel,
			      GError *error)
{
  char *data;
  gsize data_len;
  
  data = g_error_to_daemon_reply (error, read_channel->priv->current_job_seq_nr, &data_len);
  send_reply (read_channel, FALSE, data, data_len);
}


/* Might be called on an i/o thread
 */
void
g_vfs_read_channel_send_seek_offset (GVfsReadChannel *read_channel,
				    goffset offset)
{
  GVfsDaemonSocketProtocolReply *reply;
  
  reply = (GVfsDaemonSocketProtocolReply *)read_channel->priv->reply_buffer;
  reply->type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SEEK_POS);
  reply->seq_nr = g_htonl (read_channel->priv->current_job_seq_nr);
  reply->arg1 = g_htonl (offset & 0xffffffff);
  reply->arg2 = g_htonl (offset >> 32);

  send_reply (read_channel, TRUE, NULL, 0);
}

/* Might be called on an i/o thread
 */
void
g_vfs_read_channel_send_closed (GVfsReadChannel *read_channel)
{
  GVfsDaemonSocketProtocolReply *reply;
  
  reply = (GVfsDaemonSocketProtocolReply *)read_channel->priv->reply_buffer;
  reply->type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_CLOSED);
  reply->seq_nr = g_htonl (read_channel->priv->current_job_seq_nr);
  reply->arg1 = g_htonl (0);
  reply->arg2 = g_htonl (0);

  send_reply (read_channel, TRUE, NULL, 0);
}

/* Might be called on an i/o thread
 */
void
g_vfs_read_channel_send_data (GVfsReadChannel  *read_channel,
			      char            *buffer,
			      gsize            count)
{
  GVfsDaemonSocketProtocolReply *reply;

  reply = (GVfsDaemonSocketProtocolReply *)read_channel->priv->reply_buffer;
  reply->type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA);
  reply->seq_nr = g_htonl (read_channel->priv->current_job_seq_nr);
  reply->arg1 = g_htonl (count);
  reply->arg2 = g_htonl (read_channel->priv->seek_generation);

  send_reply (read_channel, TRUE, buffer, count);
}

GVfsReadChannel *
g_vfs_read_channel_new (GVfsBackend *backend,
			GError **error)
{
  GVfsReadChannel *channel;
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

  channel = g_object_new (G_TYPE_VFS_READ_CHANNEL, NULL);
  channel->priv->backend = backend;
  channel->priv->command_stream = g_input_stream_socket_new (socket_fds[0], TRUE);
  channel->priv->reply_stream = g_output_stream_socket_new (socket_fds[0], FALSE);
  channel->priv->remote_fd = socket_fds[1];

  start_request_reader (channel);
  
  return channel;
}

int
g_vfs_read_channel_steal_remote_fd (GVfsReadChannel *channel)
{
  int fd;
  fd = channel->priv->remote_fd;
  channel->priv->remote_fd = -1;
  return fd;
}

GVfsBackend *
g_vfs_read_channel_get_backend (GVfsReadChannel  *read_channel)
{
  return read_channel->priv->backend;
}

void
g_vfs_read_channel_set_backend_handle (GVfsReadChannel *read_channel,
				       GVfsBackendHandle backend_handle)
{
  read_channel->priv->backend_handle = backend_handle;
}
