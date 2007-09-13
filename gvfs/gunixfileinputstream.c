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
#include "gunixfileinputstream.h"
#include "gvfsunixdbus.h"
#include "gfileinfosimple.h"
#include "gsocketinputstream.h"
#include "gsocketoutputstream.h"
#include <daemon/gvfsdaemonprotocol.h>

G_DEFINE_TYPE (GUnixFileInputStream, g_unix_file_input_stream, G_TYPE_FILE_INPUT_STREAM);

struct _GUnixFileInputStreamPrivate {
  char *filename;
  char *mountpoint;
  GOutputStream *command_stream;
  GInputStream *data_stream;
  int fd;
  int seek_generation;
  guint32 seq_nr;

  
  gsize outstanding_data_size; /* zero means reading reply */
  int outstanding_data_seek_generation;
};

static gssize     g_unix_file_input_stream_read          (GInputStream           *stream,
							   void                   *buffer,
							   gsize                   count,
							   GError                **error);
static gssize     g_unix_file_input_stream_skip          (GInputStream           *stream,
							   gsize                   count,
							   GError                **error);
static gboolean   g_unix_file_input_stream_close         (GInputStream           *stream,
							   GError                **error);
static GFileInfo *g_unix_file_input_stream_get_file_info (GFileInputStream       *stream,
							   GFileInfoRequestFlags   requested,
							   char                   *attributes,
							   GError                **error);

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
g_unix_file_input_stream_init (GUnixFileInputStream *info)
{
  info->priv = G_TYPE_INSTANCE_GET_PRIVATE (info,
					    G_TYPE_UNIX_FILE_INPUT_STREAM,
					    GUnixFileInputStreamPrivate);
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
		   "Out of memory");
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

  /* No args in reply, only fd */
  dbus_message_get_args (message, NULL,
                         DBUS_TYPE_UINT32, &fd_id,
                         DBUS_TYPE_INVALID);
  /* TODO: verify fd id */
  file->priv->fd = receive_fd (extra_fd);
  g_print ("new fd: %d\n", file->priv->fd);
  
  file->priv->command_stream = g_socket_output_stream_new (file->priv->fd, FALSE);
  file->priv->data_stream = g_socket_input_stream_new (file->priv->fd, TRUE);
  
  return TRUE;
}

static gboolean
send_command (GUnixFileInputStream *stream, guint32 command, guint32 arg, guint32 *seq_nr, GError **error)
{
  char message[G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_SIZE];
  GVfsDaemonSocketProtocolCommand *cmd;
  gsize bytes_written;
  GError *internal_error;

  *seq_nr = stream->priv->seq_nr;
  
  cmd = (GVfsDaemonSocketProtocolCommand *)message;
  cmd->command = g_htonl (command);
  cmd->seq_nr = g_htonl (stream->priv->seq_nr++);
  cmd->arg = g_htonl (arg);

  internal_error = NULL;
  if (g_output_stream_write_all (stream->priv->command_stream,
				 message, G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_SIZE,
				 &bytes_written, &internal_error))
    {
      /* This is not a cancel, because we never cancel the command stream,
       * so ignore the fact that we just sent a partial command as we have
       * worse problems now.
       */
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error writing stream protocol: %s", internal_error->message);
      g_error_free (internal_error);
      return FALSE;
    }

  if (bytes_written != G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_SIZE) 
    {
      /* Short protocol read, shouldn't happen */
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Short write in stream protocol");
      return FALSE;
    }
  
  return TRUE;
}

/* Returns -2 if no outstanding data */
static gssize
read_outstanding_data (GUnixFileInputStream *file, char *buffer, gssize count, GError **error)
{
  gssize res;
  GError *internal_error;

  if (file->priv->outstanding_data_size == 0)
    return -2;
  
  if (file->priv->seek_generation != file->priv->outstanding_data_seek_generation)
    {
      while (file->priv->outstanding_data_size > 0)
	{
	  internal_error = NULL;
	  res = g_input_stream_skip (file->priv->data_stream,
				     file->priv->outstanding_data_size,
				     &internal_error);
	  if (res == -1)
	    {
	      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
			   "Error writing stream protocol: %s", internal_error->message);
	      g_error_free (internal_error);
	      return -1;
	    }
	  
	  if (res == 0) 
	    {
	      /* Short protocol read, shouldn't happen */
	      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
			   "Short read in stream protocol");
	      return -1;
	    }

	  file->priv->outstanding_data_size -= res;
	}
      return -2;
    }
  else
    {
      count  = MIN (count, file->priv->outstanding_data_size);
      internal_error = NULL;
      res = g_input_stream_read (file->priv->data_stream,
				 buffer, count, &internal_error);
      if (res == -1)
	{
	  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       "Error reading stream protocol: %s", internal_error->message);
	  g_error_free (internal_error);
	  return -1;
	}
      
      if (res > 0)
	file->priv->outstanding_data_size -= res;
      
      return res;
    }
}

static gboolean
read_ignoring_cancel (GUnixFileInputStream *file,
		      char *ptr,
		      gsize count,
		      gboolean *cancelled,
		      GError **error)
{
  gsize bytes, n_read;
  gboolean ok;
  GError *internal_error;
  
  bytes = 0;
  while (bytes < count)
    {
      internal_error = NULL;
      ok = g_input_stream_read_all (file->priv->data_stream,
				    ptr, count - bytes,
				    &n_read, &internal_error);
      bytes += n_read;
      ptr += n_read;

      if (!ok)
	{
	  if (internal_error->domain == G_VFS_ERROR &&
	      internal_error->code == G_VFS_ERROR_CANCELLED)
	    {
	      /* Ignore cancels here as we want to make sure we read a full reply */
	      g_error_free (internal_error);
	      *cancelled = TRUE;
	    }
	  else
	    {
	      /* We have a worse problem, ignore that we've only read a partial header */
	      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
			   "Error in stream protocol: %s", internal_error->message);
	      g_error_free (internal_error);
	      return FALSE;
	    }
	}
      
      if (n_read == 0)
	{
	  /* Short protocol read, shouldn't happen */
	  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       "Short read in stream protocol");
	  return FALSE;
	}
    }
  return TRUE;
}


static gboolean
read_read_reply (GUnixFileInputStream *file,
		 guint32 command_seq_nr,
		 gboolean *found_data,
		 gboolean *cancelled,
		 GError **error)
{
  guint32 type, seq_nr, arg1, arg2;
  char *error_data;
  GVfsDaemonSocketProtocolReply reply;

  if (!read_ignoring_cancel (file,
			     (char *)&reply,
			     G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE,
			     cancelled, error))
    return FALSE;
  
  type = g_ntohl (reply.type);
  seq_nr = g_ntohl (reply.seq_nr);
  arg1 = g_ntohl (reply.arg1);
  arg2 = g_ntohl (reply.arg2);
  
  if (type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR)
    {
      error_data = g_malloc (arg2);
      
      if (!read_ignoring_cancel (file,
				 (char *)error_data,
				 arg2,
				 cancelled, error))
	{
	  g_free (error_data);
	  return FALSE;
	}
      
      if (seq_nr == command_seq_nr)
	{
	  g_set_error (error,
		       g_quark_from_string (error_data),
		       arg1,
		       error_data + strlen (error_data) + 1);
	  g_free (error_data);
	  return FALSE;
	}
      
      g_free (error_data);
    }
  
  if (type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA)
    {
      file->priv->outstanding_data_size = arg1;
      file->priv->outstanding_data_seek_generation = arg2;
      *found_data = TRUE;
    }
  
  return TRUE;
}


static gssize
g_unix_file_input_stream_read (GInputStream *stream,
			       void         *buffer,
			       gsize         count,
			       GError      **error)
{
  GUnixFileInputStream *file;
  gssize res;
  guint32 count_32;
  guint32 seq_nr;
  gboolean cancelled, found_data;

  file = G_UNIX_FILE_INPUT_STREAM (stream);

  if (!g_unix_file_input_stream_open (file, error))
    return -1;
  
  count_32 = count;
  /* doesn't fit in 32bit, set some sensible large value */
  if (count_32 != count)
    count = count_32 = 4*1024*1024;

  /* Do we know that there is data in the pipe already, if so, read it */
  
  res = read_outstanding_data (file, buffer, count, error);
  if (res != -2)
    {
      /* Had data, or got an error */
      return res;
    }

  /* No outstanding data we know about, send read request */
  if (!send_command (file, G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_READ, count_32, &seq_nr, error))
    return -1;

  /* We sent the command, now read the reply, looking for replies to seq_nr */

  while (TRUE)
    {
      found_data = FALSE;
      do
	{
	  cancelled = FALSE;
	  if (!read_read_reply (file, seq_nr, &found_data, &cancelled, error))
	    return -1;
	  
	  if (cancelled)
	    {
	      send_command (file, G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_CANCEL, 0, &seq_nr, NULL);
	      g_set_error (error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      return -1;
	    }
	}
      while (!found_data);
      
      res = read_outstanding_data (file, buffer, count, error);
      if (res != -2)
	{
	  /* Had data, or got an error */
	  return res;
	}
    }
}

static gssize
g_unix_file_input_stream_skip (GInputStream *stream,
				gsize         count,
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
				 GError      **error)
{
  GUnixFileInputStream *file;

  file = G_UNIX_FILE_INPUT_STREAM (stream);

  if (file->priv->fd == -1)
    return TRUE;


  return FALSE;
}

static GFileInfo *
g_unix_file_input_stream_get_file_info (GFileInputStream     *stream,
					GFileInfoRequestFlags requested,
					char                 *attributes,
					GError              **error)
{
  GUnixFileInputStream *file;

  file = G_UNIX_FILE_INPUT_STREAM (stream);

  if (!g_unix_file_input_stream_open (file, error))
    return NULL;

  return NULL;
}
