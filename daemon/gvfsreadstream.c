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
  int fd;
  int remote_fd;
  
  gpointer data; /* user data, i.e. GVfsHandle */
  GVfsJob *job;
};

static void
g_vfs_read_stream_finalize (GObject *object)
{
  GVfsReadStream *read_stream;

  read_stream = G_VFS_READ_STREAM (object);
  
  if (read_stream->priv->fd != -1)
    close (read_stream->priv->fd);
  
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
		  g_cclosure_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
  
}

static void
g_vfs_read_stream_init (GVfsReadStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
					      G_TYPE_VFS_READ_STREAM,
					      GVfsReadStreamPrivate);
  stream->priv->fd = -1;
  stream->priv->remote_fd = -1;
}


static void
set_fd_nonblocking (int fd)
{
  glong fcntl_flags;

  fcntl_flags = fcntl (fd, F_GETFL);

#ifdef O_NONBLOCK
  fcntl_flags |= O_NONBLOCK;
#else
  fcntl_flags |= O_NDELAY;
#endif

  fcntl (fd, F_SETFL, fcntl_flags);
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
  stream->priv->fd = socket_fds[0];
  stream->priv->remote_fd = socket_fds[1];

  set_fd_nonblocking (stream->priv->fd);
  
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
