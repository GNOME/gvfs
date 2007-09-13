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
#include <daemon/gvfsdaemonprotocol.h>

G_DEFINE_TYPE (GUnixFileInputStream, g_unix_file_input_stream, G_TYPE_FILE_INPUT_STREAM);

struct _GUnixFileInputStreamPrivate {
  char *filename;
  char *mountpoint;
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

  if (file->priv->fd != -1)
    return TRUE;

  connection = _g_vfs_unix_get_connection_sync (file->priv->mountpoint, &extra_fd, error);
  if (connection == NULL)
    return FALSE;

  message = dbus_message_new_method_call ("org.gtk.vfs.Daemon",
					  G_VFS_DBUS_DAEMON_PATH,
					  G_VFS_DBUS_DAEMON_INTERFACE,
					  G_VFS_DBUS_OP_READ_FILE);

  
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
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error calling ReadFile: %s",
		   derror.message);
      dbus_error_free (&derror);
      return FALSE;
    }

  /* TODO: handle errors created with dbus_message_new_error? 
   * dbus_message_is_error() ? 
   */
  if (_g_error_from_dbus_message (reply, error))
    {
      dbus_message_unref (reply);
      return FALSE;
    }
  
  /* No args in reply, only fd */
 
  file->priv->fd = receive_fd (extra_fd);
  g_print ("new fd: %d\n", file->priv->fd);
  return TRUE;
}

static gboolean 
write_command (GUnixFileInputStream *file,
	       char *buffer, gsize len,
	       GError **error)
{
  GInputStream *stream = G_INPUT_STREAM (file);
  char *write_buffer;
  gsize bytes_to_write;
  gssize bytes_written;

  bytes_to_write = len;
  write_buffer = buffer;
  do
    {
      bytes_written = write (file->priv->fd, write_buffer, bytes_to_write);

      if (bytes_written == -1)
	{
	  if (g_input_stream_is_cancelled (stream))
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
		       _("Error writing command to stream: %s"),
		       g_strerror (errno));
	  return FALSE;
	}
	  
      bytes_to_write -= bytes_written;
      write_buffer += bytes_written;
    }
  while (bytes_to_write > 0);

  return TRUE;
}

static int
read_data (GUnixFileInputStream *file,
	   char *buffer, gsize count,
	   GError **error)
{
  GInputStream *stream = G_INPUT_STREAM (file);
  ssize_t res;

  while (1)
    {
      res = read (file->priv->fd, buffer, count);
      if (res == -1)
	{
	  if (g_input_stream_is_cancelled (stream))
	    {
	      g_set_error (error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      return -1;
	    }
	  
	  if (errno == EINTR)
	    continue;
	  
	  g_set_error (error, G_FILE_ERROR,
		       g_file_error_from_errno (errno),
		       _("Error reading from file '%s': %s"),
		       file->priv->filename, g_strerror (errno));
	  return -1;
	}
      
      return res;
    }
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
  char message[5];

  file = G_UNIX_FILE_INPUT_STREAM (stream);

  if (!g_unix_file_input_stream_open (file, error))
    return -1;
  
  count_32 = count;
  /* doesn't fit in 32bit, set some sensible large value */
  if (count_32 != count)
    count = count_32 = 4*1024*1024;

  message[0] = 'R';
  memcpy (&message[1], &count_32, 4);

  if (!write_command (file, message, 5, error))
    return -1;

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
		  res = read_data (file, buffer, 
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
	  res = read_data (file, read_ptr, n, error);
	  
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
	  struct {
	    gint32 block_size;
	    gint32 seek_generation;
	  } header;

	  g_assert (sizeof (header) == 8);
	  res = read_data (file, (char *)&header, 8, error);
	  if (res == -1)
	    return -1;

	  if (res != 8)
	    {
	      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
			   "Short read in stream protocol");
	      return -1;
	    }
	  
	  file->priv->outstanding_size = header.block_size;
	  file->priv->outstanding_seek_generation = header.seek_generation;
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
