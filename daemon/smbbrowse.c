#include <config.h>

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus-gmain.h>
#include "gvfsdaemon.h"
#include "gdbusutils.h"
#include "gmountsource.h"
#include "gvfsbackendsmbbrowse.h"
#include <gvfsdaemonprotocol.h>

static void
dbus_mount (GVfsDaemon *daemon,
	    DBusConnection *connection,
	    DBusMessage *message)
{
  const char *dbus_id, *obj_path;
  DBusMessageIter iter;
  DBusError derror;
  DBusMessage *reply;
  GMountSpec *mount_spec;
  GMountSource *mount_source;

  dbus_id = dbus_message_get_sender (message);
  
  dbus_message_iter_init (message, &iter);

  mount_spec = NULL;
  dbus_error_init (&derror);
  if (!_g_dbus_message_iter_get_args (&iter, &derror,
				      DBUS_TYPE_OBJECT_PATH, &obj_path,
				      0))
    {
      reply = dbus_message_new_error (message, derror.name, derror.message);
      dbus_error_free (&derror);
    }
  else if ((mount_spec = g_mount_spec_from_dbus (&iter)) == NULL)
    reply = dbus_message_new_error (message,
				    DBUS_ERROR_INVALID_ARGS,
				    "Error in mount spec");
  else
    reply = dbus_message_new_method_return (message);
      
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);

  if (mount_spec)
    {
      mount_source = g_mount_source_new_dbus (dbus_id, obj_path, mount_spec);
      g_mount_spec_unref (mount_spec);

      g_vfs_daemon_initiate_mount (daemon, mount_source);
      g_object_unref (mount_source);
    }
}

static DBusHandlerResult
mountable_message_function (DBusConnection  *connection,
			    DBusMessage     *message,
			    void            *user_data)
{
  GVfsDaemon *daemon = user_data;

  DBusHandlerResult res;
  
  res = DBUS_HANDLER_RESULT_HANDLED;
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_MOUNTABLE_INTERFACE,
				   "mount"))
    dbus_mount (daemon, connection, message);
  else
    res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  
  return res;
}


struct DBusObjectPathVTable mountable_vtable = {
  NULL,
  mountable_message_function,
};

int
main (int argc, char *argv[])
{
  DBusConnection *connection;
  GMainLoop *loop;
  GVfsDaemon *daemon;
  DBusError derror;
  GMountSource *mount_source;
  GMountSpec *mount_spec;
  GError *error;
  int res;

  dbus_threads_init_default ();
  g_thread_init (NULL);
  g_type_init ();

  g_vfs_register_backend (G_TYPE_VFS_BACKEND_SMB_BROWSE, "smb-network");
  g_vfs_register_backend (G_TYPE_VFS_BACKEND_SMB_BROWSE, "smb-server");
  
  dbus_error_init (&derror);
  connection = dbus_bus_get (DBUS_BUS_SESSION, &derror);
  if (connection == NULL)
    {
      g_print ("Error connecting dbus: %s\n", derror.message);
      dbus_error_free (&derror);
      return 1;
    }
  
  if (argc > 1 && strcmp (argv[1], "--mount") == 0)
    {
      if (argc < 4)
	{
	  g_print ("Args: --mount dbus-id object_path\n");
	  return 1;
	}

      mount_source = g_mount_source_new_dbus (argv[2], argv[3], NULL);
    }
  else
    {
      if (argc > 1)
	{
	  mount_spec = g_mount_spec_new ("smb-server");
	  g_mount_spec_set (mount_spec, "server", argv[1]);
	}
      else
	mount_spec = g_mount_spec_new ("smb-network");

      mount_source = g_mount_source_new_null (mount_spec);
      g_mount_spec_unref (mount_spec);
    }

  res = dbus_bus_request_name (connection,
			       "org.gtk.vfs.mountpoint.smb_browse",
			       0, &derror);

  error = NULL;
  
  if (res != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
    {
      if (res == -1)
	_g_error_from_dbus (&derror, &error);
      else
	g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_IO,
		     "smb-browser already running");
      g_mount_source_failed (mount_source, error);
      return 1;
    }
  
  daemon = g_vfs_daemon_new (FALSE, FALSE);
  if (daemon == NULL)
    {
      g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "error starting mount daemon");
      g_mount_source_failed (mount_source, error);
      return 1;
    }

  g_vfs_daemon_initiate_mount (daemon, mount_source);
  g_object_unref (mount_source);
  
  if (!dbus_connection_register_object_path (connection,
					     "/org/gtk/vfs/mountpoint/smb_browse",
					     &mountable_vtable, daemon))
    _g_dbus_oom ();

  
  loop = g_main_loop_new (NULL, FALSE);

  g_print ("Entering mainloop\n");
  g_main_loop_run (loop);
  
  return 0;
}
