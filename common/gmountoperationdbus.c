#include <config.h>

#include <string.h>

#include <gio/gvfstypes.h>
#include "gmountoperationdbus.h"
#include "gvfsdaemonprotocol.h"
#include "gdbusutils.h"
#include <glib/gi18n-lib.h>

G_DEFINE_TYPE (GMountOperationDBus, g_mount_operation_dbus, G_TYPE_MOUNT_OPERATION);

static DBusHandlerResult mount_op_message_function    (DBusConnection      *connection,
						       DBusMessage         *message,
						       void                *user_data);
static void              mount_op_unregister_function (DBusConnection      *connection,
						       void                *user_data);
static void              mount_op_ask_password        (GMountOperationDBus *op,
						       DBusMessage         *message);
static void              mount_op_ask_question        (GMountOperationDBus *op,
						       DBusMessage         *message);
static void              mount_op_done                (GMountOperationDBus *op,
						       DBusMessage         *message);
static void              mount_op_get_mount_spec      (GMountOperationDBus *op,
						       DBusMessage         *message);

static void
g_mount_operation_dbus_finalize (GObject *object)
{
  GMountOperationDBus *operation;

  operation = G_MOUNT_OPERATION_DBUS (object);

  dbus_connection_unregister_object_path (operation->connection,
					  operation->obj_path);
  g_free (operation->obj_path);
  dbus_connection_unref (operation->connection);
  
  if (G_OBJECT_CLASS (g_mount_operation_dbus_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_mount_operation_dbus_parent_class)->finalize) (object);
}


static void
g_mount_operation_dbus_class_init (GMountOperationDBusClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_mount_operation_dbus_finalize;
}

static void
g_mount_operation_dbus_init (GMountOperationDBus *operation)
{
  static int mount_id = 0;
  DBusObjectPathVTable mount_vtable = {
    mount_op_unregister_function,
    mount_op_message_function
  };

  operation->connection = dbus_bus_get (DBUS_BUS_SESSION, NULL);
  operation->obj_path = g_strdup_printf ("/org/gtk/gvfs/mountop/%d", mount_id++);

  if (!dbus_connection_register_object_path (operation->connection,
					     operation->obj_path,
					     &mount_vtable,
					     operation))
    _g_dbus_oom ();
}

GMountOperationDBus *
g_mount_operation_dbus_new (GMountSpec *spec)
{
  GMountOperationDBus *op;

  op = g_object_new (G_TYPE_MOUNT_OPERATION_DBUS, NULL);
  op->mount_spec = g_mount_spec_ref (spec);
  
  return op;
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
  GMountOperationDBus *op = user_data;
  
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
				   "askPassword"))
    mount_op_ask_password (op, message);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
					"askQuestion"))
    mount_op_ask_question (op, message);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
					"done"))
    mount_op_done (op, message);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
					"getMountSpec"))
    mount_op_get_mount_spec (op, message);
  else
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  return DBUS_HANDLER_RESULT_HANDLED;
}

static void
mount_op_get_mount_spec (GMountOperationDBus *op,
			 DBusMessage *message)
{
  DBusMessage *reply;
  DBusMessageIter iter;
  
  reply = dbus_message_new_method_return (message);
  
  dbus_message_iter_init_append (reply, &iter);
  g_mount_spec_to_dbus (&iter, op->mount_spec);

  if (!dbus_connection_send (op->connection, reply, NULL))
    _g_dbus_oom ();
}

static void
mount_op_send_reply (GMountOperationDBus *op,
		     DBusMessage *reply)
{
  if (!dbus_connection_send (op->connection, reply, NULL))
    _g_dbus_oom ();

  g_signal_handlers_disconnect_matched (op,
					G_SIGNAL_MATCH_ID | G_SIGNAL_MATCH_DATA,
					g_signal_lookup ("reply", G_TYPE_MOUNT_OPERATION),
					0,
					NULL,
					NULL,
					reply);
  dbus_message_unref (reply);
}

static void
ask_password_reply (GMountOperationDBus *op_dbus,
		    gboolean abort,
		    gpointer data)
{
  DBusMessage *reply = data;
  const char *username, *password, *domain;
  dbus_bool_t anonymous;
  guint32 password_save;
  dbus_bool_t handled = TRUE;
  dbus_bool_t abort_dbus = abort;
  GMountOperation *op;

  op = G_MOUNT_OPERATION (op_dbus);
  
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
mount_op_ask_password (GMountOperationDBus *op,
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
      if (!dbus_connection_send (op->connection, reply, NULL))
	_g_dbus_oom ();
      dbus_message_unref (reply);
      return;
    }
  
  reply = dbus_message_new_method_return (message);
  if (reply == NULL)
    _g_dbus_oom ();
  
  g_signal_connect (op, "reply", (GCallback)ask_password_reply, reply);
  
  g_signal_emit_by_name (op, "ask_password",
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
      mount_op_send_reply (op, reply);
    }
}

static void
ask_question_reply (GMountOperationDBus *op_dbus,
		    gboolean abort,
		    gpointer data)
{
  DBusMessage *reply = data;
  guint32 choice;
  dbus_bool_t handled = TRUE;
  dbus_bool_t abort_dbus = abort;
  GMountOperation *op;

  op = G_MOUNT_OPERATION (op_dbus);

  choice = g_mount_operation_get_choice (op);

  _g_dbus_message_append_args (reply,
			       DBUS_TYPE_BOOLEAN, &handled,
			       DBUS_TYPE_BOOLEAN, &abort_dbus,
			       DBUS_TYPE_UINT32, &choice,
			       0);

  mount_op_send_reply (op_dbus, reply);
}

static void
mount_op_ask_question (GMountOperationDBus *op,
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
      if (!dbus_connection_send (op->connection, reply, NULL))
	_g_dbus_oom ();
      dbus_message_unref (reply);
      return;
    }
  
  reply = dbus_message_new_method_return (message);
  if (reply == NULL)
    _g_dbus_oom ();
  
  g_signal_connect (op, "reply", (GCallback)ask_question_reply, reply);

  g_signal_emit_by_name (op, "ask_question",
			 message_string,
			 choices,
			 &res);
  if (!res)
    {
      _g_dbus_message_append_args (reply,
				   DBUS_TYPE_BOOLEAN, &handled,
				   0);
      mount_op_send_reply (op, reply);
    }
  
  dbus_free_string_array (choices);
}
  
static void
mount_op_done (GMountOperationDBus *op,
	       DBusMessage         *message)
{
  const char *domain, *error_message;
  dbus_bool_t success;
  DBusMessage *reply;
  DBusMessageIter iter;
  DBusError derror;
  guint32 code;
  GError *error;

  reply = NULL;
  
  dbus_message_iter_init (message, &iter);
  
  dbus_error_init (&derror);
  if (!_g_dbus_message_iter_get_args (&iter,
				      &derror,
				      DBUS_TYPE_BOOLEAN, &success,
				      0))
    {
      reply = dbus_message_new_error (message, derror.name, derror.message);
      if (reply == NULL)
	_g_dbus_oom ();
      if (!dbus_connection_send (op->connection, reply, NULL))
	_g_dbus_oom ();
      dbus_message_unref (reply);
      return;
    }

  error = NULL;
  if (!success)
    {
      if (!_g_dbus_message_iter_get_args (&iter,
					  &derror,
					  DBUS_TYPE_STRING, &domain,
					  DBUS_TYPE_UINT32, &code,
					  DBUS_TYPE_STRING, &error_message,
					  0))
	{
	  reply = dbus_message_new_error (message, derror.name, derror.message);
	  if (reply == NULL)
	    _g_dbus_oom ();
	  if (!dbus_connection_send (op->connection, reply, NULL))
	    _g_dbus_oom ();
	  dbus_message_unref (reply);
	  return;
	}

      error = g_error_new_literal (g_quark_from_string (domain),
				   code, error_message);
    }
  
  
  g_signal_emit_by_name (op, "done", success, error);

  if (error)
    g_error_free (error);

  reply = dbus_message_new_method_return (message);
  if (reply == NULL)
    _g_dbus_oom ();

  if (!dbus_connection_send (op->connection, reply, NULL))
    _g_dbus_oom ();
  dbus_message_unref (reply);
}

struct FailData {
  GMountOperationDBus *op;
  GError *error;
};

static void
fail_data_free (struct FailData *data)
{
  g_object_unref (data->op);
  g_error_free (data->error);
  g_free (data);
}

static gboolean
fail_at_idle (struct FailData *data)
{
  g_signal_emit_by_name (data->op, "done", FALSE, data->error);
  return FALSE;
}

void
g_mount_operation_dbus_fail_at_idle (GMountOperationDBus *op,
				     GError     *error)
{
  struct FailData *data;

  data = g_new (struct FailData, 1);
  data->op = g_object_ref (op);
  data->error = g_error_copy (error);
  
  g_idle_add_full (G_PRIORITY_DEFAULT,
		   (GSourceFunc)fail_at_idle,
		   data, (GDestroyNotify)fail_data_free);
}
