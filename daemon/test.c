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
  GVfsBackendTest *backend;

  g_thread_init (NULL);

  g_type_init ();

  daemon = g_vfs_daemon_new (FALSE, FALSE);
  if (daemon == NULL)
    return 1;

  backend = g_vfs_backend_test_new ();
  g_vfs_backend_register_with_daemon (G_VFS_BACKEND (backend), daemon);
  g_object_unref (backend);
  
  loop = g_main_loop_new (NULL, FALSE);

  g_print ("Entering mainloop\n");
  g_main_loop_run (loop);
  
  return 0;
}
