#include <config.h>

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gi18n.h>
#include <dbus-gmain.h>
#include <gvfsreadrequest.h>

G_DEFINE_TYPE (GVfsReadRequest, g_vfs_read_request, G_TYPE_OBJECT);

enum {
  PROP_0,
};

struct _GVfsReadRequestPrivate
{
  int fd;
  int remote_fd;
};

static void
g_vfs_read_request_finalize (GObject *object)
{
  GVfsReadRequest *read_request;

  read_request = G_VFS_READ_REQUEST (object);
  
  if (read_request->priv->fd != -1)
    close (read_request->priv->fd);
  
  if (read_request->priv->remote_fd != -1)
    close (read_request->priv->remote_fd);
  
  if (G_OBJECT_CLASS (g_vfs_read_request_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_read_request_parent_class)->finalize) (object);
}

static void
g_vfs_read_request_class_init (GVfsReadRequestClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GVfsReadRequestPrivate));
  
  gobject_class->finalize = g_vfs_read_request_finalize;
}

static void
g_vfs_read_request_init (GVfsReadRequest *request)
{
  request->priv = G_TYPE_INSTANCE_GET_PRIVATE (request,
					      G_TYPE_VFS_READ_REQUEST,
					      GVfsReadRequestPrivate);
  request->priv->fd = -1;
  request->priv->remote_fd = -1;
}

GVfsReadRequest *
g_vfs_read_request_new (GError **error)
{
  GVfsReadRequest *request;
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

  request = g_object_new (G_TYPE_VFS_READ_REQUEST, NULL);
  request->priv->fd = socket_fds[0];
  request->priv->remote_fd = socket_fds[1];

  return request;
}

int
g_vfs_read_request_get_fd (GVfsReadRequest *request)
{
  return request->priv->fd;
}

int
 g_vfs_read_request_get_remote_fd (GVfsReadRequest *request)
{
  return request->priv->remote_fd;
}

void
 g_vfs_read_request_close_remote_fd (GVfsReadRequest *request)
{
  close (request->priv->remote_fd);
  request->priv->remote_fd = -1;
}

