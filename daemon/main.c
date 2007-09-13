#include <config.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus-gmain.h>
#include "gvfsdaemontest.h"
#include <gvfsdaemonprotocol.h>

static gboolean
init_dbus (void)
{
  DBusConnection *conn;
  DBusError error;

  dbus_error_init (&error);

  conn = dbus_bus_get (DBUS_BUS_SESSION, &error);
  if (!conn)
    {
      g_printerr ("Failed to connect to the D-BUS daemon: %s\n",
		  error.message);
      
      dbus_error_free (&error);
      return FALSE;
    }

  dbus_connection_setup_with_g_main (conn, NULL);

  return TRUE;
}


int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  GVfsDaemonTest *daemon;
  
  g_type_init ();

  if (!init_dbus ())
    return 1;

  daemon = g_vfs_daemon_test_new (G_VFS_DBUS_MOUNTPOINT_NAME "foo_3A_2F_2F");

  if (!g_vfs_daemon_is_active (G_VFS_DAEMON (daemon)))
    return 1;

  loop = g_main_loop_new (NULL, FALSE);

  g_print ("Entering mainloop\n");
  g_main_loop_run (loop);
  
  return 0;
}
