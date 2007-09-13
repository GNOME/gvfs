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
#include <gvfsreadhandle.h>

G_DEFINE_TYPE (GVfsReadHandle, g_vfs_read_handle, G_TYPE_OBJECT);

enum {
  PROP_0,
};

struct _GVfsReadHandlePrivate
{
  int fd;
  int remote_fd;
  
  gpointer data;
};

static void
g_vfs_read_handle_finalize (GObject *object)
{
  GVfsReadHandle *read_handle;

  read_handle = G_VFS_READ_HANDLE (object);
  
  if (read_handle->priv->fd != -1)
    close (read_handle->priv->fd);
  
  if (read_handle->priv->remote_fd != -1)
    close (read_handle->priv->remote_fd);
  
  if (G_OBJECT_CLASS (g_vfs_read_handle_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_read_handle_parent_class)->finalize) (object);
}

static void
g_vfs_read_handle_class_init (GVfsReadHandleClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GVfsReadHandlePrivate));
  
  gobject_class->finalize = g_vfs_read_handle_finalize;
}

static void
g_vfs_read_handle_init (GVfsReadHandle *handle)
{
  handle->priv = G_TYPE_INSTANCE_GET_PRIVATE (handle,
					      G_TYPE_VFS_READ_HANDLE,
					      GVfsReadHandlePrivate);
  handle->priv->fd = -1;
  handle->priv->remote_fd = -1;
}

GVfsReadHandle *
g_vfs_read_handle_new (GError **error)
{
  GVfsReadHandle *handle;
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

  handle = g_object_new (G_TYPE_VFS_READ_HANDLE, NULL);
  handle->priv->fd = socket_fds[0];
  handle->priv->remote_fd = socket_fds[1];

  return handle;
}

int
g_vfs_read_handle_get_fd (GVfsReadHandle *handle)
{
  return handle->priv->fd;
}

int
 g_vfs_read_handle_get_remote_fd (GVfsReadHandle *handle)
{
  return handle->priv->remote_fd;
}

void
 g_vfs_read_handle_close_remote_fd (GVfsReadHandle *handle)
{
  close (handle->priv->remote_fd);
  handle->priv->remote_fd = -1;
}

void
g_vfs_read_handle_set_data (GVfsReadHandle *read_handle,
			    gpointer data)
{
  read_handle->priv->data = data;
}
