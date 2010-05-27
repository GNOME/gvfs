/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "mount.h"
#include "gmountoperationdbus.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsdbusutils.h"
#include <glib.h>
#include <gio/gio.h>

typedef struct {
  char *display_name;
  char *stable_name;
  char *x_content_types;
  char *icon;
  char *prefered_filename_encoding;
  gboolean user_visible;
  char *default_location;
  char *fuse_mountpoint; /* Always set, even if fuse not availible */
  
  /* Daemon object ref */
  char *dbus_id;
  char *object_path;

  /* Mount details */
  GMountSpec *mount_spec;
} VfsMount;

typedef struct  {
  char *type;
  char *exec;
  char *dbus_name;
  gboolean automount;
  char *scheme;
  char **scheme_aliases;
  int default_port;
  gboolean hostname_is_inet;
} VfsMountable; 

typedef void (*MountCallback) (VfsMountable *mountable,
			       GError *error,
			       gpointer user_data);

static GList *mountables = NULL;
static GList *mounts = NULL;

static gboolean fuse_available;

static void lookup_mount (DBusConnection *connection,
			  DBusMessage *message,
			  gboolean do_automount);
static void mountable_mount (VfsMountable *mountable,
			     GMountSpec *mount_spec,
			     GMountSource *source,
			     gboolean automount,
			     MountCallback callback,
			     gpointer user_data);

static VfsMount *
find_vfs_mount (const char *dbus_id,
		const char *obj_path)
{
  GList *l;
  for (l = mounts; l != NULL; l = l->next)
    {
      VfsMount *mount = l->data;

      if (strcmp (mount->dbus_id, dbus_id) == 0 &&
	  strcmp (mount->object_path, obj_path) == 0)
	return mount;
    }
  
  return NULL;
}

static VfsMount *
find_vfs_mount_by_fuse_path (const char *fuse_path)
{
  GList *l;

  if (!fuse_available)
    return NULL;
  
  for (l = mounts; l != NULL; l = l->next)
    {
      VfsMount *mount = l->data;

      if (mount->fuse_mountpoint != NULL &&
	  g_str_has_prefix (fuse_path, mount->fuse_mountpoint))
	{
	  int len = strlen (mount->fuse_mountpoint);
	  if (fuse_path[len] == 0 ||
	      fuse_path[len] == '/')
	    return mount;
	}
    }
  
  return NULL;
}


static VfsMount *
match_vfs_mount (GMountSpec *match)
{
  GList *l;
  for (l = mounts; l != NULL; l = l->next)
    {
      VfsMount *mount = l->data;

      if (g_mount_spec_match (mount->mount_spec, match))
	return mount;
    }
  
  return NULL;
}

static VfsMountable *
find_mountable (const char *type)
{
  GList *l;

  for (l = mountables; l != NULL; l = l->next)
    {
      VfsMountable *mountable = l->data;

      if (strcmp (mountable->type, type) == 0)
	return mountable;
    }
  
  return NULL;
}

static VfsMountable *
lookup_mountable (GMountSpec *spec)
{
  const char *type;
  
  type = g_mount_spec_get_type (spec);
  if (type == NULL)
    return NULL;

  return find_mountable (type);
}

static void
vfs_mountable_free (VfsMountable *mountable)
{
  g_free (mountable->type);
  g_free (mountable->exec);
  g_free (mountable->dbus_name);
  g_free (mountable->scheme);
  g_strfreev (mountable->scheme_aliases);
  g_free (mountable);
}

static void
vfs_mount_free (VfsMount *mount)
{
  g_free (mount->display_name);
  g_free (mount->stable_name);
  g_free (mount->x_content_types);
  g_free (mount->icon);
  g_free (mount->fuse_mountpoint);
  g_free (mount->prefered_filename_encoding);
  g_free (mount->default_location);
  g_free (mount->dbus_id);
  g_free (mount->object_path);
  g_mount_spec_unref (mount->mount_spec);
  
  g_free (mount);
}

static void
vfs_mount_to_dbus (VfsMount *mount,
		   DBusMessageIter *iter)
{
  DBusMessageIter struct_iter;
  dbus_bool_t user_visible;
  char *fuse_mountpoint;
  
  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_STRUCT,
					 NULL,
					 &struct_iter))
    _g_dbus_oom ();
  
  if (!dbus_message_iter_append_basic (&struct_iter,
				       DBUS_TYPE_STRING,
				       &mount->dbus_id))
    _g_dbus_oom ();
  
  if (!dbus_message_iter_append_basic (&struct_iter,
				       DBUS_TYPE_OBJECT_PATH,
				       &mount->object_path))
    _g_dbus_oom ();
  
  if (!dbus_message_iter_append_basic (&struct_iter,
				       DBUS_TYPE_STRING,
				       &mount->display_name))
    _g_dbus_oom ();

  if (!dbus_message_iter_append_basic (&struct_iter,
				       DBUS_TYPE_STRING,
				       &mount->stable_name))
    _g_dbus_oom ();

  if (!dbus_message_iter_append_basic (&struct_iter,
				       DBUS_TYPE_STRING,
				       &mount->x_content_types))
    _g_dbus_oom ();
  
  if (!dbus_message_iter_append_basic (&struct_iter,
				       DBUS_TYPE_STRING,
				       &mount->icon))
    _g_dbus_oom ();
	      
  if (!dbus_message_iter_append_basic (&struct_iter,
				       DBUS_TYPE_STRING,
				       &mount->prefered_filename_encoding))
    _g_dbus_oom ();

  user_visible = mount->user_visible;
  if (!dbus_message_iter_append_basic (&struct_iter,
				       DBUS_TYPE_BOOLEAN,
				       &user_visible))
    _g_dbus_oom ();

  fuse_mountpoint = "";
  if (fuse_available && mount->fuse_mountpoint)
    fuse_mountpoint = mount->fuse_mountpoint;
  _g_dbus_message_iter_append_cstring (&struct_iter, fuse_mountpoint);

  g_mount_spec_to_dbus (&struct_iter, mount->mount_spec);

  _g_dbus_message_iter_append_cstring (&struct_iter, mount->default_location);

  if (!dbus_message_iter_close_container (iter, &struct_iter))
    _g_dbus_oom ();
}

static void
vfs_mountable_to_dbus (VfsMountable *mountable,
		       DBusMessageIter *iter)
{
  DBusMessageIter struct_iter;
  dbus_bool_t bool;
  guint32 int32;
  char *s;
  char **a;
  char *empty[] = {NULL};
  
  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_STRUCT,
					 NULL,
					 &struct_iter))
    _g_dbus_oom ();

  if (!dbus_message_iter_append_basic (&struct_iter,
				       DBUS_TYPE_STRING,
				       &mountable->type))
    _g_dbus_oom ();
  
  s = mountable->scheme;
  if (s == NULL)
    s = "";
  if (!dbus_message_iter_append_basic (&struct_iter,
				       DBUS_TYPE_STRING,
				       &s))
    _g_dbus_oom ();

  a = mountable->scheme_aliases;
  if (a == NULL)
    a = empty;
  _g_dbus_message_iter_append_args (&struct_iter,
				    DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &a, (int)g_strv_length (a),
				    0);
  
  int32 = mountable->default_port;
  if (!dbus_message_iter_append_basic (&struct_iter,
				       DBUS_TYPE_INT32,
				       &int32))
    _g_dbus_oom ();
  
  bool = mountable->hostname_is_inet;
  if (!dbus_message_iter_append_basic (&struct_iter,
				       DBUS_TYPE_BOOLEAN,
				       &bool))
    _g_dbus_oom ();

  if (!dbus_message_iter_close_container (iter, &struct_iter))
    _g_dbus_oom ();
}


/************************************************************************
 * Support for mounting a VfsMountable                                  *
 ************************************************************************/


typedef struct {
  VfsMountable *mountable;
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
					  G_VFS_DBUS_MOUNTABLE_OP_MOUNT);

  dbus_message_iter_init_append (message, &iter);
  g_mount_spec_to_dbus (&iter, data->mount_spec);

  _g_dbus_message_append_args (message,
			       DBUS_TYPE_BOOLEAN, &data->automount,
			       0);
  
  g_mount_source_to_dbus (data->source, message);
  
  if (!dbus_connection_send_with_reply (conn, message,
					&pending,
					G_VFS_DBUS_MOUNT_TIMEOUT_MSECS))
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
				   G_VFS_DBUS_OP_SPAWNED))
    {
      dbus_connection_unregister_object_path (connection, data->obj_path);

      if (!dbus_message_get_args (message, NULL,
				  DBUS_TYPE_BOOLEAN, &succeeded,
				  DBUS_TYPE_STRING, &error_message,
				  DBUS_TYPE_INVALID))
	{
	  g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
			       _("Invalid arguments from spawned child"));
	  mount_finish (data, error);
	  g_error_free (error);
	}
      else if (!succeeded)
	{
	  g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, error_message);
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
    NULL,
    spawn_mount_message_function
  };

  data->spawned = TRUE;
  
  error = NULL;
  if (data->mountable->exec == NULL)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
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

static void
mountable_mount (VfsMountable *mountable,
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

static void
read_mountable_config (void)
{
  GDir *dir;
  char *mount_dir, *path;
  const char *filename;
  GKeyFile *keyfile;
  char **types;
  VfsMountable *mountable;
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
			  mountable = g_new0 (VfsMountable, 1);
			  mountable->type = g_strdup (types[i]);
			  mountable->exec = g_key_file_get_string (keyfile, "Mount", "Exec", NULL);
			  mountable->dbus_name = g_key_file_get_string (keyfile, "Mount", "DBusName", NULL);
			  mountable->automount = g_key_file_get_boolean (keyfile, "Mount", "AutoMount", NULL);
			  mountable->scheme = g_key_file_get_string (keyfile, "Mount", "Scheme", NULL);
			  mountable->scheme_aliases =
			    g_key_file_get_string_list (keyfile, "Mount", "SchemeAliases", NULL, NULL);
			  mountable->default_port = g_key_file_get_integer (keyfile, "Mount", "DefaultPort", NULL);
			  mountable->hostname_is_inet = g_key_file_get_boolean (keyfile, "Mount", "HostnameIsInetAddress", NULL);

			  if (mountable->scheme == NULL)
			    mountable->scheme = g_strdup (mountable->type);
			  
			  mountables = g_list_prepend (mountables, mountable);
			}
		    }
		  g_strfreev (types);
		}
	    }
	  g_key_file_free (keyfile);
	  g_free (path);
	}
      g_dir_close (dir);
    }
}

static void
re_read_mountable_config (void)
{
  g_list_foreach (mountables, (GFunc)vfs_mountable_free, NULL);
  g_list_free (mountables);
  mountables = NULL;

  read_mountable_config ();
}

/************************************************************************
 * Support for keeping track of active mounts                           *
 ************************************************************************/

static void
signal_mounted_unmounted (VfsMount *mount,
			  gboolean mounted)
{
  DBusMessage *message;
  DBusMessageIter iter;
  DBusConnection *conn;

  message = dbus_message_new_signal (G_VFS_DBUS_MOUNTTRACKER_PATH,
				     G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
				     mounted ?
				     G_VFS_DBUS_MOUNTTRACKER_SIGNAL_MOUNTED :
				     G_VFS_DBUS_MOUNTTRACKER_SIGNAL_UNMOUNTED
				     );
  if (message == NULL)
    _g_dbus_oom ();

  dbus_message_iter_init_append (message, &iter);
  vfs_mount_to_dbus (mount, &iter);

  conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);
  dbus_connection_send (conn, message, NULL);
  dbus_connection_unref (conn);
  
  dbus_message_unref (message);
}

static void
dbus_client_disconnected (const char *dbus_id)
{
  GList *l, *next;

  next = NULL;
  for (l = mounts; l != NULL; l = next)
    {
      VfsMount *mount = l->data;
      next = l->next;

      if (strcmp (mount->dbus_id, dbus_id) == 0)
	{
	  signal_mounted_unmounted (mount, FALSE);
	  
	  vfs_mount_free (mount);
	  mounts = g_list_delete_link (mounts, l);
	}
    }
}

static void
register_mount (DBusConnection *connection,
		DBusMessage *message)
{
  VfsMount *mount;
  DBusMessage *reply;
  DBusError error;
  const char *display_name, *stable_name, *x_content_types, *icon, *obj_path;
  const char *id, *prefered_filename_encoding;
  char *default_location;
  dbus_bool_t user_visible;
  DBusMessageIter iter;
  GMountSpec *mount_spec;

  id = dbus_message_get_sender (message);

  dbus_message_iter_init (message, &iter);

  dbus_error_init (&error);
  if (_g_dbus_message_iter_get_args (&iter,
				     &error,
				     DBUS_TYPE_OBJECT_PATH, &obj_path,
				     DBUS_TYPE_STRING, &display_name,
                                     DBUS_TYPE_STRING, &stable_name,
                                     DBUS_TYPE_STRING, &x_content_types,
				     DBUS_TYPE_STRING, &icon,
				     DBUS_TYPE_STRING, &prefered_filename_encoding,
				     DBUS_TYPE_BOOLEAN, &user_visible,
				     0))
    {
      if (find_vfs_mount (id, obj_path) != NULL)
	reply = dbus_message_new_error (message,
					DBUS_ERROR_INVALID_ARGS,
					"Mountpoint Already registered");
      else if ((mount_spec = g_mount_spec_from_dbus (&iter)) == NULL)
	reply = dbus_message_new_error (message,
					DBUS_ERROR_INVALID_ARGS,
                                        "Error in mount spec");
      else if (match_vfs_mount (mount_spec) != NULL)
	reply = dbus_message_new_error (message,
					DBUS_ERROR_INVALID_ARGS,
					"Mountpoint Already registered");
      else
	{
	  mount = g_new0 (VfsMount, 1);
	  mount->display_name = g_strdup (display_name);
          mount->stable_name = g_strdup (stable_name);
          mount->x_content_types = g_strdup (x_content_types);
	  mount->icon = g_strdup (icon);
	  mount->prefered_filename_encoding = g_strdup (prefered_filename_encoding);
	  mount->user_visible = user_visible;
	  mount->dbus_id = g_strdup (id);
	  mount->object_path = g_strdup (obj_path);
	  mount->mount_spec = mount_spec;

	  if (_g_dbus_message_iter_get_args (&iter, NULL,
					     G_DBUS_TYPE_CSTRING, &default_location,
					     0))
	    mount->default_location = default_location;
	  else
	    mount->default_location = g_strdup ("");

	  if (user_visible)
	    {
	      char *fs_name;
	      
	      /* Keep in sync with fuse daemon */
	      fs_name = g_uri_escape_string (mount->stable_name, "+@#$., ", TRUE);
	      
	      mount->fuse_mountpoint = g_build_filename (g_get_home_dir(), ".gvfs", fs_name, NULL);
	    }
	  
	  mounts = g_list_prepend (mounts, mount);

	  signal_mounted_unmounted (mount, TRUE);

	  reply = dbus_message_new_method_return (message);
	}
    }
  else
    {
      reply = dbus_message_new_error (message,
				      error.name, error.message);
      dbus_error_free (&error);
    }
  
  if (reply == NULL)
    _g_dbus_oom ();
  
  dbus_connection_send (connection, reply, NULL);
}

typedef struct {
  DBusMessage *message;
  DBusConnection *connection;
} AutoMountData;

static void
automount_done (VfsMountable *mountable,
		GError *error,
		gpointer _data)
{
  DBusMessage *reply;
  AutoMountData *data = _data;
  
  if (error)
    {
      reply = _dbus_message_new_gerror (data->message,
					G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED,
					_("Automount failed: %s"), error->message);
      dbus_connection_send (data->connection, reply, NULL);
    }
  else
    lookup_mount (data->connection,
		  data->message,
		  FALSE);

  dbus_connection_unref (data->connection);
  dbus_message_unref (data->message);
  g_free (data);
}

static DBusMessage *
maybe_automount (GMountSpec *spec,
		 DBusMessage *message,
		 DBusConnection *connection,
		 gboolean do_automount)
{
  VfsMountable *mountable;
  DBusMessage *reply;

  mountable = lookup_mountable (spec);

  reply = NULL;
  if (mountable != NULL && do_automount && mountable->automount)
    {
      AutoMountData *data;
      GMountSource *mount_source;

      g_debug ("automounting...\n");

      mount_source = g_mount_source_new_dummy ();

      data = g_new0 (AutoMountData, 1);
      data->message = dbus_message_ref (message);
      data->connection = dbus_connection_ref (connection);
      
      mountable_mount (mountable, spec, mount_source, TRUE, automount_done, data);
      g_object_unref (mount_source);
    }
  else if (mountable != NULL)
    reply = _dbus_message_new_gerror (message,
				      G_IO_ERROR,
				      G_IO_ERROR_NOT_MOUNTED,
				      _("The specified location is not mounted"));
  else
    reply = _dbus_message_new_gerror (message,
				      G_IO_ERROR,
				      G_IO_ERROR_NOT_SUPPORTED,
				      _("The specified location is not supported"));
  
  return reply;
}

static void
lookup_mount (DBusConnection *connection,
	      DBusMessage *message,
	      gboolean do_automount)
{
  VfsMount *mount;
  DBusMessage *reply;
  DBusMessageIter iter;
  GMountSpec *spec;

  dbus_message_iter_init (message, &iter);
  spec = g_mount_spec_from_dbus (&iter);

  reply = NULL;
  if (spec != NULL)
    {
      mount = match_vfs_mount (spec);

      if (mount == NULL)
	reply = maybe_automount (spec, message, connection, do_automount);
      else
	{
	  reply = dbus_message_new_method_return (message);

	  if (reply)
	    {
	      dbus_message_iter_init_append (reply, &iter);

	      vfs_mount_to_dbus (mount, &iter);
	    }
	}
    }
  else
    reply = dbus_message_new_error (message,
				    DBUS_ERROR_INVALID_ARGS,
				    "Invalid arguments");
  
  g_mount_spec_unref (spec);
  if (reply != NULL)
    dbus_connection_send (connection, reply, NULL);
}

static void
lookup_mount_by_fuse_path (DBusConnection *connection,
			   DBusMessage *message)
{
  VfsMount *mount;
  DBusMessage *reply;
  DBusMessageIter iter;
  char *fuse_path;

  dbus_message_iter_init (message, &iter);

  reply = NULL;
  if (_g_dbus_message_iter_get_args (&iter, NULL, 
				     G_DBUS_TYPE_CSTRING, &fuse_path,
				     0))
    {
      mount = find_vfs_mount_by_fuse_path (fuse_path);

      if (mount == NULL)
	reply = _dbus_message_new_gerror (message,
					  G_IO_ERROR,
					  G_IO_ERROR_NOT_MOUNTED,
					  _("The specified location is not mounted"));
      else
	{
	  reply = dbus_message_new_method_return (message);
	  if (reply)
	    {
	      dbus_message_iter_init_append (reply, &iter);
	      vfs_mount_to_dbus (mount, &iter);
	    }
	}
    }
  else
    reply = dbus_message_new_error (message,
				    DBUS_ERROR_INVALID_ARGS,
				    "Invalid arguments");
  
  if (reply != NULL)
    dbus_connection_send (connection, reply, NULL);
}

static void
list_mounts (DBusConnection *connection,
	     DBusMessage *message)
{
  VfsMount *mount;
  DBusMessage *reply;
  DBusMessageIter iter, array_iter;
  GList *l;

  reply = dbus_message_new_method_return (message);
  if (reply == NULL)
    _g_dbus_oom ();

  dbus_message_iter_init_append (reply, &iter);


  if (!dbus_message_iter_open_container (&iter,
					 DBUS_TYPE_ARRAY,
					 DBUS_STRUCT_BEGIN_CHAR_AS_STRING
					   DBUS_TYPE_STRING_AS_STRING
					   DBUS_TYPE_OBJECT_PATH_AS_STRING
					   DBUS_TYPE_STRING_AS_STRING
					   DBUS_TYPE_STRING_AS_STRING
					   DBUS_TYPE_STRING_AS_STRING
					   DBUS_TYPE_STRING_AS_STRING
					   DBUS_TYPE_STRING_AS_STRING
					   DBUS_TYPE_BOOLEAN_AS_STRING
					   DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_BYTE_AS_STRING
 					   G_MOUNT_SPEC_TYPE_AS_STRING
					   DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_BYTE_AS_STRING
					 DBUS_STRUCT_END_CHAR_AS_STRING,
					 &array_iter))
    _g_dbus_oom ();

  for (l = mounts; l != NULL; l = l->next)
    {
      mount = l->data;

      vfs_mount_to_dbus (mount, &array_iter);
    }

  if (!dbus_message_iter_close_container (&iter, &array_iter))
    _g_dbus_oom ();
  
  dbus_connection_send (connection, reply, NULL);
}

static void
mount_location_done  (VfsMountable *mountable,
		      GError *error,
		      gpointer user_data)
{
  DBusMessage *message, *reply;
  DBusConnection *conn;

  message = user_data;
  
  if (error)
    reply = _dbus_message_new_from_gerror (message, error);
  else
    reply = dbus_message_new_method_return (message);

  dbus_message_unref (message);

  if (reply == NULL)
    _g_dbus_oom ();
    
  conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);
  if (conn)
    {
      dbus_connection_send (conn, reply, NULL);
      dbus_connection_unref (conn);
    }
  
  dbus_message_unref (reply);
}

static void
mount_location (DBusConnection *connection,
		DBusMessage *message)
{
  DBusMessageIter iter;
  DBusMessage *reply;
  DBusError derror;
  GMountSpec *spec;
  const char *obj_path, *dbus_id;
  VfsMountable *mountable;
  
  dbus_message_iter_init (message, &iter);

  mountable = NULL;
  spec = NULL;
  reply = NULL;

  spec = g_mount_spec_from_dbus (&iter);
  if (spec == NULL)
    reply = dbus_message_new_error (message, DBUS_ERROR_INVALID_ARGS,
				    "Invalid arguments");
  else
    {
      dbus_error_init (&derror);
      if (!_g_dbus_message_iter_get_args (&iter,
					  &derror,
					  DBUS_TYPE_STRING, &dbus_id,
					  DBUS_TYPE_OBJECT_PATH, &obj_path,
					  0))
	{
	  reply = dbus_message_new_error (message, derror.name, derror.message);
	  dbus_error_free (&derror);
	}
      else
	{
	  VfsMount *mount;
	  mount = match_vfs_mount (spec);
	  
	  if (mount != NULL)
	    reply = _dbus_message_new_gerror (message,
					      G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED,
					      _("Location is already mounted"));
	  else
	    {
	      mountable = lookup_mountable (spec);
	      
	      if (mountable == NULL)
		reply = _dbus_message_new_gerror (message,
						  G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED,
						  _("Location is not mountable"));
	    }
	}
    }
  
  if (reply)
    dbus_connection_send (connection, reply, NULL);
  else
    {
      GMountSource *source;

      source = g_mount_source_new (dbus_id, obj_path);
      mountable_mount (mountable,
		       spec,
		       source,
		       FALSE,
		       mount_location_done, dbus_message_ref (message));
      g_object_unref (source);
    }

  if (spec)
    g_mount_spec_unref (spec);
  
}

static void
list_mount_types (DBusConnection *connection,
		  DBusMessage *message)
{
  VfsMountable *mountable;
  DBusMessage *reply;
  DBusMessageIter iter, array_iter;
  GList *l;

  reply = dbus_message_new_method_return (message);
  if (reply == NULL)
    _g_dbus_oom ();

  dbus_message_iter_init_append (reply, &iter);

  
  if (!dbus_message_iter_open_container (&iter,
					 DBUS_TYPE_ARRAY,
					 DBUS_TYPE_STRING_AS_STRING,
					 &array_iter))
    _g_dbus_oom ();

  for (l = mountables; l != NULL; l = l->next)
    {
      mountable = l->data;
      if (!dbus_message_iter_append_basic (&array_iter,
            DBUS_TYPE_STRING,
	    &mountable->type))
        _g_dbus_oom ();
    }

  if (!dbus_message_iter_close_container (&iter, &array_iter))
    _g_dbus_oom ();
  
  dbus_connection_send (connection, reply, NULL);
}

static void
list_mountable_info (DBusConnection *connection,
		     DBusMessage *message)
{
  VfsMountable *mountable;
  DBusMessage *reply;
  DBusMessageIter iter, array_iter;
  GList *l;

  reply = dbus_message_new_method_return (message);
  if (reply == NULL)
    _g_dbus_oom ();

  dbus_message_iter_init_append (reply, &iter);

  
  if (!dbus_message_iter_open_container (&iter,
					 DBUS_TYPE_ARRAY,
					 DBUS_STRUCT_BEGIN_CHAR_AS_STRING
					   DBUS_TYPE_STRING_AS_STRING /* type */
					   DBUS_TYPE_STRING_AS_STRING /* scheme */
					   DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_STRING_AS_STRING /* scheme aliases */
					   DBUS_TYPE_INT32_AS_STRING /* default port */
					   DBUS_TYPE_BOOLEAN_AS_STRING /* host is inet */
					 DBUS_STRUCT_END_CHAR_AS_STRING,
					 &array_iter))
    _g_dbus_oom ();

  for (l = mountables; l != NULL; l = l->next)
    {
      mountable = l->data;
      
      vfs_mountable_to_dbus (mountable, &array_iter);
    }

  if (!dbus_message_iter_close_container (&iter, &array_iter))
    _g_dbus_oom ();
  
  dbus_connection_send (connection, reply, NULL);
}

static DBusHandlerResult
dbus_message_function (DBusConnection  *connection,
		       DBusMessage     *message,
		       void            *user_data)
{
  DBusHandlerResult res;
  
  res = DBUS_HANDLER_RESULT_HANDLED;
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
				   G_VFS_DBUS_MOUNTTRACKER_OP_REGISTER_FUSE))
    fuse_available = TRUE;
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					G_VFS_DBUS_MOUNTTRACKER_OP_REGISTER_MOUNT))
    register_mount (connection, message);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					G_VFS_DBUS_MOUNTTRACKER_OP_LOOKUP_MOUNT))
    lookup_mount (connection, message, TRUE);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					G_VFS_DBUS_MOUNTTRACKER_OP_LOOKUP_MOUNT_BY_FUSE_PATH))
    lookup_mount_by_fuse_path (connection, message);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					G_VFS_DBUS_MOUNTTRACKER_OP_LIST_MOUNTS))
    list_mounts (connection, message);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					G_VFS_DBUS_MOUNTTRACKER_OP_MOUNT_LOCATION))
    mount_location (connection, message);
  else if (dbus_message_is_method_call (message,
  					G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					G_VFS_DBUS_MOUNTTRACKER_OP_LIST_MOUNT_TYPES))
    list_mount_types (connection, message);
  else if (dbus_message_is_method_call (message,
  					G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					G_VFS_DBUS_MOUNTTRACKER_OP_LIST_MOUNTABLE_INFO))
    list_mountable_info (connection, message);
  else
    res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  
  return res;
}

struct DBusObjectPathVTable tracker_dbus_vtable = {
  NULL,
  dbus_message_function
};


static DBusHandlerResult
mount_tracker_filter_func (DBusConnection *conn,
			   DBusMessage    *message,
			   gpointer        data)
{
  const char *name, *from, *to;

  if (dbus_message_is_signal (message,
			      DBUS_INTERFACE_DBUS,
			      "NameOwnerChanged"))
    {
      if (dbus_message_get_args (message, NULL,
				 DBUS_TYPE_STRING, &name,
				 DBUS_TYPE_STRING, &from,
				 DBUS_TYPE_STRING, &to,
				 DBUS_TYPE_INVALID))
	{
	  if (*name == ':' &&  *to == 0)
	    dbus_client_disconnected (name);
	}
      
    }
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


static int reload_pipes[2];

static void
sigusr1_handler (int sig)
{
  while (write (reload_pipes[1], "a", 1) != 1)
    ;
}

static gboolean
reload_pipes_cb (GIOChannel *io,
		 GIOCondition condition,
		 gpointer data)
{
  char a;
  
  while (read (reload_pipes[0], &a, 1) != 1)
    ;

  re_read_mountable_config ();
  
  return TRUE;
}

void
mount_init (void)
{
  DBusConnection *conn;
  DBusError error;
  struct sigaction sa;
  GIOChannel *io;
  
  read_mountable_config ();

  if (pipe (reload_pipes) != -1)
    {
      io = g_io_channel_unix_new (reload_pipes[0]);
      g_io_add_watch (io, G_IO_IN, reload_pipes_cb, NULL);
      
      sa.sa_handler = sigusr1_handler;
      sigemptyset (&sa.sa_mask);
      sa.sa_flags = 0;
      sigaction (SIGUSR1, &sa, NULL);
    }
  
  conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);

  if (!dbus_connection_register_object_path (conn, G_VFS_DBUS_MOUNTTRACKER_PATH,
					     &tracker_dbus_vtable, NULL))
    _g_dbus_oom ();

  if (!dbus_connection_add_filter (conn,
				   mount_tracker_filter_func, NULL, NULL))
    _g_dbus_oom ();
  
  
  dbus_error_init (&error);
  dbus_bus_add_match (conn,
		      "sender='org.freedesktop.DBus',"
		      "interface='org.freedesktop.DBus',"
		      "member='NameOwnerChanged'",
		      &error);
  if (dbus_error_is_set (&error))
    {
      g_warning ("Failed to add dbus match: %s\n", error.message);
      dbus_error_free (&error);
    }
}

