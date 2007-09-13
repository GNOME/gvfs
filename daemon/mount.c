#include <config.h>

#include <string.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "mount.h"
#include "gmountoperationdbus.h"
#include "gvfsdaemonprotocol.h"
#include "gdbusutils.h"

struct _Mountable {
  char *type;
  char *exec;
  char *dbus_name;
  gboolean automount;
}; 

static GList *mountables;

void
mount_init (void)
{
  GDir *dir;
  char *mount_dir, *path;
  const char *filename;
  GKeyFile *keyfile;
  char **types;
  Mountable *mountable;
  int i;
  
  mount_dir = MOUNTABLE_DIR;
  dir = g_dir_open (mount_dir, 0, NULL);

  if (dir)
    {
      while ((filename = g_dir_read_name (dir)) != NULL)
	{
	  path = g_build_filename (mount_dir, filename, NULL);
	  
	  keyfile = g_key_file_new ();
	  if (g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, NULL))
	    {
	      types = g_key_file_get_string_list (keyfile, "Mount", "Type", NULL, NULL);
	      if (types != NULL)
		{
		  for (i = 0; types[i] != NULL; i++)
		    {
		      if (*types[i] != 0)
			{
			  mountable = g_new0 (Mountable, 1);
			  mountable->type = g_strdup (types[i]);
			  mountable->exec = g_key_file_get_string (keyfile, "Mount", "Exec", NULL);
			  mountable->dbus_name = g_key_file_get_string (keyfile, "Mount", "DBusName", NULL);
			  mountable->automount = g_key_file_get_boolean (keyfile, "Mount", "AutoMount", NULL);
			  
			  mountables = g_list_prepend (mountables, mountable);
			}
		    }
		  g_strfreev (types);
		}
	    }
	  g_key_file_free (keyfile);
	  g_free (path);
	}
    }
}

gboolean
mountable_is_automount (Mountable *mountable)
{
  return mountable->automount;
}

static Mountable *
find_mountable (const char *type)
{
  GList *l;

  for (l = mountables; l != NULL; l = l->next)
    {
      Mountable *mountable = l->data;

      if (strcmp (mountable->type, type) == 0)
	return mountable;
    }
  
  return NULL;
}

Mountable *
lookup_mountable (GMountSpec *spec)
{
  const char *type;
  
  type = g_mount_spec_get_type (spec);
  if (type == NULL)
    return NULL;

  return find_mountable (type);
}

static void
exec_mount (GMountOperationDBus *op,
	    Mountable *mountable)
{
  DBusConnection *conn;
  const char *id;
  char *exec;
  GError *error;
  
  if (mountable->exec == NULL)
    {
      error = NULL;
      g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "No exec key defined for mountpoint");
      g_mount_operation_dbus_fail_at_idle (op, error);
      g_error_free (error);
      return;
    }

  conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);
  id = dbus_bus_get_unique_name (conn);
  
  exec = g_strconcat (mountable->exec, " ", id, " ", op->obj_path, NULL);
  
  dbus_connection_unref (conn);

  if (!g_spawn_command_line_async (exec, &error))
    {
      g_mount_operation_dbus_fail_at_idle (op, error);
      g_error_free (error);
    }
  
  g_free (exec);
}

static void
dbus_mount_reply (DBusPendingCall *pending,
		  void            *user_data)
{
  Mountable *mountable;
  DBusMessage *reply;
  GError *error;
  GMountOperationDBus *op = user_data;

  reply = dbus_pending_call_steal_reply (pending);
  dbus_pending_call_unref (pending);

  if (dbus_message_is_error (reply, DBUS_ERROR_NAME_HAS_NO_OWNER) ||
      dbus_message_is_error (reply, DBUS_ERROR_SERVICE_UNKNOWN))
    {
      mountable = g_object_get_data (G_OBJECT (op), "mountable");
      exec_mount (op, mountable);
    }
  else
    {
      error = NULL;
      if (_g_error_from_message (reply, &error))
	{
	  g_mount_operation_dbus_fail_at_idle (op, error);
	  g_error_free (error);
	}
    }
  
  dbus_message_unref (reply);
}

GMountOperation *
mountable_mount (Mountable *mountable,
		 GMountSpec *spec,
		 gboolean is_automount)
{
  DBusConnection *conn;
  GMountOperationDBus *op;
  DBusMessage *message;
  DBusMessageIter iter;
  DBusPendingCall *pending;
  dbus_bool_t automount;

  op = g_mount_operation_dbus_new (spec);
  g_object_set_data (G_OBJECT (op), "mountable", mountable);
  
  if (mountable->dbus_name)
    {
      message = dbus_message_new_method_call (mountable->dbus_name,
					      G_VFS_DBUS_MOUNTABLE_PATH,
					      G_VFS_DBUS_MOUNTABLE_INTERFACE,
					      "mount");
      automount = is_automount;
      if (!dbus_message_append_args (message,
				     DBUS_TYPE_OBJECT_PATH, &op->obj_path,
				     DBUS_TYPE_BOOLEAN, &automount,
				     0))
	_g_dbus_oom ();
      
      dbus_message_iter_init_append (message, &iter);
      g_mount_spec_to_dbus (&iter, spec);
      
      conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);

      if (!dbus_connection_send_with_reply (conn, message,
					    &pending,
					    2000))
	_g_dbus_oom ();

      dbus_connection_unref (conn);
      dbus_message_unref (message);

      if (pending == NULL)
	{
	  GError *error = NULL;
	  g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       "Error while getting peer-to-peer dbus connection: %s",
		       "Connection is closed");
	  g_mount_operation_dbus_fail_at_idle (op, error);
	  g_error_free (error);
	}
      else if (!dbus_pending_call_set_notify (pending,
					      dbus_mount_reply,
					      g_object_ref (op),
					      g_object_unref))
	_g_dbus_oom ();
    }
  else
    exec_mount (op, mountable);
  
  return G_MOUNT_OPERATION (op);
}
