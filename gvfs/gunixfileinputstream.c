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

  gsize outstanding_size;
  int outstanding_seek_generation;
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

static gssize
g_unix_file_input_stream_read (GInputStream *stream,
			       void         *buffer,
			       gsize         count,
			       GError      **error)
{
  GUnixFileInputStream *file;
  gssize res;
  gsize n;
  gsize n_read;
  char *read_ptr;
  guint32 count_32;
  char message[G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_SIZE];
  GVfsDaemonSocketProtocolCommand *cmd;
  gsize bytes_written;

  file = G_UNIX_FILE_INPUT_STREAM (stream);

  if (!g_unix_file_input_stream_open (file, error))
    return -1;
  
  count_32 = count;
  /* doesn't fit in 32bit, set some sensible large value */
  if (count_32 != count)
    count = count_32 = 4*1024*1024;

  cmd = (GVfsDaemonSocketProtocolCommand *)message;
  cmd->command = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_READ);
  cmd->arg = g_htonl (count_32);

  if (g_output_stream_write_all (file->priv->command_stream,
				 message, G_VFS_DAEMON_SOCKET_PROTOCOL_COMMAND_SIZE,
				 &bytes_written, error))
    {
      /* TODO: We sent a partial command down the pipe, what do we do here.
	 Especially if the error is a cancel... */
      return -1;
    }

  n_read = 0;
  read_ptr = buffer;

  while (n_read < count)
    {
      /* Inside a read block */
      if (file->priv->outstanding_size > 0)
	{
	  if (file->priv->seek_generation != file->priv->outstanding_seek_generation)
	    {
	      char buffer[4096];

	      while (file->priv->outstanding_size > 0)
		{
		  res = g_input_stream_read (file->priv->data_stream, buffer,
					     MIN (4096, file->priv->outstanding_size),
					     error);
		  if (res == -1)
		    return -1;
		  
		  if (res == 0) /* EOF */
		    return n_read;
		  
		  file->priv->outstanding_size -= res;
		}
	    }

	  n  = MIN (count, file->priv->outstanding_size);
	  res = g_input_stream_read (file->priv->data_stream,
				     read_ptr, n, error);
	  if (res == -1) 
	    {
	      if (n_read == 0)
		return -1;
	      if (error != NULL)
		g_error_free (*error);
	      return n_read;
	    }
	  
	  if (res == 0) /* EOF */
	    return n_read;
	  
	  n_read += res;
	  read_ptr += res;
	  
	  if (n_read == count)
	    return count;
	}
      else /* At start of block */ 
	{
	  GVfsDaemonSocketProtocolReply reply;

	  g_assert (sizeof (reply) == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE);
	  if (g_input_stream_read_all (file->priv->data_stream, (char *)&reply,
				       G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE,
				       &n_read, error))
	    {
	      /* TODO: We read a partial command, what do we do here, especially
		 on a cancel */
	      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
			   "Short read in stream protocol");
	      return -1;
	    }

	  if (n_read != G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE)
	    {
	      /* TODO: end of file */
	      return -1;
	    }

	  if (g_ntohl (reply.type) != G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA)
	    {
	      /* TODO: handle e.g. errors */
	      g_assert_not_reached ();
	    }

	  file->priv->outstanding_size = g_ntohl (reply.arg1);
	  file->priv->outstanding_seek_generation = g_ntohl (reply.arg2);
	  g_print ("read reply size: %d\n", file->priv->outstanding_size);
	}
    }
  
  return n_read;
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
