#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsbackend.h"

G_DEFINE_TYPE (GVfsBackend, g_vfs_backend, G_TYPE_OBJECT);

static void
g_vfs_backend_finalize (GObject *object)
{
  GVfsBackend *backend;

  backend = G_VFS_BACKEND (object);

  if (G_OBJECT_CLASS (g_vfs_backend_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_parent_class)->finalize) (object);
}

static void
g_vfs_backend_class_init (GVfsBackendClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_finalize;
}

static void
g_vfs_backend_init (GVfsBackend *backend)
{
}

gboolean
g_vfs_backend_open_for_read (GVfsBackend *backend,
			     GVfsJobOpenForRead *job,
			     char *filename)
{
  GVfsBackendClass *class;

  class = G_VFS_BACKEND_GET_CLASS (backend);
  
  return class->open_for_read (backend, job, filename);
}

gboolean
g_vfs_backend_close_read (GVfsBackend        *backend,
			  GVfsJobCloseRead   *job,
			  GVfsHandle         *handle)
{
  GVfsBackendClass *class;

  class = G_VFS_BACKEND_GET_CLASS (backend);
  
  return class->close_read (backend, job, handle);
}

gboolean
g_vfs_backend_read (GVfsBackend *backend,
		    GVfsJobRead *job,
		    GVfsHandle *handle,
		    char *buffer,
		    gsize bytes_requested)
{
  GVfsBackendClass *class;

  class = G_VFS_BACKEND_GET_CLASS (backend);
  
  return class->read (backend, job, handle,
		      buffer, bytes_requested);
}

gboolean
g_vfs_backend_seek_on_read  (GVfsBackend        *backend,
			     GVfsJobSeekRead    *job,
			     GVfsHandle         *handle,
			     goffset             offset,
			     GSeekType           type)
{
  GVfsBackendClass *class;

  class = G_VFS_BACKEND_GET_CLASS (backend);
  
  return class->seek_on_read (backend, job, handle,
			      offset, type);
}
