#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include "gvfsdaemontest.h"

G_DEFINE_TYPE (GVfsDaemonTest, g_vfs_daemon_test, G_TYPE_VFS_DAEMON);

static void read_file (GVfsDaemon *daemon,
		       DBusMessage *reply,
		       const char *path,
		       int *socket_fd_out);


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

static void 
read_file (GVfsDaemon *daemon,
	   DBusMessage *reply,
	   const char *path,
	   int *socket_fd_out)
{
  char *str = "YAY";
  int socket_fds[2];
  int ret;
  
  g_print ("read_file: %s\n", path);

  dbus_message_append_args (reply,
			    DBUS_TYPE_STRING, &str,
			    DBUS_TYPE_INVALID);
  
  ret = socketpair (AF_UNIX, SOCK_STREAM, 0, socket_fds);
  *socket_fd_out = socket_fds[0];
}
