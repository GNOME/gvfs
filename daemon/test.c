#include <config.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus-gmain.h>
#include "gvfsdaemon.h"
#include "gvfsbackendtest.h"
#include <gvfsdaemonprotocol.h>

int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  GVfsDaemon *daemon;
  GMountSpec *mount_spec;
  GMountSource *mount_source;

  dbus_threads_init_default ();
  g_thread_init (NULL);
  g_type_init ();

  g_vfs_register_backend (G_TYPE_VFS_BACKEND_TEST, "test");

  daemon = g_vfs_daemon_new (FALSE, FALSE);
  if (daemon == NULL)
    return 1;

  mount_spec = g_mount_spec_new ("test");
  mount_source = g_mount_source_new_null (mount_spec);
  g_mount_spec_unref (mount_spec);
  g_vfs_daemon_initiate_mount (daemon, mount_source);
  g_object_unref (mount_source);

  loop = g_main_loop_new (NULL, FALSE);

  g_print ("Entering mainloop\n");
  g_main_loop_run (loop);
  
  return 0;
}
