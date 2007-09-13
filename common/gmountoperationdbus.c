#include <config.h>

#include <string.h>

#include <dbus/dbus.h>

#include <gio/giotypes.h>
#include <gio/gioerror.h>
#include "gmountoperationdbus.h"
#include "gvfsdaemonprotocol.h"
#include "gdbusutils.h"
#include <glib/gi18n-lib.h>

typedef struct 
{
  GMountOperation *op;
  char *obj_path;
  char *dbus_id;
  DBusConnection *connection;
} GMountOperationDBus;

static DBusHandlerResult mount_op_message_function    (DBusConnection      *connection,
						       DBusMessage         *message,
						       void                *user_data);
static void              mount_op_unregister_function (DBusConnection      *connection,
						       void                *user_data);
static void              mount_op_ask_password        (GMountOperationDBus *op_dbus,
						       DBusMessage         *message);
static void              mount_op_ask_question        (GMountOperationDBus *op_dbus,
						       DBusMessage         *message);

static void
g_mount_operation_dbus_free (GMountOperationDBus *op_dbus)
{
  if (op_dbus->connection)
    {
      dbus_connection_unregister_object_path (op_dbus->connection,
					      op_dbus->obj_path);
      dbus_connection_unref (op_dbus->connection);
    }
  g_free (op_dbus->dbus_id);
  g_free (op_dbus->obj_path);
  g_free (op_dbus);
}

GMountSource *
g_mount_operation_dbus_wrap (GMountOperation *op)
{
  GMountOperationDBus *op_dbus;
  static int mount_id = 0;
  DBusObjectPathVTable mount_vtable = {
    mount_op_unregister_function,
    mount_op_message_function
  };

  op_dbus = g_new0 (GMountOperationDBus, 1);
  
  op_dbus->op = op;
  op_dbus->connection = dbus_bus_get (DBUS_BUS_SESSION, NULL);
  op_dbus->obj_path = g_strdup_printf ("/org/gtk/gvfs/mountop/%d", mount_id++);
  if (op_dbus->connection)
    {
      op_dbus->dbus_id = g_strdup (dbus_bus_get_unique_name (op_dbus->connection));
      if (!dbus_connection_register_object_path (op_dbus->connection,
						 op_dbus->obj_path,
						 &mount_vtable,
						 op_dbus))
	_g_dbus_oom ();
    }

  g_object_set_data_full (G_OBJECT (op), "dbus-op",
			  op_dbus, (GDestroyNotify)g_mount_operation_dbus_free);
  
  return g_mount_source_new (op_dbus->dbus_id, op_dbus->obj_path);
}

/**
 * Called when a #DBusObjectPathVTable is unregistered (or its connection is freed).
 * Found in #DBusObjectPathVTable.
 */
static void
mount_op_unregister_function (DBusConnection  *connection,
			      void            *user_data)
{
}

/**
 * Called when a message is sent to a registered object path. Found in
 * #DBusObjectPathVTable which is registered with dbus_connection_register_object_path()
 * or dbus_connection_register_fallback().
 */
static DBusHandlerResult
mount_op_message_function (DBusConnection  *connection,
			   DBusMessage     *message,
			   void            *user_data)
{
  GMountOperationDBus *op_dbus = user_data;
  
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
				   "askPassword"))
    mount_op_ask_password (op_dbus, message);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
					"askQuestion"))
    mount_op_ask_question (op_dbus, message);
  else
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  return DBUS_HANDLER_RESULT_HANDLED;
}

static void
mount_op_send_reply (GMountOperationDBus *op_dbus,
		     DBusMessage *reply)
{
  if (!dbus_connection_send (op_dbus->connection, reply, NULL))
    _g_dbus_oom ();

  g_signal_handlers_disconnect_matched (op_dbus->op,
					G_SIGNAL_MATCH_ID | G_SIGNAL_MATCH_DATA,
					g_signal_lookup ("reply", G_TYPE_MOUNT_OPERATION),
					0,
					NULL,
					NULL,
					reply);
  dbus_message_unref (reply);
}

static void
ask_password_reply (GMountOperation *op,
		    gboolean abort,
		    gpointer data)
{
  DBusMessage *reply = data;
  const char *username, *password, *domain;
  dbus_bool_t anonymous;
  guint32 password_save;
  dbus_bool_t handled = TRUE;
  dbus_bool_t abort_dbus = abort;
  GMountOperationDBus *op_dbus;

  op_dbus = g_object_get_data (G_OBJECT (op), "dbus-op");
  
  password = g_mount_operation_get_password (op);
  if (password == NULL)
    password = "";
  username = g_mount_operation_get_username (op);
  if (username == NULL)
    username = "";
  domain = g_mount_operation_get_domain (op);
  if (domain == NULL)
    domain = "";
  anonymous = g_mount_operation_get_anonymous (op);
  password_save = g_mount_operation_get_password_save (op);

  _g_dbus_message_append_args (reply,
			       DBUS_TYPE_BOOLEAN, &handled,
			       DBUS_TYPE_BOOLEAN, &abort_dbus,
			       DBUS_TYPE_STRING, &password,
			       DBUS_TYPE_STRING, &username,
			       DBUS_TYPE_STRING, &domain,
			       DBUS_TYPE_BOOLEAN, &anonymous,
			       DBUS_TYPE_UINT32, &password_save,
			       0);

  mount_op_send_reply (op_dbus, reply);
}

static void
mount_op_ask_password (GMountOperationDBus *op_dbus,
		       DBusMessage *message)
{
  const char *message_string, *default_user, *default_domain;
  dbus_bool_t handled = FALSE;
  guint32 flags;
  DBusMessageIter iter;
  DBusMessage *reply;
  DBusError error;
  gboolean res;

  reply = NULL;

  dbus_message_iter_init (message, &iter);
  
  dbus_error_init (&error);
  if (!_g_dbus_message_iter_get_args (&iter,
				      &error,
				      DBUS_TYPE_STRING, &message_string,
				      DBUS_TYPE_STRING, &default_user,
				      DBUS_TYPE_STRING, &default_domain,
				      DBUS_TYPE_UINT32, &flags,
				      0))
    {
      reply = dbus_message_new_error (message, error.name, error.message);
      if (reply == NULL)
	_g_dbus_oom ();
      if (!dbus_connection_send (op_dbus->connection, reply, NULL))
	_g_dbus_oom ();
      dbus_message_unref (reply);
      return;
    }
  
  reply = dbus_message_new_method_return (message);
  if (reply == NULL)
    _g_dbus_oom ();
  
  g_signal_connect (op_dbus->op, "reply", (GCallback)ask_password_reply, reply);
  
  g_signal_emit_by_name (op_dbus->op, "ask_password",
			 message_string,
			 default_user,
			 default_domain,
			 flags,
			 &res);

  if (!res)
    {
      _g_dbus_message_append_args (reply,
				   DBUS_TYPE_BOOLEAN, &handled,
				   0);
      mount_op_send_reply (op_dbus, reply);
    }
}

static void
ask_question_reply (GMountOperation *op,
		    gboolean abort,
		    gpointer data)
{
  DBusMessage *reply = data;
  guint32 choice;
  dbus_bool_t handled = TRUE;
  dbus_bool_t abort_dbus = abort;
  GMountOperationDBus *op_dbus;

  op_dbus = g_object_get_data (G_OBJECT (op), "dbus-op");

  choice = g_mount_operation_get_choice (op);

  _g_dbus_message_append_args (reply,
			       DBUS_TYPE_BOOLEAN, &handled,
			       DBUS_TYPE_BOOLEAN, &abort_dbus,
			       DBUS_TYPE_UINT32, &choice,
			       0);

  mount_op_send_reply (op_dbus, reply);
}

static void
mount_op_ask_question (GMountOperationDBus *op_dbus,
		       DBusMessage         *message)
{
  const char *message_string;
  char **choices;
  int num_choices;
  dbus_bool_t handled = FALSE;
  DBusMessage *reply;
  DBusError error;
  gboolean res;
  DBusMessageIter iter;

  reply = NULL;
  
  dbus_message_iter_init (message, &iter);
  dbus_error_init (&error);
  if (!_g_dbus_message_iter_get_args (&iter,
				      &error,
				      DBUS_TYPE_STRING, &message_string,
				      DBUS_TYPE_ARRAY, DBUS_TYPE_STRING,
				      &choices, &num_choices,
				      0))
    {
      reply = dbus_message_new_error (message, error.name, error.message);
      if (reply == NULL)
	_g_dbus_oom ();
      if (!dbus_connection_send (op_dbus->connection, reply, NULL))
	_g_dbus_oom ();
      dbus_message_unref (reply);
      return;
    }
  
  reply = dbus_message_new_method_return (message);
  if (reply == NULL)
    _g_dbus_oom ();
  
  g_signal_connect (op_dbus->op, "reply", (GCallback)ask_question_reply, reply);

  g_signal_emit_by_name (op_dbus->op, "ask_question",
			 message_string,
			 choices,
			 &res);
  if (!res)
    {
      _g_dbus_message_append_args (reply,
				   DBUS_TYPE_BOOLEAN, &handled,
				   0);
      mount_op_send_reply (op_dbus, reply);
    }
  
  dbus_free_string_array (choices);
}
