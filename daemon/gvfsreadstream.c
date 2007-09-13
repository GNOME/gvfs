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
#include <gvfs/ginputstreamsocket.h>
#include <gvfs/goutputstreamsocket.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsdaemonutils.h>
#include <gvfsjobread.h>
#include <gvfsjobseekread.h>
#include <gvfsjobcloseread.h>

G_DEFINE_TYPE (GVfsReadStream, g_vfs_read_stream, G_TYPE_OBJECT);

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
  GVfsReadStream *read_stream;
  GInputStream *command_stream;
  char buffer[G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE];
  int buffer_size;
} RequestReader;

struct _GVfsReadStreamPrivate
{
  gboolean connection_closed;
  GInputStream *command_stream;
  GOutputStream *reply_stream;
  int remote_fd;
  int seek_generation;
  
  gpointer data; /* user data, i.e. GVfsHandle */
  GVfsJob *current_job;
  guint32 current_job_seq_nr;

  RequestReader *request_reader;
  
  char reply_buffer[G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE];
  int reply_buffer_pos;
  
  char *output_data; /* Owned by job */
  gsize output_data_size;
  gsize output_data_pos;
};

static void start_request_reader (GVfsReadStream *stream);

static void
g_vfs_read_stream_finalize (GObject *object)
{
  GVfsReadStream *read_stream;

  read_stream = G_VFS_READ_STREAM (object);

  if (read_stream->priv->current_job)
    g_object_unref (read_stream->priv->current_job);
  read_stream->priv->current_job = NULL;
  
  if (read_stream->priv->reply_stream)
    g_object_unref (read_stream->priv->reply_stream);
  read_stream->priv->reply_stream = NULL;

  if (read_stream->priv->request_reader)
    read_stream->priv->request_reader->read_stream = NULL;
  read_stream->priv->request_reader = NULL;

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
  
  signals[CLOSED] =
    g_signal_new ("closed",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GVfsReadStreamClass, closed),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
  
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
g_vfs_read_stream_connection_closed (GVfsReadStream *stream)
{
  if (stream->priv->connection_closed)
    return;
  stream->priv->connection_closed = TRUE;
  
  if (stream->priv->current_job == NULL)
    {
      stream->priv->current_job = g_vfs_job_close_read_new (stream, stream->priv->data);
      stream->priv->current_job_seq_nr = 0;
      g_signal_emit (stream, signals[NEW_JOB], 0, stream->priv->current_job);
    }
  /* Otherwise we'll close when current_job is finished */
}

static void
got_command (GVfsReadStream *stream,
	     guint32 command,
	     guint32 seq_nr,
	     guint32 arg1,
	     guint32 arg2)
{
  GVfsJob *job;
  GError *error;
  GSeekType seek_type;

  g_print ("got_command %d %d %d %d\n", command, seq_nr, arg1, arg2);

  if (stream->priv->current_job != NULL)
    {
      if (command != G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL)
	{
	  g_warning ("Ignored non-cancel request with outstanding request");
	  return;
	}

      if (arg1 == stream->priv->current_job_seq_nr)
	g_vfs_job_cancel (stream->priv->current_job);
      return;
    }
  
  job = NULL;
  switch (command)
    {
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_READ:
      job = g_vfs_job_read_new (stream,
				stream->priv->data,
				arg1);
      break;
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CLOSE:
      job = g_vfs_job_close_read_new (stream,
				      stream->priv->data);
      break;
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_CUR:
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_END:
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_SET:
      seek_type = G_SEEK_SET;
      if (command == G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_END)
	seek_type = G_SEEK_END;
      else if (command == G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_CUR)
	seek_type = G_SEEK_CUR;
      
      stream->priv->seek_generation++;
      job = g_vfs_job_seek_read_new (stream,
				     stream->priv->data,
				     seek_type,
				     ((goffset)arg1) | (((goffset)arg2) << 32));
      break;
      
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL:
      /* Ignore cancel with no outstanding job */
      break;
      
    default:
      error = NULL;
      g_set_error (&error, G_VFS_ERROR,
		   G_VFS_ERROR_INTERNAL_ERROR,
		   "Unknown stream command %d\n", command);
      g_vfs_read_stream_send_error (stream, error);
      g_error_free (error);
      break;
    }

  if (job)
    {
      stream->priv->current_job = job;
      stream->priv->current_job_seq_nr = seq_nr;
      g_signal_emit (stream, signals[NEW_JOB], 0, job);
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

  if (reader->read_stream == NULL)
    {
      /* ReadStream was finalized */
      g_object_unref (reader->command_stream);
      g_free (reader);
      return;
    }
  
  if (count_read <= 0)
    {
      reader->read_stream->priv->request_reader = NULL;
      g_vfs_read_stream_connection_closed (reader->read_stream);
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

  got_command (reader->read_stream, command, seq_nr, arg1, arg2);
  
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
start_request_reader (GVfsReadStream *stream)
{
  RequestReader *reader;

  reader = g_new0 (RequestReader, 1);
  reader->read_stream = stream;
  reader->command_stream = g_object_ref (stream->priv->command_stream);
  reader->buffer_size = 0;
  
  g_input_stream_read_async (reader->command_stream,
			     reader->buffer + reader->buffer_size,
			     G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE - reader->buffer_size,
			     0,
			     command_read_cb,
			     reader,
			     NULL);

  stream->priv->request_reader = reader;
}


static void
send_reply_cb (GOutputStream *output_stream,
	       void          *buffer,
	       gsize          bytes_requested,
	       gssize         bytes_written,
	       gpointer       data,
	       GError        *error)
{
  GVfsReadStream *stream = data;
  GVfsJob *job;

  g_print ("send_reply_cb: %d\n", bytes_written);

  if (bytes_written <= 0)
    {
      g_vfs_read_stream_connection_closed (stream);
      goto error_out;
    }

  if (stream->priv->reply_buffer_pos < G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE)
    {
      stream->priv->reply_buffer_pos += bytes_written;

      /* Write more of reply header if needed */
      if (stream->priv->reply_buffer_pos < G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE)
	{
	  g_output_stream_write_async (stream->priv->reply_stream,
				       stream->priv->reply_buffer + stream->priv->reply_buffer_pos,
				       G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE - stream->priv->reply_buffer_pos,
				       0,
				       send_reply_cb, stream,
				       NULL);  
	  return;
	}
      bytes_written = 0;
    }

  stream->priv->output_data_pos += bytes_written;

  /* Write more of output_data if needed */
  if (stream->priv->output_data != NULL &&
      stream->priv->output_data_pos < stream->priv->output_data_size)
    {
      g_output_stream_write_async (stream->priv->reply_stream,
				   stream->priv->output_data + stream->priv->output_data_pos,
				   stream->priv->output_data_size - stream->priv->output_data_pos,
				   0,
				   send_reply_cb, stream,
				   NULL);
      return;
    }

 error_out:
  
  /* Sent full reply */
  stream->priv->output_data = NULL;

  job = stream->priv->current_job;
  stream->priv->current_job = NULL;
  g_vfs_job_emit_finished (job);

  if (G_IS_VFS_JOB_CLOSE_READ (job))
    g_signal_emit (stream, signals[CLOSED], 0);
  else if (stream->priv->connection_closed)
    {
      stream->priv->current_job = g_vfs_job_close_read_new (stream, stream->priv->data);
      stream->priv->current_job_seq_nr = 0;
      g_signal_emit (stream, signals[NEW_JOB], 0, stream->priv->current_job);
    }

  g_object_unref (job);
  g_print ("Sent reply\n");
}

/* Might be called on an i/o thread */
static void
send_reply (GVfsReadStream *stream,
	    gboolean use_header,
	    char *data,
	    gsize data_len)
{
  
  stream->priv->output_data = data;
  stream->priv->output_data_size = data_len;
  stream->priv->output_data_pos = 0;

  if (use_header)
    {
      stream->priv->reply_buffer_pos = 0;

      g_output_stream_write_async (stream->priv->reply_stream,
				   stream->priv->reply_buffer,
				   G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE,
				   0,
				   send_reply_cb, stream,
				   NULL);  
    }
  else
    {
      stream->priv->reply_buffer_pos = G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE;

      g_output_stream_write_async (stream->priv->reply_stream,
				   stream->priv->output_data,
				   stream->priv->output_data_size,
				   0,
				   send_reply_cb, stream,
				   NULL);  
    }
}

/* Might be called on an i/o thread
 */
void
g_vfs_read_stream_send_error (GVfsReadStream  *read_stream,
			      GError *error)
{
  char *data;
  gsize data_len;
  
  data = g_error_to_daemon_reply (error, read_stream->priv->current_job_seq_nr, &data_len);
  send_reply (read_stream, FALSE, data, data_len);
}


/* Might be called on an i/o thread
 */
void
g_vfs_read_stream_send_seek_offset (GVfsReadStream *read_stream,
				    goffset offset)
{
  GVfsDaemonSocketProtocolReply *reply;
  
  reply = (GVfsDaemonSocketProtocolReply *)read_stream->priv->reply_buffer;
  reply->type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SEEK_POS);
  reply->seq_nr = g_htonl (read_stream->priv->current_job_seq_nr);
  reply->arg1 = g_htonl (offset & 0xffffffff);
  reply->arg2 = g_htonl (offset >> 32);

  send_reply (read_stream, TRUE, NULL, 0);
}

/* Might be called on an i/o thread
 */
void
g_vfs_read_stream_send_closed (GVfsReadStream *read_stream)
{
  GVfsDaemonSocketProtocolReply *reply;
  
  reply = (GVfsDaemonSocketProtocolReply *)read_stream->priv->reply_buffer;
  reply->type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_CLOSED);
  reply->seq_nr = g_htonl (read_stream->priv->current_job_seq_nr);
  reply->arg1 = g_htonl (0);
  reply->arg2 = g_htonl (0);

  send_reply (read_stream, TRUE, NULL, 0);
}

/* Might be called on an i/o thread
 */
void
g_vfs_read_stream_send_data (GVfsReadStream  *read_stream,
			     char            *buffer,
			     gsize            count)
{
  GVfsDaemonSocketProtocolReply *reply;

  reply = (GVfsDaemonSocketProtocolReply *)read_stream->priv->reply_buffer;
  reply->type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA);
  reply->seq_nr = g_htonl (read_stream->priv->current_job_seq_nr);
  reply->arg1 = g_htonl (count);
  reply->arg2 = g_htonl (read_stream->priv->seek_generation);

  send_reply (read_stream, TRUE, buffer, count);
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
  stream->priv->command_stream = g_input_stream_socket_new (socket_fds[0], TRUE);
  stream->priv->reply_stream = g_output_stream_socket_new (socket_fds[0], FALSE);
  stream->priv->remote_fd = socket_fds[1];

  start_request_reader (stream);
  
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
