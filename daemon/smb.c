#include <config.h>

#include <stdlib.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus-gmain.h>
#include "gvfsdaemon.h"
#include "gvfsbackendsmb.h"
#include <gvfsdaemonprotocol.h>

int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  GVfsDaemon *daemon;
  GVfsBackendSmb *backend;
  const char *server, *share;

  if (argc < 3)
    {
      g_print ("Args: server share\n");
      return 0;
    }
  
  server = argv[1];
  share = argv[2];
  
  g_thread_init (NULL);

  g_type_init ();

  daemon = g_vfs_daemon_new (FALSE, FALSE);
  if (daemon == NULL)
    return 1;

  backend = g_vfs_backend_smb_new (server, share);

  if (backend == NULL)
    {
      g_print ("Failed instantiating backend\n");
      return 1;
    }
  
  g_vfs_backend_register_with_daemon (G_VFS_BACKEND (backend), daemon);
  g_object_unref (backend);
  
  loop = g_main_loop_new (NULL, FALSE);

  g_print ("Entering mainloop\n");
  g_main_loop_run (loop);
  
  return 0;
}
