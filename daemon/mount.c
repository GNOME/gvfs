#include <config.h>

#include <string.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "mount.h"
#include "gmountoperationdbus.h"
#include "gvfsdaemonprotocol.h"
#include "gdbusutils.h"
#include <gio/gioerror.h>

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

typedef struct {
  Mountable *mountable;
  dbus_bool_t automount;
  GMountSource *source;
  GMountSpec *mount_spec;
  MountCallback callback;
  gpointer user_data;
  char *obj_path;
  gboolean spawned;
} MountData;

static void spawn_mount (MountData *data);

static void
mount_data_free (MountData *data)
{
  g_object_unref (data->source);
  g_mount_spec_unref (data->mount_spec);
  g_free (data->obj_path);
  
  g_free (data);
}

static void
mount_finish (MountData *data, GError *error)
{
  data->callback (data->mountable, error, data->user_data);
  mount_data_free (data);
}

static void
dbus_mount_reply (DBusPendingCall *pending,
		  void            *_data)
{
  DBusMessage *reply;
  GError *error;
  MountData *data = _data;

  reply = dbus_pending_call_steal_reply (pending);
  dbus_pending_call_unref (pending);

  if ((dbus_message_is_error (reply, DBUS_ERROR_NAME_HAS_NO_OWNER) ||
       dbus_message_is_error (reply, DBUS_ERROR_SERVICE_UNKNOWN)) &&
      !data->spawned)
    spawn_mount (data);
  else
    {
      error = NULL;
      if (_g_error_from_message (reply, &error))
	{
	  mount_finish (data, error);
	  g_error_free (error);
	}
      else
	mount_finish (data, NULL);
    }
  
  dbus_message_unref (reply);
}

static void
mountable_mount_with_name (MountData *data,
			   const char *dbus_name)
{
  DBusConnection *conn;
  DBusMessage *message;
  DBusPendingCall *pending;
  GError *error = NULL;
  DBusMessageIter iter;

  conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);
  message = dbus_message_new_method_call (dbus_name,
					  G_VFS_DBUS_MOUNTABLE_PATH,
					  G_VFS_DBUS_MOUNTABLE_INTERFACE,
					  "mount");

  dbus_message_iter_init_append (message, &iter);
  g_mount_spec_to_dbus (&iter, data->mount_spec);

  _g_dbus_message_append_args (message,
			       DBUS_TYPE_BOOLEAN, &data->automount,
			       0);
  
  g_mount_source_to_dbus (data->source, message);
  
  if (!dbus_connection_send_with_reply (conn, message,
					&pending,
					2000))
    _g_dbus_oom ();
  
  dbus_message_unref (message);
  dbus_connection_unref (conn);
  
  if (pending == NULL)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "Error while getting peer-to-peer dbus connection: %s",
		   "Connection is closed");
      mount_finish (data, error);
      g_error_free (error);
      return;
    }
  
  if (!dbus_pending_call_set_notify (pending,
				     dbus_mount_reply,
				     data, NULL))
    _g_dbus_oom ();
}

static void
spawn_mount_unregister_function (DBusConnection  *connection,
				 void            *user_data)
{
}

static DBusHandlerResult
spawn_mount_message_function (DBusConnection  *connection,
			      DBusMessage     *message,
			      void            *user_data)
{
  MountData *data = user_data;
  GError *error = NULL;
  dbus_bool_t succeeded;
  char *error_message;

  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_SPAWNER_INTERFACE,
				   "spawned"))
    {
      dbus_connection_unregister_object_path (connection, data->obj_path);

      if (!dbus_message_get_args (message, NULL,
				  DBUS_TYPE_BOOLEAN, &succeeded,
				  DBUS_TYPE_STRING, &error_message,
				  DBUS_TYPE_INVALID))
	{
	  g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
		       _("Invalid arguments from spawned child"));
	  mount_finish (data, error);
	  g_error_free (error);
	}
      else if (!succeeded)
	{
	  g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, error_message);
	  mount_finish (data, error);
	  g_error_free (error);
	}
      else
	mountable_mount_with_name (data, dbus_message_get_sender (message));
      
      return DBUS_HANDLER_RESULT_HANDLED;
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void
spawn_mount (MountData *data)
{
  char *exec;
  GError *error;
  DBusConnection *connection;
  static int mount_id = 0;
  DBusObjectPathVTable spawn_vtable = {
    spawn_mount_unregister_function,
    spawn_mount_message_function
  };

  data->spawned = TRUE;
  
  error = NULL;
  if (data->mountable->exec == NULL)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "No exec key defined for mountpoint");
      mount_finish (data, error);
      g_error_free (error);
    }
  else
    {
      data->obj_path = g_strdup_printf ("/org/gtk/gvfs/exec_spaw/%d", mount_id++);

      connection = dbus_bus_get (DBUS_BUS_SESSION, NULL);
      if (!dbus_connection_register_object_path (connection,
						 data->obj_path,
						 &spawn_vtable,
						 data))
	_g_dbus_oom ();
      
      exec = g_strconcat (data->mountable->exec, " --spawner ", dbus_bus_get_unique_name (connection), " ", data->obj_path, NULL);

      if (!g_spawn_command_line_async (exec, &error))
	{
	  dbus_connection_unregister_object_path (connection, data->obj_path);
	  mount_finish (data, error);
	  g_error_free (error);
	}
      
      /* TODO: Add a timeout here to detect spawned app crashing */
      
      dbus_connection_unref (connection);
      g_free (exec);
    }
}

void
mountable_mount (Mountable *mountable,
		 GMountSpec *mount_spec,
		 GMountSource *source,
		 gboolean automount,
		 MountCallback callback,
		 gpointer user_data)
{
  MountData *data;

  data = g_new0 (MountData, 1);
  data->automount = automount;
  data->mountable = mountable;
  data->source = g_object_ref (source);
  data->mount_spec = g_mount_spec_ref (mount_spec);
  data->callback = callback;
  data->user_data = user_data;

  if (mountable->dbus_name == NULL)
    spawn_mount (data);
  else
    mountable_mount_with_name (data, mountable->dbus_name);
  
}
