#include <config.h>

#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <dbus-gmain.h>
#include "gvfsdaemon.h"
#include "gdbusutils.h"
#include "gvfsbackendsmbbrowse.h"
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
    {
      if (!succeeded)
	g_print ("Error mounting: %s\n", error->message);
      return;
    }
  
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
  gboolean start_op;

  dbus_id = dbus_message_get_sender (message);
  
  dbus_message_iter_init (message, &iter);

  mount_spec = NULL;
  start_op = FALSE;
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
    {
      reply = dbus_message_new_method_return (message);
      start_op = TRUE;
    }
      
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);

  /* Make sure we send the reply so the sender doesn't timeout
   * while we mount stuff */
  dbus_connection_flush(connection);

  if (start_op)
    {
      GVfsBackendSmbBrowse *backend;
      GError *error;

      error = NULL;
      backend = g_vfs_backend_smb_browse_new (mount_spec, &error);
      if (backend == NULL)
	send_done (dbus_id, obj_path, FALSE, error);
      else
	{      
	  g_vfs_backend_register_with_daemon (G_VFS_BACKEND (backend), daemon);
	  g_object_unref (backend);
      
	  /* TODO: Verify registration succeeded? */
      
	  send_done (dbus_id, obj_path, TRUE, NULL);
	}
    }

  if (mount_spec)
    g_mount_spec_unref (mount_spec);
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
  GMainLoop *loop;
  GVfsDaemon *daemon;
  GVfsBackendSmbBrowse *backend;
  const char *dbus_id, *obj_path;
  DBusMessage *message, *reply;
  DBusMessageIter iter;
  DBusError derror;
  GMountSpec *mount_spec;
  GError *error;
  int res;

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
  if (argc > 1 && strcmp (argv[1], "--mount") == 0)
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
      if (argc > 1)
	{
	  mount_spec = g_mount_spec_new ("smb-server");
	  g_mount_spec_set (mount_spec, "server", argv[1]);
	}
      else
	mount_spec = g_mount_spec_new ("smb-network");
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
      send_done (dbus_id, obj_path, FALSE, error);
      return 1;
    }
  
  daemon = g_vfs_daemon_new (FALSE, FALSE);
  if (daemon == NULL)
    {
      g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "error starting mount daemon");
      send_done (dbus_id, obj_path, FALSE, error);
      return 1;
    }

  backend = g_vfs_backend_smb_browse_new (mount_spec, &error);
  if (backend == NULL)
    {
      send_done (dbus_id, obj_path, FALSE, error);
      return 1;
    }
  
  g_vfs_backend_register_with_daemon (G_VFS_BACKEND (backend), daemon);
  g_object_unref (backend);

  /* TODO: Verify registration succeeded? */
  
  send_done (dbus_id, obj_path, TRUE, NULL);

  if (!dbus_connection_register_object_path (connection,
					     "/org/gtk/vfs/mountpoint/smb_browse",
					     &mountable_vtable, daemon))
    _g_dbus_oom ();

  
  loop = g_main_loop_new (NULL, FALSE);

  g_print ("Entering mainloop\n");
  g_main_loop_run (loop);
  
  return 0;
}
