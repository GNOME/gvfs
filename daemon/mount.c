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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include "mount.h"
#include "gmountoperationdbus.h"
#include "gvfsdaemonprotocol.h"
#include <gvfsdbus.h>
#include <gvfsutils.h>

typedef struct {
  char *display_name;
  char *stable_name;
  char *x_content_types;
  char *icon;
  char *symbolic_icon;
  char *prefered_filename_encoding;
  gboolean user_visible;
  char *default_location;
  char *fuse_mountpoint; /* Always set, even if fuse not available */
  
  /* Daemon object ref */
  char *dbus_id;
  char *object_path;
  guint name_watcher_id;

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
  gboolean mount_per_client;
} VfsMountable; 

typedef void (*MountCallback) (VfsMountable *mountable,
			       GError *error,
			       gpointer user_data);

static GList *mountables = NULL;
static GList *mounts = NULL;
static GList *ongoing = NULL;

static gboolean fuse_available;

static GVfsDBusMountTracker *mount_tracker = NULL;


static void lookup_mount (GVfsDBusMountTracker *object,
                          GDBusMethodInvocation *invocation,
                          GMountSpec *spec,
                          gboolean do_automount);

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
  if (mount->name_watcher_id != 0)
    g_bus_unwatch_name (mount->name_watcher_id);

  g_free (mount->display_name);
  g_free (mount->stable_name);
  g_free (mount->x_content_types);
  g_free (mount->icon);
  g_free (mount->symbolic_icon);
  g_free (mount->fuse_mountpoint);
  g_free (mount->prefered_filename_encoding);
  g_free (mount->default_location);
  g_free (mount->dbus_id);
  g_free (mount->object_path);
  g_mount_spec_unref (mount->mount_spec);

  g_free (mount);
}


/* Keep in sync with dbus-interfaces.xml */
#define VFS_MOUNT_ARRAY_DBUS_STRUCT_TYPE "a(sossssssbay(aya{sv})ay)"
#define VFS_MOUNTABLE_ARRAY_DBUS_STRUCT_TYPE "a(ssasib)"

static GVariant *
vfs_mount_to_dbus (VfsMount *mount)
{
  return g_variant_new ("(sossssssb^ay@(aya{sv})^ay)",
                        mount->dbus_id,
                        mount->object_path,
                        mount->display_name,
                        mount->stable_name,
                        mount->x_content_types,
                        mount->icon,
                        mount->symbolic_icon,
                        mount->prefered_filename_encoding,
                        mount->user_visible,
                        (fuse_available && mount->fuse_mountpoint) ? mount->fuse_mountpoint : "",
                        g_mount_spec_to_dbus (mount->mount_spec),
                        mount->default_location ? mount->default_location : "");
}

static GVariant *
vfs_mountable_to_dbus (VfsMountable *mountable)
{
  char *empty[] = {NULL};
  
  return g_variant_new ("(ss^asib)",
                        mountable->type,
                        mountable->scheme ? mountable->scheme : "",
                        mountable->scheme_aliases ? mountable->scheme_aliases : empty,
                        mountable->default_port,
                        mountable->hostname_is_inet);
}


/************************************************************************
 * Support for mounting a VfsMountable                                  *
 ************************************************************************/


typedef struct {
  VfsMountable *mountable;
  gboolean automount;
  GMountSource *source;
  GMountSpec *mount_spec;
  MountCallback callback;
  gpointer user_data;
  char *obj_path;
  gboolean spawned;
  GVfsDBusSpawner *spawner;
  GList *pending; /* MountData */
} MountData;

static void spawn_mount (MountData *data);

static void
mount_data_free (MountData *data)
{
  g_object_unref (data->source);
  g_mount_spec_unref (data->mount_spec);
  g_free (data->obj_path);
  g_clear_object (&data->spawner);
  g_list_free_full (data->pending, (GDestroyNotify) mount_data_free);

  g_free (data);
}

static void
mount_finish (MountData *data, GError *error)
{
  GList *l;

  ongoing = g_list_remove (ongoing, data);

  data->callback (data->mountable, error, data->user_data);
  for (l = data->pending; l != NULL; l = l->next)
    {
      MountData *pending_data = l->data;
      pending_data->callback (pending_data->mountable, error, pending_data->user_data);
    }

  mount_data_free (data);
}

static void
dbus_mount_reply (GVfsDBusMountable *proxy,
                  GAsyncResult  *res,
                  gpointer user_data)
{
  GError *error = NULL;
  MountData *data = user_data;

  if (!gvfs_dbus_mountable_call_mount_finish (proxy,
                                              res,
                                              &error))
    {
      if ((g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
           g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)) &&
           !data->spawned)
        spawn_mount (data);
      else if (g_error_matches (error, G_IO_ERROR,
                                G_IO_ERROR_ALREADY_MOUNTED))
        mount_finish (data, NULL);
      else
        {
          g_dbus_error_strip_remote_error (error);
          g_debug ("dbus_mount_reply: Error from org.gtk.vfs.Mountable.mount(): %s\n", error->message);
          mount_finish (data, error);
          g_error_free (error);
        }
    }
  else
    {
      mount_finish (data, NULL);
    }
}

static void
mountable_mount_proxy_cb (GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
  GVfsDBusMountable *proxy;
  GError *error = NULL;
  MountData *data = user_data;
  
  proxy = gvfs_dbus_mountable_proxy_new_for_bus_finish (res, &error);

  if (proxy == NULL)
    {
      g_printerr ("mountable_mount_proxy_cb: Error creating proxy: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      mount_finish (data, error);
      g_error_free (error);
      return;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), 
                                    G_VFS_DBUS_MOUNT_TIMEOUT_MSECS);
  
  gvfs_dbus_mountable_call_mount (proxy,
                                  g_mount_spec_to_dbus (data->mount_spec),
                                  data->automount,
                                  g_mount_source_to_dbus (data->source),
                                  NULL,
                                  (GAsyncReadyCallback) dbus_mount_reply, data);

  g_object_unref (proxy);
}

static void
mountable_mount_with_name (MountData *data,
			   const char *dbus_name)
{
  gvfs_dbus_mountable_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                         G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS | G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                         dbus_name,
                                         G_VFS_DBUS_MOUNTABLE_PATH,
                                         NULL,
                                         mountable_mount_proxy_cb,
                                         data);
}

static gboolean
spawn_mount_handle_spawned (GVfsDBusSpawner *object,
                            GDBusMethodInvocation *invocation,
                            gboolean arg_succeeded,
                            const gchar *arg_error_message,
                            guint arg_error_code,
                            gpointer user_data)
{
  MountData *data = user_data;
  GError *error = NULL;

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (data->spawner));

  if (!arg_succeeded)
    {
      if (arg_error_code == G_IO_ERROR_ALREADY_MOUNTED &&
          data->mountable->dbus_name != NULL)
        {
          /* This means the spawn failed since someone already owned the name.
             It might not strictly be mounted yet, as the mount might not
             be registred yet. So, to avoid races we ask the new owner of
             the name to mount. It'll typically return an ALREADY_MOUNTED
             error which we treat as success.
          */
          mountable_mount_with_name (data, data->mountable->dbus_name);
        }
      else
        {
          g_set_error_literal (&error, G_IO_ERROR, arg_error_code, arg_error_message);
          mount_finish (data, error);
          g_error_free (error);
        }
    }
  else
    {
      mountable_mount_with_name (data, g_dbus_method_invocation_get_sender (invocation));
    }
  
  gvfs_dbus_spawner_complete_spawned (object, invocation);
  
  return TRUE;
}

static void
child_watch_cb (GPid pid,
                gint status,
                gpointer user_data)
{
  MountData *data = user_data;
  GError *error = NULL;
  gint code = 0;

  if (!g_spawn_check_wait_status (status, &error))
    {
      if (error->domain == G_SPAWN_EXIT_ERROR)
        code = error->code;

      g_clear_error (&error);
    }

  /* GVfs daemons always exit with 0, but gvfsd-admin is spawned over pkexec,
   * which can fail when the authentication dialog is dismissed for example.
   */
  if (code == 126 || code == 127)
    {
      error = g_error_new_literal (G_IO_ERROR,
                                   G_IO_ERROR_PERMISSION_DENIED,
                                   _("Permission denied"));
      mount_finish (data, error);
      g_error_free (error);
    }

  g_spawn_close_pid (pid);
}

static void
spawn_mount (MountData *data)
{
  char *exec;
  GError *error;
  GDBusConnection *connection;
  static int mount_id = 0;
  gchar **argv = NULL;
  GPid pid;

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

      connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
      if (! connection)
        {
          mount_finish (data, error);
          g_error_free (error);
          return;
        }
      
      data->spawner = gvfs_dbus_spawner_skeleton_new ();
      g_signal_connect (data->spawner, "handle-spawned", G_CALLBACK (spawn_mount_handle_spawned), data);
      
      if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (data->spawner),
                                             connection,
                                             data->obj_path, 
                                             &error))
        {
          mount_finish (data, error);
          g_error_free (error);
          g_object_unref (connection);
          return;
        }

      exec = g_strconcat (data->mountable->exec,
                          gvfs_get_debug () ? " --debug" : "",
                          " --spawner ",
                          g_dbus_connection_get_unique_name (connection),
                          " ",
                          data->obj_path,
                          NULL);

      /* G_SPAWN_DO_NOT_REAP_CHILD is necessary for admin backend to prevent
       * double forking causing pkexec failures, see:
       * https://bugzilla.gnome.org/show_bug.cgi?id=793445
       */
      if (!g_shell_parse_argv (exec, NULL, &argv, &error) ||
          !g_spawn_async (NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, &error))
        {
          g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (data->spawner));
          mount_finish (data, error);
          g_error_free (error);
        }
      else
        {
          g_child_watch_add (pid, child_watch_cb, data);
        }

      g_strfreev (argv);

      /* TODO: Add a timeout here to detect spawned app crashing */
      
      g_object_unref (connection);
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
  GList *l;

  data = g_new0 (MountData, 1);
  data->automount = automount;
  data->mountable = mountable;
  data->source = g_object_ref (source);
  data->mount_spec = g_mount_spec_ref (mount_spec);
  data->callback = callback;
  data->user_data = user_data;

  for (l = ongoing; l != NULL; l = l->next)
    {
      MountData *ongoing_data = l->data;
      if (g_mount_spec_equal (ongoing_data->mount_spec, mount_spec))
        {
          ongoing_data->pending = g_list_append (ongoing_data->pending, data);
          return;
        }
    }

  ongoing = g_list_append (ongoing, data);

  if (mountable->dbus_name == NULL)
    spawn_mount (data);
  else
    mountable_mount_with_name (data, mountable->dbus_name);
}

static void
read_mountable_config (void)
{
  GDir *dir;
  char *path;
  const gchar *mount_extension, *mount_dir;
  const char *filename;
  GKeyFile *keyfile;
  char **types;
  VfsMountable *mountable;
  int i;

  mount_extension = g_getenv ("GVFS_MOUNTABLE_EXTENSION");
  if (mount_extension == NULL || *mount_extension == 0)
    mount_extension = ".mount";
  
  mount_dir = g_getenv ("GVFS_MOUNTABLE_DIR");
  if (mount_dir == NULL || *mount_dir == 0)
    mount_dir = MOUNTABLE_DIR;
  dir = g_dir_open (mount_dir, 0, NULL);

  if (dir)
    {
      while ((filename = g_dir_read_name (dir)) != NULL)
	{
	  if (!g_str_has_suffix (filename, mount_extension))
	    continue;

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
			  mountable->mount_per_client = g_key_file_get_boolean (keyfile, "Mount", "MountPerClient", NULL);

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
  g_list_free_full (mountables, (GDestroyNotify)vfs_mountable_free);
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
  if (mounted)
    gvfs_dbus_mount_tracker_emit_mounted (mount_tracker, vfs_mount_to_dbus (mount));
  else
    gvfs_dbus_mount_tracker_emit_unmounted (mount_tracker, vfs_mount_to_dbus (mount));
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
name_vanished_cb (GDBusConnection *connection,
                  const gchar *name,
                  gpointer user_data)
{
  dbus_client_disconnected (name);
}

static gboolean
handle_register_mount (GVfsDBusMountTracker *object,
                       GDBusMethodInvocation *invocation,
                       const gchar *arg_obj_path,
                       const gchar *arg_display_name,
                       const gchar *arg_stable_name,
                       const gchar *arg_x_content_types,
                       const gchar *arg_icon,
                       const gchar *arg_symbolic_icon,
                       const gchar *arg_prefered_filename_encoding,
                       gboolean arg_user_visible,
                       GVariant *arg_mount_spec,
                       const gchar *arg_default_location,
                       gpointer user_data)
{
  VfsMount *mount;
  const char *id;
  GMountSpec *mount_spec;

  id = g_dbus_method_invocation_get_sender (invocation);

  if (find_vfs_mount (id, arg_obj_path) != NULL) {
    g_dbus_method_invocation_return_error_literal (invocation,
                                                   G_IO_ERROR,
                                                   G_IO_ERROR_ALREADY_MOUNTED,
                                                   "Mountpoint Already registered");
  }
  else if ((mount_spec = g_mount_spec_from_dbus (arg_mount_spec)) == NULL) {
    g_dbus_method_invocation_return_error_literal (invocation, 
                                                   G_IO_ERROR,
                                                   G_IO_ERROR_INVALID_ARGUMENT,
                                                   "Error in mount spec");
  }
  else if (match_vfs_mount (mount_spec) != NULL) {
    g_dbus_method_invocation_return_error_literal (invocation,
                                                   G_IO_ERROR,
                                                   G_IO_ERROR_ALREADY_MOUNTED,
                                                   "Mountpoint Already registered");
  }
  else
    {
      mount = g_new0 (VfsMount, 1);
      mount->display_name = g_strdup (arg_display_name);
      mount->stable_name = g_strdup (arg_stable_name);
      mount->x_content_types = g_strdup (arg_x_content_types);
      mount->icon = g_strdup (arg_icon);
      mount->symbolic_icon = g_strdup (arg_symbolic_icon);
      mount->prefered_filename_encoding = g_strdup (arg_prefered_filename_encoding);
      mount->user_visible = arg_user_visible;
      mount->dbus_id = g_strdup (id);
      mount->object_path = g_strdup (arg_obj_path);
      mount->mount_spec = mount_spec;

      if (arg_default_location)  
        mount->default_location = g_strdup (arg_default_location);
      else
        mount->default_location = g_strdup ("");

      if (arg_user_visible)
        {
          /* Use the old .gvfs location as fallback, not .cache/gvfs */
          if (g_strcmp0 (g_get_user_runtime_dir(), g_get_user_cache_dir ()) == 0)
            mount->fuse_mountpoint = g_build_filename (g_get_home_dir(), ".gvfs", mount->stable_name, NULL);
          else
            mount->fuse_mountpoint = g_build_filename (g_get_user_runtime_dir(), "gvfs", mount->stable_name, NULL);
        }
      
      mounts = g_list_prepend (mounts, mount);

      /* watch the mount for being disconnected */
      mount->name_watcher_id = g_bus_watch_name (G_BUS_TYPE_SESSION,
                                                 id,
                                                 G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                 NULL,
                                                 name_vanished_cb,
                                                 NULL,
                                                 NULL); 

      signal_mounted_unmounted (mount, TRUE);

      gvfs_dbus_mount_tracker_complete_register_mount (object, invocation);
    }
  
  return TRUE;
}

typedef struct {
  GVfsDBusMountTracker *object;
  GDBusMethodInvocation *invocation;
  GMountSpec *spec;
} AutoMountData;

static void
automount_done (VfsMountable *mountable,
		GError *error,
		gpointer _data)
{
  AutoMountData *data = _data;
  
  if (error)
    g_dbus_method_invocation_return_error (data->invocation,
                                           G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED,
                                           _("Automount failed: %s"), error->message);
  else
    lookup_mount (data->object,
                  data->invocation,
                  data->spec,
                  FALSE);

  g_object_unref (data->object);
  g_object_unref (data->invocation);
  g_mount_spec_unref (data->spec);
  g_free (data);
}

static void
maybe_automount (GMountSpec *spec,
                 GVfsDBusMountTracker *object,
                 GDBusMethodInvocation *invocation,
                 gboolean do_automount)
{
  VfsMountable *mountable;

  mountable = lookup_mountable (spec);

  if (mountable != NULL && do_automount && mountable->automount)
    {
      AutoMountData *data;
      GMountSource *mount_source;

      g_debug ("automounting...\n");

      mount_source = g_mount_source_new_dummy ();

      data = g_new0 (AutoMountData, 1);
      data->object = g_object_ref (object);
      data->invocation = g_object_ref (invocation);
      data->spec = g_mount_spec_ref (spec);
      
      mountable_mount (mountable, spec, mount_source, TRUE, automount_done, data);
      g_object_unref (mount_source);
    }
  else if (mountable != NULL)
    g_dbus_method_invocation_return_error_literal (invocation,
                                                   G_IO_ERROR,
                                                   G_IO_ERROR_NOT_MOUNTED,
                                                   _("The specified location is not mounted"));
  else
    g_dbus_method_invocation_return_error_literal (invocation,
                                                   G_IO_ERROR,
                                                   G_IO_ERROR_NOT_SUPPORTED,
                                                   _("The specified location is not supported"));
}

static void
lookup_mount (GVfsDBusMountTracker *object,
              GDBusMethodInvocation *invocation,
              GMountSpec *spec,
              gboolean do_automount)
{
  VfsMount *mount;

  mount = match_vfs_mount (spec);
  if (mount == NULL)
    maybe_automount (spec, object, invocation, do_automount);
  else
    gvfs_dbus_mount_tracker_complete_lookup_mount (object, invocation,
                                                   vfs_mount_to_dbus (mount)); 
}

static void
sanitize_spec (GMountSpec *spec, GDBusMethodInvocation *invocation)
{
  const gchar *client;
  VfsMountable *mountable;

  mountable = lookup_mountable (spec);
  if (mountable && mountable->mount_per_client)
    {
      client = g_dbus_method_invocation_get_sender (invocation);
      g_mount_spec_set (spec, "client", client);
    }
}

static gboolean 
handle_lookup_mount (GVfsDBusMountTracker *object,
                     GDBusMethodInvocation *invocation,
                     GVariant *arg_mount_spec,
                     gpointer user_data)
{
  GMountSpec *spec;

  spec = g_mount_spec_from_dbus (arg_mount_spec);

  if (spec != NULL)
    {
      sanitize_spec (spec, invocation);
      lookup_mount (object, invocation, spec, TRUE);
      g_mount_spec_unref (spec);
    }
  else
    g_dbus_method_invocation_return_error_literal (invocation,
                                                   G_IO_ERROR,
                                                   G_IO_ERROR_INVALID_ARGUMENT,
                                                   "Invalid arguments");
  return TRUE;
}

static gboolean
handle_lookup_mount_by_fuse_path (GVfsDBusMountTracker *object,
                                  GDBusMethodInvocation *invocation,
                                  const gchar *arg_fuse_path,
                                  gpointer user_data)
{
  VfsMount *mount;

  mount = find_vfs_mount_by_fuse_path (arg_fuse_path);

  if (mount == NULL)
    g_dbus_method_invocation_return_error_literal (invocation,
                                                   G_IO_ERROR,
                                                   G_IO_ERROR_NOT_MOUNTED,
                                                   _("The specified location is not mounted"));
  else
    gvfs_dbus_mount_tracker_complete_lookup_mount_by_fuse_path (object,
                                                                invocation,
                                                                vfs_mount_to_dbus (mount));
  
  return TRUE;
}

static void
build_mounts_array (GVariantBuilder *mounts_array,
                    gboolean user_visible_only,
                    GDBusMethodInvocation *invocation)
{
  GList *l;
  VfsMount *mount;
  VfsMountable *mountable;

  g_variant_builder_init (mounts_array, G_VARIANT_TYPE (VFS_MOUNT_ARRAY_DBUS_STRUCT_TYPE));
  for (l = mounts; l != NULL; l = l->next)
    {
      mount = l->data;

      mountable = lookup_mountable (mount->mount_spec);
      if (mountable && mountable->mount_per_client)
        {
          const gchar *client;

          client = g_dbus_method_invocation_get_sender (invocation);
          if (g_strcmp0 (g_mount_spec_get (mount->mount_spec, "client"), client) != 0)
            continue;
        }

      if (!user_visible_only || mount->user_visible)
        g_variant_builder_add_value (mounts_array, vfs_mount_to_dbus (mount));
    }
}

static gboolean
handle_list_mounts (GVfsDBusMountTracker *object,
                    GDBusMethodInvocation *invocation,
                    gpointer user_data)
{
  GVariantBuilder mounts_array;

  build_mounts_array (&mounts_array, FALSE, invocation);

  gvfs_dbus_mount_tracker_complete_list_mounts (object, invocation,
                                                g_variant_builder_end (&mounts_array));

  return TRUE;
}

static gboolean
handle_list_mounts2 (GVfsDBusMountTracker *object,
                     GDBusMethodInvocation *invocation,
                     gboolean arg_user_visible_only,
                     gpointer user_data)
{
  GVariantBuilder mounts_array;

  build_mounts_array (&mounts_array, arg_user_visible_only, invocation);

  gvfs_dbus_mount_tracker_complete_list_mounts2 (object, invocation,
                                                 g_variant_builder_end (&mounts_array));

  return TRUE;
}

static void
mount_location_done  (VfsMountable *mountable,
		      GError *error,
		      gpointer user_data)
{
  GDBusMethodInvocation *invocation = G_DBUS_METHOD_INVOCATION (user_data);
  
  if (error)
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    /* alas gvfs_dbus_mount_tracker_complete_mount_location() */
    g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));

  g_object_unref (invocation);
}

static gboolean 
handle_mount_location (GVfsDBusMountTracker *object,
                       GDBusMethodInvocation *invocation,
                       GVariant *arg_mount_spec,
                       GVariant *arg_mount_source,
                       gpointer user_data)
{
  GMountSpec *spec;
  VfsMountable *mountable;

  spec = g_mount_spec_from_dbus (arg_mount_spec);
  if (spec == NULL)
    g_dbus_method_invocation_return_error_literal (invocation,
                                                   G_IO_ERROR,
                                                   G_IO_ERROR_INVALID_ARGUMENT,
                                                   "Invalid arguments");
  else
    {
      VfsMount *mount;

      sanitize_spec (spec, invocation);
      mount = match_vfs_mount (spec);
      
      if (mount != NULL)
        g_dbus_method_invocation_return_error_literal (invocation,
                                                       G_IO_ERROR,
                                                       G_IO_ERROR_ALREADY_MOUNTED,
                                                       _("Location is already mounted"));
      else
        {
          mountable = lookup_mountable (spec);
          
          if (mountable == NULL)
            g_dbus_method_invocation_return_error_literal (invocation,
                                                           G_IO_ERROR,
                                                           G_IO_ERROR_NOT_MOUNTED,
                                                           _("Location is not mountable"));
          else
            {
              GMountSource *source;

              source = g_mount_source_from_dbus (arg_mount_source);
              mountable_mount (mountable,
                               spec,
                               source,
                               FALSE,
                               mount_location_done, g_object_ref (invocation));
              g_object_unref (source);
            }
        }
    }
  
  if (spec)
    g_mount_spec_unref (spec);

  return TRUE;
}

static gboolean
handle_list_mount_types (GVfsDBusMountTracker *object,
                         GDBusMethodInvocation *invocation,
                         gpointer user_data)
{
  VfsMountable *mountable;
  GPtrArray *types;
  GList *l;

  types = g_ptr_array_new ();
  for (l = mountables; l != NULL; l = l->next)
    {
      mountable = l->data;
      g_ptr_array_add (types, (gpointer) mountable->type);
    }
  g_ptr_array_add (types, NULL);

  gvfs_dbus_mount_tracker_complete_list_mount_types (object, invocation,
                                                     (const gchar *const *) types->pdata);

  g_ptr_array_free (types, TRUE);
  return TRUE;
}

static gboolean
handle_list_mountable_info (GVfsDBusMountTracker *object,
                            GDBusMethodInvocation *invocation,
                            gpointer user_data)
{
  GList *l;
  GVariantBuilder mountables_array;
  
  g_variant_builder_init (&mountables_array, G_VARIANT_TYPE (VFS_MOUNTABLE_ARRAY_DBUS_STRUCT_TYPE));
  for (l = mountables; l != NULL; l = l->next) {
    g_variant_builder_add_value (&mountables_array, vfs_mountable_to_dbus (l->data));
  }
  
  gvfs_dbus_mount_tracker_complete_list_mountable_info (object, invocation,
                                                        g_variant_builder_end (&mountables_array));
  
  return TRUE;
}

static gboolean 
handle_register_fuse (GVfsDBusMountTracker *object,
                      GDBusMethodInvocation *invocation,
                      gpointer user_data)
{
  fuse_available = TRUE;
  gvfs_dbus_mount_tracker_complete_register_fuse (object, invocation);
  
  return TRUE;
}

static gboolean
handle_unregister_mount (GVfsDBusMountTracker *object,
                         GDBusMethodInvocation *invocation,
                         const gchar *arg_obj_path,
                         gpointer user_data)
{
  const char *id;

  id = g_dbus_method_invocation_get_sender (invocation);

  if (find_vfs_mount (id, arg_obj_path) == NULL) {
    g_dbus_method_invocation_return_error_literal (invocation,
                                                   G_IO_ERROR,
                                                   G_IO_ERROR_NOT_MOUNTED,
                                                   "Mountpoint not registered");
    return TRUE;
  }

  dbus_client_disconnected (id);

  gvfs_dbus_mount_tracker_complete_unregister_mount (object, invocation);

  return TRUE;
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

gboolean
mount_init (void)
{
  GDBusConnection *conn;
  struct sigaction sa;
  GIOChannel *io;
  GError *error;
  gboolean res;
  
  res = TRUE;

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
  
  
  error = NULL;
  conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (! conn)
    {
      g_warning ("Error connecting to session bus: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      return FALSE;
    }
  
  mount_tracker = gvfs_dbus_mount_tracker_skeleton_new ();

  g_signal_connect (mount_tracker, "handle-register-fuse", G_CALLBACK (handle_register_fuse), NULL);
  g_signal_connect (mount_tracker, "handle-register-mount", G_CALLBACK (handle_register_mount), NULL);
  g_signal_connect (mount_tracker, "handle-mount-location", G_CALLBACK (handle_mount_location), NULL);
  g_signal_connect (mount_tracker, "handle-lookup-mount", G_CALLBACK (handle_lookup_mount), NULL);
  g_signal_connect (mount_tracker, "handle-lookup-mount-by-fuse-path", G_CALLBACK (handle_lookup_mount_by_fuse_path), NULL);
  g_signal_connect (mount_tracker, "handle-list-mounts", G_CALLBACK (handle_list_mounts), NULL);
  g_signal_connect (mount_tracker, "handle-list-mounts2", G_CALLBACK (handle_list_mounts2), NULL);
  g_signal_connect (mount_tracker, "handle-list-mountable-info", G_CALLBACK (handle_list_mountable_info), NULL);
  g_signal_connect (mount_tracker, "handle-list-mount-types", G_CALLBACK (handle_list_mount_types), NULL);
  g_signal_connect (mount_tracker, "handle-unregister-mount", G_CALLBACK (handle_unregister_mount), NULL);
  
  error = NULL;
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (mount_tracker), conn,
                                         G_VFS_DBUS_MOUNTTRACKER_PATH, &error))
    {
      g_warning ("Error exporting mount tracker: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      g_object_unref (mount_tracker);
      mount_tracker = NULL;
      res = FALSE;
    }
  g_object_unref (conn);
  
  return res;
}

void
mount_finalize (void)
{
  if (mount_tracker != NULL)
    {
      g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (mount_tracker));
      g_object_unref (mount_tracker);
    }
}
