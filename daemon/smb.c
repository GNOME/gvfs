#include <config.h>

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus-gmain.h>
#include "gvfsdaemon.h"
#include "gdbusutils.h"
#include "gvfsbackendsmb.h"
#include <gvfsdaemonprotocol.h>

static DBusConnection *connection;

static void
send_done (const char *dbus_id,
	   const char *obj_path,
	   gboolean succeeded, GError *error)
{
  dbus_bool_t succeeded_dbus = succeeded;
  const char *domain;
  DBusMessage *message;
  guint32 code;

  if (dbus_id == NULL)
    return;
  
  message =
    dbus_message_new_method_call (dbus_id,
				  obj_path,
				  G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
				  "done");

  if (succeeded)
    _g_dbus_message_append_args (message,
				 DBUS_TYPE_BOOLEAN, &succeeded_dbus,
				 0);
  else
    {
      domain = g_quark_to_string (error->domain);
      code = error->code;
      
      _g_dbus_message_append_args (message,
				   DBUS_TYPE_BOOLEAN, &succeeded_dbus,
				   DBUS_TYPE_STRING, &domain,
				   DBUS_TYPE_UINT32, &code,
				   DBUS_TYPE_STRING, &error->message,
				   0);
    }
      
  dbus_connection_send_with_reply_and_block (connection, message, 2000, NULL);
}

int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  GVfsDaemon *daemon;
  GVfsBackendSmb *backend;
  const char *dbus_id, *obj_path;
  DBusMessage *message, *reply;
  DBusMessageIter iter;
  DBusError derror;
  GMountSpec *mount_spec;
  GError *error;

  g_thread_init (NULL);

  g_type_init ();

  dbus_error_init (&derror);
  connection = dbus_bus_get (DBUS_BUS_SESSION, &derror);
  if (connection == NULL)
    {
      g_print ("Error connecting dbus: %s\n", derror.message);
      dbus_error_free (&derror);
      return 1;
    }
  
  dbus_id = NULL;
  obj_path = NULL;
  if (strcmp (argv[1], "--mount") == 0)
    {
      if (argc < 4)
	{
	  g_print ("Args: --mount dbus-id object_path\n");
	  return 1;
	}
      
      dbus_id = argv[2];
      obj_path = argv[3];

      message =
	dbus_message_new_method_call (dbus_id,
				      obj_path,
				      G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
				      "getMountSpec");

      reply = dbus_connection_send_with_reply_and_block (connection, message, 2000, &derror);
      dbus_message_unref (message);
      if (reply == NULL)
	{
	  g_print ("Error requesting mount spec: %s\n", derror.message);
	  dbus_error_free (&derror);
	  return 1;
	}

      dbus_message_iter_init (reply, &iter);
      mount_spec = g_mount_spec_from_dbus (&iter);
      dbus_message_unref (reply);
    }
  else
    {
      if (argc < 3)
	{
	  g_print ("Args: server share\n");
	  
	  return 1;
	}
      mount_spec = g_mount_spec_new ("smb-share");
      g_mount_spec_set (mount_spec, "server", argv[1]);
      g_mount_spec_set (mount_spec, "share", argv[2]);
    }
  
  error = NULL;
  
  daemon = g_vfs_daemon_new (FALSE, FALSE);
  if (daemon == NULL)
    {
      g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "error starting mount daemon");
      send_done (dbus_id, obj_path, FALSE, error);
      return 1;
    }

  backend = g_vfs_backend_smb_new (mount_spec, &error);
  if (backend == NULL)
    {
      send_done (dbus_id, obj_path, FALSE, error);
      return 1;
    }
  
  g_vfs_backend_register_with_daemon (G_VFS_BACKEND (backend), daemon);
  g_object_unref (backend);

  /* TODO: Verify registration succeeded? */
  
  send_done (dbus_id, obj_path, TRUE, error);
  
  loop = g_main_loop_new (NULL, FALSE);

  g_print ("Entering mainloop\n");
  g_main_loop_run (loop);
  
  return 0;
}
