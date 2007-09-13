#include <config.h>

#include <glib.h>
#include <dbus/dbus.h>
#include "gvfsdaemontest.h"

G_DEFINE_TYPE (GVfsDaemonTest, g_vfs_daemon_test, G_TYPE_VFS_DAEMON);

static void
g_vfs_daemon_test_finalize (GObject *object)
{
  GVfsDaemonTest *daemon;

  daemon = G_VFS_DAEMON_TEST (object);
  
  if (G_OBJECT_CLASS (g_vfs_daemon_test_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_daemon_test_parent_class)->finalize) (object);
}

static void
g_vfs_daemon_test_class_init (GVfsDaemonTestClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_vfs_daemon_test_finalize;
}

static void
g_vfs_daemon_test_init (GVfsDaemonTest *daemon)
{
}

GVfsDaemonTest *
g_vfs_daemon_test_new (const char *mountpoint)
{
  GVfsDaemonTest *daemon;

  daemon = g_object_new (G_TYPE_VFS_DAEMON_TEST,
			 "mountpoint", mountpoint,
			 NULL);

  return daemon;
}
