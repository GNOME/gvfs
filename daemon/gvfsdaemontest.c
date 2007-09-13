#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include "gvfsdaemontest.h"

G_DEFINE_TYPE (GVfsDaemonTest, g_vfs_daemon_test, G_TYPE_VFS_DAEMON);

static GVfsReadRequest * read_file (GVfsDaemon *daemon,
				    const char *path,
				    GError **error);


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
  GVfsDaemonClass *daemon_class = G_VFS_DAEMON_CLASS (klass);
  
  gobject_class->finalize = g_vfs_daemon_test_finalize;

  daemon_class->read_file = read_file;
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

static GVfsReadRequest *
read_file (GVfsDaemon *daemon,
	   const char *path,
	   GError **error)
{
  GVfsReadRequest *read_request;
  
  g_print ("read_file: %s\n", path);
  
  read_request = g_vfs_read_request_new (error);
  return read_request;
}
