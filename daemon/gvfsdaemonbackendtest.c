#include <config.h>

#include "gvfsdaemonbackendtest.h"

G_DEFINE_TYPE (GVfsDaemonBackendTest, g_vfs_daemon_backend_test, G_TYPE_VFS_DAEMON_BACKEND);

static void
g_vfs_daemon_backend_test_finalize (GObject *object)
{
  GVfsDaemonBackendTest *daemon;

  daemon = G_VFS_DAEMON_BACKEND_TEST (object);
  
  if (G_OBJECT_CLASS (g_vfs_daemon_backend_test_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_daemon_backend_test_parent_class)->finalize) (object);
}

static void
g_vfs_daemon_backend_test_class_init (GVfsDaemonBackendTestClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  //GVfsDaemonBackendClass *backend_class = G_VFS_DAEMON_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_daemon_backend_test_finalize;

  //backend_class->open_for_read = open_for_read;
}

static void
g_vfs_daemon_backend_test_init (GVfsDaemonBackendTest *daemon)
{
}

GVfsDaemonBackendTest *
g_vfs_daemon_backend_test_new (void)
{
  GVfsDaemonBackendTest *backend;
  backend = g_object_new (G_TYPE_VFS_DAEMON_BACKEND_TEST,
			 NULL);
  return backend;
}

