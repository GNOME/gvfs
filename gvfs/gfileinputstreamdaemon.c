#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include "gvfserror.h"
#include "gseekable.h"
#include "gfileinputstreamdaemon.h"
#include "gvfsdaemondbus.h"
#include "gfileinfolocal.h"
#include "ginputstreamsocket.h"
#include "goutputstreamsocket.h"
#include <daemon/gvfsdaemonprotocol.h>

#define MAX_READ_SIZE (4*1024*1024)

typedef enum {
  READ_STATE_INIT = 0,
  READ_STATE_WROTE_COMMAND,
  READ_STATE_HANDLE_INPUT,
  READ_STATE_HANDLE_INPUT_BLOCK,
  READ_STATE_SKIP_BLOCK,
  READ_STATE_HANDLE_HEADER,
  READ_STATE_READ_BLOCK,
} ReadState;

typedef enum {
  SEEK_STATE_INIT = 0,
  SEEK_STATE_WROTE_REQUEST,
  SEEK_STATE_HANDLE_INPUT,
  SEEK_STATE_HANDLE_INPUT_BLOCK,
  SEEK_STATE_SKIP_BLOCK,
  SEEK_STATE_HANDLE_HEADER,
} SeekState;


typedef enum {
  INPUT_STATE_IN_REPLY_HEADER,
  INPUT_STATE_IN_BLOCK,
} InputState;


typedef enum {
  STATE_OP_DONE,
  STATE_OP_READ,
  STATE_OP_WRITE,
  STATE_OP_SKIP,
} StateOp;

typedef struct {
  ReadState state;

  /* Input */
  char *buffer;
  gsize buffer_size;
  /* Output */
  gssize ret_val;
  GError *ret_error;
  
  gboolean sent_cancel;
  
  guint32 seq_nr;
} ReadOperation;

typedef struct {
  SeekState state;

  /* Input */
  goffset offset;
  GSeekType seek_type;
  /* Output */
  gboolean ret_val;
  GError *ret_error;
  goffset ret_offset;
  
  gboolean sent_cancel;
  gboolean sent_seek;
  
  guint32 seq_nr;
} SeekOperation;

typedef struct {
  gboolean cancelled;
  
  char *io_buffer;
  gsize io_size;
  gsize io_res;
  /* The operation always succeeds, or gets cancelled.
     If we get an error doing the i/o that is considered fatal */
  gboolean io_allow_cancel;
  gboolean io_cancelled;
} IOOperationData;

typedef StateOp (*state_machine_iterator) (GFileInputStreamDaemon *file, IOOperationData *io_op, gpointer data);

struct _GFileInputStreamDaemonPrivate {
  char *filename;
  char *mountpoint;
  GOutputStream *command_stream;
  GInputStream *data_stream;
  int fd;
  int seek_generation;
  guint32 seq_nr;
  goffset current_offset;

  guint can_seek : 1;
  
  InputState input_state;
  gsize input_block_size;
  int input_block_seek_generation;

  GString *input_buffer;
  GString *output_buffer;
};

static gssize     g_file_input_stream_daemon_read          (GInputStream           *stream,
							    void                   *buffer,
							    gsize                   count,
							    GCancellable           *cancellable,
							    GError                **error);
static gssize     g_file_input_stream_daemon_skip          (GInputStream           *stream,
							    gsize                   count,
							    GCancellable           *cancellable,
							    GError                **error);
static gboolean   g_file_input_stream_daemon_close         (GInputStream           *stream,
							    GCancellable           *cancellable,
							    GError                **error);
static GFileInfo *g_file_input_stream_daemon_get_file_info (GFileInputStream       *stream,
							    GFileInfoRequestFlags   requested,
							    char                   *attributes,
							    GCancellable           *cancellable,
							    GError                **error);
static goffset    g_file_input_stream_daemon_tell          (GFileInputStream       *stream);
static gboolean   g_file_input_stream_daemon_can_seek      (GFileInputStream       *stream);
static gboolean   g_file_input_stream_daemon_seek          (GFileInputStream       *stream,
							    goffset                 offset,
							    GSeekType               type,
							    GCancellable           *cancellable,
							    GError                **error);

G_DEFINE_TYPE (GFileInputStreamDaemon, g_file_input_stream_daemon,
	       G_TYPE_FILE_INPUT_STREAM)

static void
g_file_input_stream_daemon_finalize (GObject *object)
{
  GFileInputStreamDaemon *file;
  
  file = G_FILE_INPUT_STREAM_DAEMON (object);

  if (file->priv->command_stream)
    g_object_unref (file->priv->command_stream);
  if (file->priv->data_stream)
    g_object_unref (file->priv->data_stream);
  
  g_free (file->priv->filename);
  g_free (file->priv->mountpoint);
  
  if (G_OBJECT_CLASS (g_file_input_stream_daemon_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_input_stream_daemon_parent_class)->finalize) (object);
}

static void
g_file_input_stream_daemon_class_init (GFileInputStreamDaemonClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);
  GFileInputStreamClass *file_stream_class = G_FILE_INPUT_STREAM_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GFileInputStreamDaemonPrivate));
  
  gobject_class->finalize = g_file_input_stream_daemon_finalize;

  stream_class->read = g_file_input_stream_daemon_read;
  stream_class->skip = g_file_input_stream_daemon_skip;
  stream_class->close = g_file_input_stream_daemon_close;
  file_stream_class->tell = g_file_input_stream_daemon_tell;
  file_stream_class->can_seek = g_file_input_stream_daemon_can_seek;
  file_stream_class->seek = g_file_input_stream_daemon_seek;
  file_stream_class->get_file_info = g_file_input_stream_daemon_get_file_info;
}

static void
g_file_input_stream_daemon_init (GFileInputStreamDaemon *info)
{
  info->priv = G_TYPE_INSTANCE_GET_PRIVATE (info,
					    G_TYPE_FILE_INPUT_STREAM_DAEMON,
					    GFileInputStreamDaemonPrivate);

  info->priv->output_buffer = g_string_new ("");
  info->priv->input_buffer = g_string_new ("");
}

GFileInputStream *
g_file_input_stream_daemon_new (const char *filename,
			      const char *mountpoint)
{
  GFileInputStreamDaemon *stream;

  stream = g_object_new (G_TYPE_FILE_INPUT_STREAM_DAEMON, NULL);

  stream->priv->filename = g_strdup (filename);
  stream->priv->mountpoint = g_strdup (mountpoint);
  stream->priv->fd = -1;
  
  return G_FILE_INPUT_STREAM (stream);
}

/* receive a file descriptor over file descriptor fd */
static int 
receive_fd (int connection_fd)
{
  struct msghdr msg;
  struct iovec iov;
  char buf[1];
  int rv;
  char ccmsg[CMSG_SPACE (sizeof(int))];
  struct cmsghdr *cmsg;

  iov.iov_base = buf;
  iov.iov_len = 1;
  msg.msg_name = 0;
  msg.msg_namelen = 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ccmsg;
  msg.msg_controllen = sizeof (ccmsg);
  
  rv = recvmsg (connection_fd, &msg, 0);
  if (rv == -1) 
    {
      perror ("recvmsg");
      return -1;
    }

  cmsg = CMSG_FIRSTHDR (&msg);
  if (!cmsg->cmsg_type == SCM_RIGHTS) {
    g_warning("got control message of unknown type %d", 
	      cmsg->cmsg_type);
    return -1;
  }

  return *(int*)CMSG_DATA(cmsg);
}

static gboolean
g_file_input_stream_daemon_open (GFileInputStreamDaemon *file,
				 GError **error)
{
  DBusConnection *connection;
  DBusError derror;
  int extra_fd;
  DBusMessage *message, *reply;
  DBusMessageIter iter;
  guint32 fd_id;
  dbus_bool_t can_seek;
  

  if (file->priv->fd != -1)
    return TRUE;

  connection = _g_vfs_daemon_get_connection_sync (file->priv->mountpoint, &extra_fd, error);
  if (connection == NULL)
    return FALSE;

  message = dbus_message_new_method_call ("org.gtk.vfs.Daemon",
					  G_VFS_DBUS_DAEMON_PATH,
					  G_VFS_DBUS_DAEMON_INTERFACE,
					  G_VFS_DBUS_OP_OPEN_FOR_READ);

  
  dbus_message_iter_init_append (message, &iter);
  if (!_g_dbus_message_iter_append_filename (&iter, file->priv->filename))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOMEM,
		   _("Out of memory"));
      return FALSE;
    }
      
  dbus_error_init (&derror);
  reply = dbus_connection_send_with_reply_and_block (connection, message, -1,
						     &derror);
  dbus_message_unref (message);
  if (!reply)
    {
      _g_error_from_dbus (&derror, error);
      dbus_error_free (&derror);
      return FALSE;
    }

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Error in stream protocol: %s"), _("Invalid return value from open"));
      return FALSE;
    }
  
  dbus_message_unref (reply);
      
  /* TODO: verify fd id */
  file->priv->fd = receive_fd (extra_fd);
  file->priv->can_seek = can_seek;
  
  file->priv->command_stream = g_output_stream_socket_new (file->priv->fd, FALSE);
  file->priv->data_stream = g_input_stream_socket_new (file->priv->fd, TRUE);
  
  return TRUE;
}

static gboolean
error_is_cancel (GError *error)
{
  return error != NULL &&
    error->domain == G_VFS_ERROR &&
    error->code == G_VFS_ERROR_CANCELLED;
}

static void
append_request (GFileInputStreamDaemon *stream, guint32 command,
		guint32 arg1, guint32 arg2, guint32 *seq_nr)
{
  GVfsDaemonSocketProtocolRequest cmd;

  g_assert (sizeof (cmd) == G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE);
  
  if (seq_nr)
    *seq_nr = stream->priv->seq_nr;
  
  cmd.command = g_htonl (command);
  cmd.seq_nr = g_htonl (stream->priv->seq_nr++);
  cmd.arg1 = g_htonl (arg1);
  cmd.arg2 = g_htonl (arg2);

  g_string_append_len (stream->priv->output_buffer,
		       (char *)&cmd, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE);
}

static gsize
get_reply_header_missing_bytes (GString *buffer)
{
  GVfsDaemonSocketProtocolReply *reply;
  guint32 type;
  guint32 arg2;
  
  if (buffer->len < G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE)
    return G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE - buffer->len;

  reply = (GVfsDaemonSocketProtocolReply *)buffer->str;

  type = g_ntohl (reply->type);
  arg2 = g_ntohl (reply->arg2);
  
  if (type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR)
    return G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE + arg2 - buffer->len;
  return 0;
}

static char *
decode_reply (GString *buffer, GVfsDaemonSocketProtocolReply *reply_out)
{
  GVfsDaemonSocketProtocolReply *reply;
  reply = (GVfsDaemonSocketProtocolReply *)buffer->str;
  reply_out->type = g_ntohl (reply->type);
  reply_out->seq_nr = g_ntohl (reply->seq_nr);
  reply_out->arg1 = g_ntohl (reply->arg1);
  reply_out->arg2 = g_ntohl (reply->arg2);
  
  return buffer->str + G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE;
}

static void
decode_error (GVfsDaemonSocketProtocolReply *reply, char *data, GError **error)
{
  g_set_error (error,
	       g_quark_from_string (data),
	       reply->arg1,
	       data + strlen (data) + 1);
}


static gboolean
run_sync_state_machine (GFileInputStreamDaemon *file,
			state_machine_iterator iterator,
			gpointer data,
			GCancellable *cancellable,
			GError **error)
{
  gssize res;
  StateOp io_op;
  IOOperationData io_data;
  GError *io_error;

  memset (&io_data, 0, sizeof (io_data));
  
  while (TRUE)
    {
      if (cancellable)
	io_data.cancelled = g_cancellable_is_cancelled (cancellable);
      
      io_op = iterator (file, &io_data, data);

      if (io_op == STATE_OP_DONE)
	return TRUE;
      
      io_error = NULL;
      if (io_op == STATE_OP_READ)
	{
	  res = g_input_stream_read (file->priv->data_stream,
				     io_data.io_buffer, io_data.io_size,
				     io_data.io_allow_cancel ? cancellable : NULL,
				     &io_error);
	}
      else if (io_op == STATE_OP_SKIP)
	{
	  res = g_input_stream_skip (file->priv->data_stream,
				     io_data.io_size,
				     io_data.io_allow_cancel ? cancellable : NULL,
				     &io_error);
	}
      else if (io_op == STATE_OP_WRITE)
	{
	  res = g_output_stream_write (file->priv->command_stream,
				       io_data.io_buffer, io_data.io_size,
				       io_data.io_allow_cancel ? cancellable : NULL,
				       &io_error);
	}
      else
	g_assert_not_reached ();
      
      if (res == -1)
	{
	  if (error_is_cancel (io_error))
	    {
	      io_data.io_res = 0;
	      io_data.io_cancelled = TRUE;
	      g_error_free (io_error);
	    }
	  else
	    {
	      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
			   _("Error in stream protocol: %s"), io_error->message);
	      g_error_free (io_error);
	      return FALSE;
	    }
	}
      else
	{
	  io_data.io_res = res;
	  io_data.io_cancelled = FALSE;
	}
    }
  while (io_op != STATE_OP_DONE);
}

/* read cycle:

   if we know of a (partially read) matching outstanding block, read from it
   create packet, append to outgoing
   flush outgoing
   start processing input, looking for a data block with same seek gen,
    or an error with same seq nr
   on cancel, send cancel command and go back to loop
 */

static StateOp
iterate_read_state_machine (GFileInputStreamDaemon *file, IOOperationData *io_op, ReadOperation *op)
{
  GFileInputStreamDaemonPrivate *priv = file->priv;
  gsize len;

  while (TRUE)
    {
      switch (op->state)
	{
	  /* Initial state for read op */
	case READ_STATE_INIT:
	  /* If we're already reading some data, but we didn't read all, just use that
	     and don't even send a request */
	  if (priv->input_state == INPUT_STATE_IN_BLOCK &&
	      priv->seek_generation == priv->input_block_seek_generation)
	    {
	      op->state = READ_STATE_READ_BLOCK;
	      io_op->io_buffer = op->buffer;
	      io_op->io_size = MIN (op->buffer_size, priv->input_block_size);
	      io_op->io_allow_cancel = TRUE; /* Allow cancel before we sent request */
	      return STATE_OP_READ;
	    }

	  append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_READ,
			  op->buffer_size, 0, &op->seq_nr);
	  op->state = READ_STATE_WROTE_COMMAND;
	  io_op->io_buffer = priv->output_buffer->str;
	  io_op->io_size = priv->output_buffer->len;
	  io_op->io_allow_cancel = TRUE; /* Allow cancel before first byte of request sent */
	  return STATE_OP_WRITE;

	  /* wrote parts of output_buffer */
	case READ_STATE_WROTE_COMMAND:
	  if (io_op->io_cancelled)
	    {
	      op->ret_val = -1;
	      g_set_error (&op->ret_error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      return STATE_OP_DONE;
	    }
	  
	  if (io_op->io_res < priv->output_buffer->len)
	    {
	      memcpy (priv->output_buffer->str,
		      priv->output_buffer->str + io_op->io_res,
		      priv->output_buffer->len - io_op->io_res);
	      g_string_truncate (priv->output_buffer,
				 priv->output_buffer->len - io_op->io_res);
	      io_op->io_buffer = priv->output_buffer->str;
	      io_op->io_size = priv->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }
	  g_string_truncate (priv->output_buffer, 0);

	  op->state = READ_STATE_HANDLE_INPUT;
	  break;

	  /* No op */
	case READ_STATE_HANDLE_INPUT:
	  if (io_op->cancelled && !op->sent_cancel)
	    {
	      op->sent_cancel = TRUE;
	      append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL,
			      op->seq_nr, 0, NULL);
	      op->state = READ_STATE_WROTE_COMMAND;
	      io_op->io_buffer = priv->output_buffer->str;
	      io_op->io_size = priv->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }
	  
	  if (priv->input_state == INPUT_STATE_IN_BLOCK)
	    {
	      op->state = READ_STATE_HANDLE_INPUT_BLOCK;
	      break;
	    }
	  else if (priv->input_state == INPUT_STATE_IN_REPLY_HEADER)
	    {
	      op->state = READ_STATE_HANDLE_HEADER;
	      break;
	    }
	  g_assert_not_reached ();
	  break;

	  /* No op */
	case READ_STATE_HANDLE_INPUT_BLOCK:
	  g_assert (priv->input_state == INPUT_STATE_IN_BLOCK);
	  
	  if (priv->seek_generation ==
	      priv->input_block_seek_generation)
	    {
	      op->state = READ_STATE_READ_BLOCK;
	      io_op->io_buffer = op->buffer;
	      io_op->io_size = MIN (op->buffer_size, priv->input_block_size);
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_READ;
	    }
	  else
	    {
	      op->state = READ_STATE_SKIP_BLOCK;
	      /* Reuse client buffer for skipping */
	      io_op->io_buffer = NULL;
	      io_op->io_size = priv->input_block_size;
	      io_op->io_allow_cancel = !op->sent_cancel;
	      return STATE_OP_SKIP;
	    }
	  break;

	  /* Read block data */
	case READ_STATE_SKIP_BLOCK:
	  if (io_op->io_cancelled)
	    {
	      op->state = READ_STATE_HANDLE_INPUT;
	      break;
	    }
	  
	  g_assert (io_op->io_res <= priv->input_block_size);
	  priv->input_block_size -= io_op->io_res;
	  
	  if (priv->input_block_size == 0)
	    priv->input_state = INPUT_STATE_IN_REPLY_HEADER;
	  
	  op->state = READ_STATE_HANDLE_INPUT;
	  break;
	  
	  /* read header data, (or manual io_len/res = 0) */
	case READ_STATE_HANDLE_HEADER:
	  if (io_op->io_cancelled)
	    {
	      op->state = READ_STATE_HANDLE_INPUT;
	      break;
	    }

	  if (io_op->io_res > 0)
	    {
	      gsize unread_size = io_op->io_size - io_op->io_res;
	      g_string_set_size (priv->input_buffer,
				 priv->input_buffer->len - unread_size);
	    }
	  
	  len = get_reply_header_missing_bytes (priv->input_buffer);
	  if (len > 0)
	    {
	      gsize current_len = priv->input_buffer->len;
	      g_string_set_size (priv->input_buffer,
				 current_len + len);
	      io_op->io_buffer = priv->input_buffer->str + current_len;
	      io_op->io_size = len;
	      io_op->io_allow_cancel = !op->sent_cancel;
	      return STATE_OP_READ;
	    }

	  /* Got full header */

	  {
	    GVfsDaemonSocketProtocolReply reply;
	    char *data;
	    data = decode_reply (priv->input_buffer, &reply);

	    if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR &&
		reply.seq_nr == op->seq_nr)
	      {
		op->ret_val = -1;
		decode_error (&reply, data, &op->ret_error);
		g_string_truncate (priv->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA)
	      {
		g_string_truncate (priv->input_buffer, 0);
		priv->input_state = INPUT_STATE_IN_BLOCK;
		priv->input_block_size = reply.arg1;
		priv->input_block_seek_generation = reply.arg2;
		op->state = READ_STATE_HANDLE_INPUT_BLOCK;
		break;
	      }
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SEEK_POS)
	      {
		/* Ignore when reading */
	      }
	    else
	      g_assert_not_reached ();
	  }

	  g_string_truncate (priv->input_buffer, 0);
	  
	  /* This wasn't interesting, read next reply */
	  op->state = READ_STATE_HANDLE_HEADER;
	  break;

	  /* Read block data */
	case READ_STATE_READ_BLOCK:
	  if (io_op->io_cancelled)
	    {
	      op->ret_val = -1;
	      g_set_error (&op->ret_error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      return STATE_OP_DONE;
	    }
	  
	  if (io_op->io_res > 0)
	    {
	      g_assert (io_op->io_res <= priv->input_block_size);
	      priv->input_block_size -= io_op->io_res;
	      if (priv->input_block_size == 0)
		priv->input_state = INPUT_STATE_IN_REPLY_HEADER;
	    }
	  
	  op->ret_val = io_op->io_res;
	  op->ret_error = NULL;
	  return STATE_OP_DONE;
	  
	default:
	  g_assert_not_reached ();
	}
      
      /* Clear io_op between non-op state switches */
      io_op->io_size = 0;
      io_op->io_res = 0;
      io_op->io_cancelled = FALSE;
 
    }
}

static gssize
g_file_input_stream_daemon_read (GInputStream *stream,
				 void         *buffer,
				 gsize         count,
				 GCancellable *cancellable,
				 GError      **error)
{
  GFileInputStreamDaemon *file;
  ReadOperation op;

  file = G_FILE_INPUT_STREAM_DAEMON (stream);

  if (!g_file_input_stream_daemon_open (file, error))
    return -1;

  /* Limit for sanity and to avoid 32bit overflow */
  if (count > MAX_READ_SIZE)
    count = MAX_READ_SIZE;

  memset (&op, 0, sizeof (op));
  op.state = READ_STATE_INIT;
  op.buffer = buffer;
  op.buffer_size = count;
  
  if (!run_sync_state_machine (file, (state_machine_iterator)iterate_read_state_machine,
			       &op, cancellable, error))
    return -1; /* IO Error */

  if (op.ret_val == -1)
    g_propagate_error (error, op.ret_error);
  else
    file->priv->current_offset += op.ret_val;
  
  return op.ret_val;
}

static gssize
g_file_input_stream_daemon_skip (GInputStream *stream,
				 gsize         count,
				 GCancellable *cancellable,
				 GError      **error)
{
  GFileInputStreamDaemon *file;

  file = G_FILE_INPUT_STREAM_DAEMON (stream);
  
  if (!g_file_input_stream_daemon_open (file, error))
    return -1;

  /* TODO: .. */
  g_assert_not_reached ();
  
  return 0;
}

static gboolean
g_file_input_stream_daemon_close (GInputStream *stream,
				  GCancellable *cancellable,
				  GError      **error)
{
  GFileInputStreamDaemon *file;
  GError *my_error;

  file = G_FILE_INPUT_STREAM_DAEMON (stream);

  if (file->priv->fd == -1)
    return TRUE;

  my_error = NULL;
  if (!g_output_stream_close (file->priv->command_stream, cancellable, error))
    {
      g_input_stream_close (file->priv->data_stream, cancellable, NULL);
      return FALSE;
    }
  
  return g_input_stream_close (file->priv->data_stream, NULL, error);
}

static goffset
g_file_input_stream_daemon_tell (GFileInputStream *stream)
{
  GFileInputStreamDaemon *file;

  file = G_FILE_INPUT_STREAM_DAEMON (stream);
  
  return file->priv->current_offset;
}

static gboolean
g_file_input_stream_daemon_can_seek (GFileInputStream *stream)
{
  GFileInputStreamDaemon *file;

  file = G_FILE_INPUT_STREAM_DAEMON (stream);

  if (!g_file_input_stream_daemon_open (file, NULL))
    return FALSE;

  return file->priv->can_seek;
}

static StateOp
iterate_seek_state_machine (GFileInputStreamDaemon *file, IOOperationData *io_op, SeekOperation *op)
{
  GFileInputStreamDaemonPrivate *priv = file->priv;
  gsize len;
  guint32 request;

  while (TRUE)
    {
      switch (op->state)
	{
	  /* Initial state for read op */
	case SEEK_STATE_INIT:
	  request = G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_SET;
	  if (op->seek_type == G_SEEK_CUR)
	    request = G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_CUR;
	  else if (op->seek_type == G_SEEK_END)
	    request = G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_END;
	  append_request (file, request,
			  op->offset & 0xffffffff,
			  op->offset >> 32,
			  &op->seq_nr);
	  op->state = SEEK_STATE_WROTE_REQUEST;
	  op->sent_seek = FALSE;
	  io_op->io_buffer = priv->output_buffer->str;
	  io_op->io_size = priv->output_buffer->len;
	  io_op->io_allow_cancel = TRUE; /* Allow cancel before first byte of request sent */
	  return STATE_OP_WRITE;

	  /* wrote parts of output_buffer */
	case SEEK_STATE_WROTE_REQUEST:
	  if (io_op->io_cancelled)
	    {
	      op->ret_val = -1;
	      g_set_error (&op->ret_error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      return STATE_OP_DONE;
	    }

	  /* We weren't cancelled before first byte sent, so now we will send
	   * the seek request. Increase the seek generation now. */
	  if (!op->sent_seek)
	    priv->seek_generation++;
	  op->sent_seek = TRUE;
	  
	  if (io_op->io_res < priv->output_buffer->len)
	    {
	      memcpy (priv->output_buffer->str,
		      priv->output_buffer->str + io_op->io_res,
		      priv->output_buffer->len - io_op->io_res);
	      g_string_truncate (priv->output_buffer,
				 priv->output_buffer->len - io_op->io_res);
	      io_op->io_buffer = priv->output_buffer->str;
	      io_op->io_size = priv->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }
	  g_string_truncate (priv->output_buffer, 0);

	  op->state = SEEK_STATE_HANDLE_INPUT;
	  break;

	  /* No op */
	case SEEK_STATE_HANDLE_INPUT:
	  if (io_op->cancelled && !op->sent_cancel)
	    {
	      op->sent_cancel = TRUE;
	      append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL,
			      op->seq_nr, 0, NULL);
	      op->state = SEEK_STATE_WROTE_REQUEST;
	      io_op->io_buffer = priv->output_buffer->str;
	      io_op->io_size = priv->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }
	  
	  if (priv->input_state == INPUT_STATE_IN_BLOCK)
	    {
	      op->state = SEEK_STATE_HANDLE_INPUT_BLOCK;
	      break;
	    }
	  else if (priv->input_state == INPUT_STATE_IN_REPLY_HEADER)
	    {
	      op->state = SEEK_STATE_HANDLE_HEADER;
	      break;
	    }
	  g_assert_not_reached ();
	  break;

	  /* No op */
	case SEEK_STATE_HANDLE_INPUT_BLOCK:
	  g_assert (priv->input_state == INPUT_STATE_IN_BLOCK);
	  
	  op->state = SEEK_STATE_SKIP_BLOCK;
	  /* Reuse client buffer for skipping */
	  io_op->io_buffer = NULL;
	  io_op->io_size = priv->input_block_size;
	  io_op->io_allow_cancel = !op->sent_cancel;
	  return STATE_OP_SKIP;

	  /* Read block data */
	case SEEK_STATE_SKIP_BLOCK:
	  if (io_op->io_cancelled)
	    {
	      op->state = SEEK_STATE_HANDLE_INPUT;
	      break;
	    }
	  
	  g_assert (io_op->io_res <= priv->input_block_size);
	  priv->input_block_size -= io_op->io_res;
	  
	  if (priv->input_block_size == 0)
	    priv->input_state = INPUT_STATE_IN_REPLY_HEADER;
	  
	  op->state = SEEK_STATE_HANDLE_INPUT;
	  break;
	  
	  /* read header data, (or manual io_len/res = 0) */
	case SEEK_STATE_HANDLE_HEADER:
	  if (io_op->io_cancelled)
	    {
	      op->state = SEEK_STATE_HANDLE_INPUT;
	      break;
	    }

	  if (io_op->io_res > 0)
	    {
	      gsize unread_size = io_op->io_size - io_op->io_res;
	      g_string_set_size (priv->input_buffer,
				 priv->input_buffer->len - unread_size);
	    }
	  
	  len = get_reply_header_missing_bytes (priv->input_buffer);
	  if (len > 0)
	    {
	      gsize current_len = priv->input_buffer->len;
	      g_string_set_size (priv->input_buffer,
				 current_len + len);
	      io_op->io_buffer = priv->input_buffer->str + current_len;
	      io_op->io_size = len;
	      io_op->io_allow_cancel = !op->sent_cancel;
	      return STATE_OP_READ;
	    }

	  /* Got full header */

	  {
	    GVfsDaemonSocketProtocolReply reply;
	    char *data;
	    data = decode_reply (priv->input_buffer, &reply);

	    if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR &&
		reply.seq_nr == op->seq_nr)
	      {
		op->ret_val = FALSE;
		decode_error (&reply, data, &op->ret_error);
		g_string_truncate (priv->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA)
	      {
		g_string_truncate (priv->input_buffer, 0);
		priv->input_state = INPUT_STATE_IN_BLOCK;
		priv->input_block_size = reply.arg1;
		priv->input_block_seek_generation = reply.arg2;
		op->state = SEEK_STATE_HANDLE_INPUT_BLOCK;
		break;
	      }
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SEEK_POS)
	      {
		op->ret_val = TRUE;
		op->ret_offset = ((goffset)reply.arg2) << 32 | (goffset)reply.arg1;
		g_string_truncate (priv->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    else
	      g_assert_not_reached ();
	  }

	  g_string_truncate (priv->input_buffer, 0);
	  
	  /* This wasn't interesting, read next reply */
	  op->state = SEEK_STATE_HANDLE_HEADER;
	  break;

	default:
	  g_assert_not_reached ();
	}
      
      /* Clear io_op between non-op state switches */
      io_op->io_size = 0;
      io_op->io_res = 0;
      io_op->io_cancelled = FALSE;
 
    }
}


static gboolean
g_file_input_stream_daemon_seek (GFileInputStream *stream,
				 goffset offset,
				 GSeekType type,
				 GCancellable *cancellable,
				 GError **error)
{
  GFileInputStreamDaemon *file;
  SeekOperation op;

  file = G_FILE_INPUT_STREAM_DAEMON (stream);

  if (!g_file_input_stream_daemon_open (file, error))
    return -1;

  if (!file->priv->can_seek)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_NOT_SUPPORTED,
		   _("Seek not supported on stream"));
      return FALSE;
    }

  memset (&op, 0, sizeof (op));
  op.state = SEEK_STATE_INIT;
  op.offset = offset;
  op.seek_type = type;
  
  if (!run_sync_state_machine (file, (state_machine_iterator)iterate_seek_state_machine,
			       &op, cancellable, error))
    return -1; /* IO Error */

  if (!op.ret_val)
    g_propagate_error (error, op.ret_error);
  else
    file->priv->current_offset = op.ret_offset;
  
  return op.ret_val;
}

static GFileInfo *
g_file_input_stream_daemon_get_file_info (GFileInputStream     *stream,
					  GFileInfoRequestFlags requested,
					  char                 *attributes,
					  GCancellable         *cancellable,
					  GError              **error)
{
  GFileInputStreamDaemon *file;

  file = G_FILE_INPUT_STREAM_DAEMON (stream);

  if (!g_file_input_stream_daemon_open (file, error))
    return NULL;

  return NULL;
}
