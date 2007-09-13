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
#include "gunixfileinputstream.h"
#include "gvfsunixdbus.h"
#include "gfileinfosimple.h"
#include "gsocketinputstream.h"
#include "gsocketoutputstream.h"
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
  
  gboolean cancelled;
  gboolean sent_cancel;
  
  char *io_buffer;
  gsize io_size;
  gsize io_res;
  /* The operation always succeeds, or gets cancelled.
     If we get an error doing the i/o that is considered fatal */
  gboolean io_allow_cancel;
  gboolean io_cancelled;
  
  guint32 seq_nr;
  
} ReadOperation;


struct _GUnixFileInputStreamPrivate {
  char *filename;
  char *mountpoint;
  GOutputStream *command_stream;
  GInputStream *data_stream;
  int fd;
  int seek_generation;
  guint32 seq_nr;
  goffset current_offset;

  guint can_seek : 1;
  guint can_truncate : 1;
  
  InputState input_state;
  gsize input_block_size;
  int input_block_seek_generation;

  GString *input_buffer;
  GString *output_buffer;
};

static void g_unix_file_input_stream_seekable_iface_init (GSeekableIface *iface);
static gssize     g_unix_file_input_stream_read          (GInputStream           *stream,
							  void                   *buffer,
							  gsize                   count,
							  GCancellable           *cancellable,
							  GError                **error);
static gssize     g_unix_file_input_stream_skip          (GInputStream           *stream,
							  gsize                   count,
							  GCancellable           *cancellable,
							  GError                **error);
static gboolean   g_unix_file_input_stream_close         (GInputStream           *stream,
							  GCancellable           *cancellable,
							  GError                **error);
static GFileInfo *g_unix_file_input_stream_get_file_info (GFileInputStream       *stream,
							  GFileInfoRequestFlags   requested,
							  char                   *attributes,
							  GCancellable           *cancellable,
							  GError                **error);
static goffset    g_unix_file_input_stream_tell          (GSeekable              *seekable);
static gboolean   g_unix_file_input_stream_can_seek      (GSeekable              *seekable);
static gboolean   g_unix_file_input_stream_seek          (GSeekable              *seekable,
							  goffset                 offset,
							  GSeekType               type,
							  GCancellable           *cancellable,
							  GError                **error);
static gboolean   g_unix_file_input_stream_can_truncate  (GSeekable              *seekable);
static gboolean   g_unix_file_input_stream_truncate      (GSeekable              *seekable,
							  goffset                 offset,
							  GCancellable           *cancellable,
							  GError                **error);

G_DEFINE_TYPE_WITH_CODE (GUnixFileInputStream, g_unix_file_input_stream,
			 G_TYPE_FILE_INPUT_STREAM,
			 G_IMPLEMENT_INTERFACE (G_TYPE_SEEKABLE,
						g_unix_file_input_stream_seekable_iface_init))

static void
g_unix_file_input_stream_finalize (GObject *object)
{
  GUnixFileInputStream *file;
  
  file = G_UNIX_FILE_INPUT_STREAM (object);

  if (file->priv->command_stream)
    g_object_unref (file->priv->command_stream);
  if (file->priv->data_stream)
    g_object_unref (file->priv->data_stream);
  
  g_free (file->priv->filename);
  g_free (file->priv->mountpoint);
  
  if (G_OBJECT_CLASS (g_unix_file_input_stream_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_unix_file_input_stream_parent_class)->finalize) (object);
}

static void
g_unix_file_input_stream_class_init (GUnixFileInputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);
  GFileInputStreamClass *file_stream_class = G_FILE_INPUT_STREAM_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GUnixFileInputStreamPrivate));
  
  gobject_class->finalize = g_unix_file_input_stream_finalize;

  stream_class->read = g_unix_file_input_stream_read;
  stream_class->skip = g_unix_file_input_stream_skip;
  stream_class->close = g_unix_file_input_stream_close;
  file_stream_class->get_file_info = g_unix_file_input_stream_get_file_info;
}

static void
g_unix_file_input_stream_seekable_iface_init (GSeekableIface *iface)
{
  iface->tell = g_unix_file_input_stream_tell;
  iface->can_seek = g_unix_file_input_stream_can_seek;
  iface->seek = g_unix_file_input_stream_seek;
  iface->can_truncate = g_unix_file_input_stream_can_truncate;
  iface->truncate = g_unix_file_input_stream_truncate;
}

static void
g_unix_file_input_stream_init (GUnixFileInputStream *info)
{
  info->priv = G_TYPE_INSTANCE_GET_PRIVATE (info,
					    G_TYPE_UNIX_FILE_INPUT_STREAM,
					    GUnixFileInputStreamPrivate);

  info->priv->output_buffer = g_string_new ("");
  info->priv->input_buffer = g_string_new ("");
}

GFileInputStream *
g_unix_file_input_stream_new (const char *filename,
			      const char *mountpoint)
{
  GUnixFileInputStream *stream;

  stream = g_object_new (G_TYPE_UNIX_FILE_INPUT_STREAM, NULL);

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
g_unix_file_input_stream_open (GUnixFileInputStream *file,
			       GError      **error)
{
  DBusConnection *connection;
  DBusError derror;
  int extra_fd;
  DBusMessage *message, *reply;
  DBusMessageIter iter;
  guint32 fd_id;
  dbus_bool_t can_seek, can_truncate;
  

  if (file->priv->fd != -1)
    return TRUE;

  connection = _g_vfs_unix_get_connection_sync (file->priv->mountpoint, &extra_fd, error);
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

  dbus_message_get_args (message, NULL,
                         DBUS_TYPE_UINT32, &fd_id,
                         DBUS_TYPE_BOOLEAN, &can_seek,
                         DBUS_TYPE_BOOLEAN, &can_truncate,
                         DBUS_TYPE_INVALID);
  /* TODO: verify fd id */
  file->priv->fd = receive_fd (extra_fd);
  file->priv->can_seek = can_seek;
  file->priv->can_truncate = can_truncate;
  
  file->priv->command_stream = g_socket_output_stream_new (file->priv->fd, FALSE);
  file->priv->data_stream = g_socket_input_stream_new (file->priv->fd, TRUE);
  
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
append_request (GUnixFileInputStream *stream, guint32 command, guint32 arg, guint32 *seq_nr)
{
  GVfsDaemonSocketProtocolCommand cmd;

  g_assert (sizeof (cmd) == G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_SIZE);
  
  if (seq_nr)
    *seq_nr = stream->priv->seq_nr;
  
  cmd.command = g_htonl (command);
  cmd.seq_nr = g_htonl (stream->priv->seq_nr++);
  cmd.arg = g_htonl (arg);

  g_string_append_len (stream->priv->output_buffer,
		       (char *)&cmd, G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_SIZE);
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


/* read cycle:

   if we know of a (partially read) matching outstanding block, read from it
   create packet, append to outgoing
   flush outgoing
   start processing input, looking for a data block with same seek gen,
    or an error with same seq nr
   on cancel, send cancel command and go back to loop
 */

static StateOp
run_read_state_machine (GUnixFileInputStream *file, ReadOperation *op)
{
  GUnixFileInputStreamPrivate *priv = file->priv;
  gsize len;

  do
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
	      op->io_buffer = op->buffer;
	      op->io_size = MIN (op->buffer_size, priv->input_block_size);
	      op->io_allow_cancel = TRUE; /* Allow cancel before we sent request */
	      return STATE_OP_READ;
	    }

	  append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_READ,
			  op->buffer_size, &op->seq_nr);
	  op->state = READ_STATE_WROTE_COMMAND;
	  op->io_buffer = priv->output_buffer->str;
	  op->io_size = priv->output_buffer->len;
	  op->io_allow_cancel = TRUE; /* Allow cancel before first byte of request sent */
	  return STATE_OP_WRITE;

	  /* wrote parts of output_buffer */
	case READ_STATE_WROTE_COMMAND:
	  if (op->io_cancelled)
	    {
	      op->ret_val = -1;
	      g_set_error (&op->ret_error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      return STATE_OP_DONE;
	    }
	  
	  if (op->io_res < priv->output_buffer->len)
	    {
	      memcpy (priv->output_buffer->str,
		      priv->output_buffer->str + op->io_res,
		      priv->output_buffer->len - op->io_res);
	      g_string_truncate (priv->output_buffer,
				 priv->output_buffer->len - op->io_res);
	      op->io_buffer = priv->output_buffer->str;
	      op->io_size = priv->output_buffer->len;
	      op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }
	  g_string_truncate (priv->output_buffer, 0);

	  op->state = READ_STATE_HANDLE_INPUT;
	  break;

	  /* No op */
	case READ_STATE_HANDLE_INPUT:
	  if (op->cancelled && !op->sent_cancel)
	    {
	      op->sent_cancel = TRUE;
	      append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_CANCEL,
			      op->seq_nr, NULL);
	      op->state = READ_STATE_WROTE_COMMAND;
	      op->io_buffer = priv->output_buffer->str;
	      op->io_size = priv->output_buffer->len;
	      op->io_allow_cancel = FALSE;
	    }
	  
	  if (priv->input_state == INPUT_STATE_IN_BLOCK)
	    {
	      op->state = READ_STATE_HANDLE_INPUT_BLOCK;
	      break;
	    }
	  else if (priv->input_state == INPUT_STATE_IN_REPLY_HEADER)
	    {
	      op->io_size = 0;
	      op->io_res = 0;
	      op->io_cancelled = FALSE;
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
	      op->io_buffer = op->buffer;
	      op->io_size = MIN (op->buffer_size, priv->input_block_size);
	      op->io_allow_cancel = FALSE;
	      return STATE_OP_READ;
	    }
	  else
	    {
	      op->state = READ_STATE_SKIP_BLOCK;
	      /* Reuse client buffer for skipping */
	      op->io_buffer = op->buffer;
	      op->io_size = priv->input_block_size;
	      op->io_allow_cancel = !op->sent_cancel;
	      return STATE_OP_SKIP;
	    }
	  break;

	  /* Read block data */
	case READ_STATE_SKIP_BLOCK:
	  if (op->io_cancelled)
	    {
	      op->state = READ_STATE_HANDLE_INPUT;
	      break;
	    }
	  
	  g_assert (op->io_res <= priv->input_block_size);
	  priv->input_block_size -= op->io_res;
	  
	  if (priv->input_block_size == 0)
	    priv->input_state = INPUT_STATE_IN_REPLY_HEADER;
	  
	  op->state = READ_STATE_HANDLE_INPUT;
	  break;
	  
	  /* read header data, (or manual io_len/res = 0) */
	case READ_STATE_HANDLE_HEADER:
	  if (op->io_cancelled)
	    {
	      op->state = READ_STATE_HANDLE_INPUT;
	      break;
	    }

	  if (op->io_res > 0)
	    {
	      gsize unread_size = op->io_size - op->io_res;
	      g_string_set_size (priv->input_buffer,
				 priv->input_buffer->len - unread_size);
	    }
	  
	  len = get_reply_header_missing_bytes (priv->input_buffer);
	  if (len > 0)
	    {
	      gsize current_len = priv->input_buffer->len;
	      g_string_set_size (priv->input_buffer,
				 current_len + len);
	      op->io_buffer = priv->input_buffer->str + current_len;
	      op->io_size = len;
	      op->io_allow_cancel = !op->sent_cancel;
	      return STATE_OP_READ;
	    }

	  /* Got full header */

	  {
	    GVfsDaemonSocketProtocolReply *reply;
	    guint32 type, seq_nr, arg1, arg2;
	    char *data;
	    
	    reply = (GVfsDaemonSocketProtocolReply *)priv->input_buffer->str;
	    type = g_ntohl (reply->type);
	    seq_nr = g_ntohl (reply->seq_nr);
	    arg1 = g_ntohl (reply->arg1);
	    arg2 = g_ntohl (reply->arg2);
	    data = priv->input_buffer->str + G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE;

	    if (type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR &&
		seq_nr == op->seq_nr)
	      {
		op->ret_val = -1;
		g_set_error (&op->ret_error,
			     g_quark_from_string (data),
			     arg1,
			     data + strlen (data) + 1);
		g_string_truncate (priv->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    else if (type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA)
	      {
		g_string_truncate (priv->input_buffer, 0);
		priv->input_state = INPUT_STATE_IN_BLOCK;
		priv->input_block_size = arg1;
		priv->input_block_seek_generation = arg2;
		op->state = READ_STATE_HANDLE_INPUT_BLOCK;
		break;
	      }
	    else if (type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SEEK_POS)
	      {
		/* Ignore when reading */
	      }
	    else
	      g_assert_not_reached ();
	  }

	  g_string_truncate (priv->input_buffer, 0);
	  
	  /* This wasn't interesting, read next reply */
	  op->io_size = 0;
	  op->io_res = 0;
	  op->io_cancelled = FALSE;
	  op->state = READ_STATE_HANDLE_HEADER;
	  break;

	  /* Read block data */
	case READ_STATE_READ_BLOCK:
	  if (op->io_cancelled)
	    {
	      op->ret_val = -1;
	      g_set_error (&op->ret_error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      return STATE_OP_DONE;
	    }
	  
	  if (op->io_res > 0)
	    {
	      g_assert (op->io_res <= priv->input_block_size);
	      priv->input_block_size -= op->io_res;
	      if (priv->input_block_size == 0)
		priv->input_state = INPUT_STATE_IN_REPLY_HEADER;
	    }
	  
	  op->ret_val = op->io_res;
	  op->ret_error = NULL;
	  return STATE_OP_DONE;
	  
	default:
	  g_assert_not_reached ();
	}
      
    }
  while (1);
}

static gssize
g_unix_file_input_stream_read (GInputStream *stream,
			       void         *buffer,
			       gsize         count,
			       GCancellable *cancellable,
			       GError      **error)
{
  GUnixFileInputStream *file;
  gssize res;
  StateOp io_op;
  ReadOperation op;
  GError *io_error;

  file = G_UNIX_FILE_INPUT_STREAM (stream);

  if (!g_unix_file_input_stream_open (file, error))
    return -1;

  /* Limit for sanity and to avoid 32bit overflow */
  if (count > MAX_READ_SIZE)
    count = MAX_READ_SIZE;

  memset (&op, 0, sizeof (ReadOperation));
  op.state = READ_STATE_INIT;
  
  op.buffer = buffer;
  op.buffer_size = count;
  
  do
    {
      if (cancellable)
	op.cancelled = g_cancellable_is_cancelled (cancellable);
      
      io_op = run_read_state_machine (file, &op);

      if (io_op == STATE_OP_DONE)
	break;
      
      io_error = NULL;
      if (io_op == STATE_OP_READ)
	{
	  res = g_input_stream_read (file->priv->data_stream,
				     op.io_buffer, op.io_size,
				     op.io_allow_cancel ? cancellable : NULL,
				     &io_error);
	}
      else if (io_op == STATE_OP_SKIP)
	{
	  res = g_input_stream_skip (file->priv->data_stream,
				     op.io_size,
				     op.io_allow_cancel ? cancellable : NULL,
				     &io_error);
	}
      else if (io_op == STATE_OP_WRITE)
	{
	  res = g_output_stream_write (file->priv->command_stream,
				       op.io_buffer, op.io_size,
				       op.io_allow_cancel ? cancellable : NULL,
				       &io_error);
	}

      if (res == -1)
	{
	  if (error_is_cancel (io_error))
	    {
	      op.io_res = 0;
	      op.io_cancelled = TRUE;
	      g_error_free (io_error);
	    }
	  else
	    {
	      op.ret_val = -1;
	      g_set_error (&op.ret_error, G_FILE_ERROR, G_FILE_ERROR_IO,
			   "Error in stream protocol: %s", io_error->message);
	      g_error_free (io_error);
	      break;
	    }
	}
      else
	{
	  op.io_res = res;
	  op.io_cancelled = FALSE;
	}
    }
  while (io_op != STATE_OP_DONE);

  if (op.ret_val == -1)
    g_propagate_error (error, op.ret_error);
  else
    file->priv->current_offset += op.ret_val;
  
  return op.ret_val;
}

static gssize
g_unix_file_input_stream_skip (GInputStream *stream,
			       gsize         count,
			       GCancellable *cancellable,
			       GError      **error)
{
  GUnixFileInputStream *file;

  file = G_UNIX_FILE_INPUT_STREAM (stream);
  
  if (!g_unix_file_input_stream_open (file, error))
    return -1;

  return 0;
}

static gboolean
g_unix_file_input_stream_close (GInputStream *stream,
				GCancellable *cancellable,
				GError      **error)
{
  GUnixFileInputStream *file;
  GError *my_error;

  file = G_UNIX_FILE_INPUT_STREAM (stream);

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
g_unix_file_input_stream_tell (GSeekable  *seekable)
{
  GUnixFileInputStream *file;

  file = G_UNIX_FILE_INPUT_STREAM (seekable);
  
  return file->priv->current_offset;
}

static gboolean
g_unix_file_input_stream_can_seek (GSeekable  *seekable)
{
  GUnixFileInputStream *file;

  file = G_UNIX_FILE_INPUT_STREAM (seekable);

  if (!g_unix_file_input_stream_open (file, NULL))
    return FALSE;

  return file->priv->can_seek;
}

static gboolean
g_unix_file_input_stream_seek (GSeekable  *seekable,
			       goffset     offset,
			       GSeekType   type,
			       GCancellable  *cancellable,
			       GError    **error)
{
  GUnixFileInputStream *file;

  file = G_UNIX_FILE_INPUT_STREAM (seekable);

  if (!g_unix_file_input_stream_open (file, error))
    return -1;

  if (!file->priv->can_seek)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_NOT_SUPPORTED,
		   _("Seek not supported on stream"));
      return FALSE;
    }

  /* TODO: implement seek */
  return TRUE;
}

static gboolean
g_unix_file_input_stream_can_truncate (GSeekable  *seekable)
{
  GUnixFileInputStream *file;

  file = G_UNIX_FILE_INPUT_STREAM (seekable);

  if (!g_unix_file_input_stream_open (file, NULL))
    return FALSE;

  return file->priv->can_seek;
}

static gboolean
g_unix_file_input_stream_truncate (GSeekable  *seekable,
				   goffset     offset,
				   GCancellable  *cancellable,
				   GError    **error)
{
  GUnixFileInputStream *file;

  file = G_UNIX_FILE_INPUT_STREAM (seekable);

  if (!g_unix_file_input_stream_open (file, error))
    return -1;

  if (!file->priv->can_truncate)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_NOT_SUPPORTED,
		   _("Truncate not supported on stream"));
      return FALSE;
    }

  /* TODO: implement truncate */
  
  return TRUE;

}

static GFileInfo *
g_unix_file_input_stream_get_file_info (GFileInputStream     *stream,
					GFileInfoRequestFlags requested,
					char                 *attributes,
					GCancellable         *cancellable,
					GError              **error)
{
  GUnixFileInputStream *file;

  file = G_UNIX_FILE_INPUT_STREAM (stream);

  if (!g_unix_file_input_stream_open (file, error))
    return NULL;

  return NULL;
}
