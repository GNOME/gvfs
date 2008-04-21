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
#include <signal.h>
#include <stdlib.h>
#include <dbus/dbus.h>
#include "gdaemonvfs.h"
#include "gvfsuriutils.h"
#include "gdaemonfile.h"
#include <gio/gio.h>
#include <gvfsdaemonprotocol.h>
#include <gmodule.h>
#include "gvfsdaemondbus.h"
#include "gdbusutils.h"
#include "gmountspec.h"
#include "gvfsurimapper.h"
#include "gdaemonvolumemonitor.h"
#include <glib/gi18n-lib.h>

typedef struct  {
  char *type;
  char *scheme;
  char **scheme_aliases;
  int default_port;
  gboolean host_is_inet;
} MountableInfo; 

struct _GDaemonVfs
{
  GVfs parent;

  DBusConnection *async_bus;
  
  GVfs *wrapped_vfs;
  GList *mount_cache;

  GHashTable *from_uri_hash;
  GHashTable *to_uri_hash;

  MountableInfo **mountable_info;
  char **supported_uri_schemes;
};

struct _GDaemonVfsClass
{
  GVfsClass parent_class;
};

G_DEFINE_DYNAMIC_TYPE (GDaemonVfs, g_daemon_vfs, G_TYPE_VFS)

static GDaemonVfs *the_vfs = NULL;

G_LOCK_DEFINE_STATIC(mount_cache);


static void fill_mountable_info (GDaemonVfs *vfs);

static void
g_daemon_vfs_finalize (GObject *object)
{
  GDaemonVfs *vfs;

  vfs = G_DAEMON_VFS (object);

  if (vfs->from_uri_hash)
    g_hash_table_destroy (vfs->from_uri_hash);
  
  if (vfs->to_uri_hash)
    g_hash_table_destroy (vfs->to_uri_hash);

  g_strfreev (vfs->supported_uri_schemes);

  if (vfs->async_bus)
    {
      dbus_connection_close (vfs->async_bus);
      dbus_connection_unref (vfs->async_bus);
    }

  if (vfs->wrapped_vfs)
    g_object_unref (vfs->wrapped_vfs);
  
  /* must chain up */
  G_OBJECT_CLASS (g_daemon_vfs_parent_class)->finalize (object);
}

static MountableInfo *
get_mountable_info_for_scheme (GDaemonVfs *vfs,
			       const char *scheme)
{
  MountableInfo *info;
  int i, j;

  if (vfs->mountable_info == NULL)
    return NULL;

  for (i = 0; vfs->mountable_info[i] != NULL; i++)
    {
      info = vfs->mountable_info[i];
      
      if (info->scheme != NULL && strcmp (info->scheme, scheme) == 0)
	return info;

      if (info->scheme_aliases != NULL)
	{
	  for (j = 0; info->scheme_aliases[j] != NULL; j++)
	    {
	      if (strcmp (info->scheme_aliases[j], scheme) == 0)
		return info;
	    }
	}
      
    }
  
  return NULL;
}

static MountableInfo *
get_mountable_info_for_type (GDaemonVfs *vfs,
			     const char *type)
{
  MountableInfo *info;
  int i;
  
  if (vfs->mountable_info == NULL)
    return NULL;
  
  for (i = 0; vfs->mountable_info[i] != NULL; i++)
    {
      info = vfs->mountable_info[i];
      
      if (strcmp (info->type, type) == 0)
	return info;
    }
  
  return NULL;
}

static gboolean
get_mountspec_from_uri (GDaemonVfs *vfs,
			const char *uri,
			GMountSpec **spec_out,
			char **path_out)
{
  GMountSpec *spec;
  char *path;
  GVfsUriMapper *mapper;
  char *scheme;
  GVfsUriMountInfo *info;
  
  scheme = g_uri_parse_scheme (uri);
  if (scheme == NULL)
    return FALSE;
  
  spec = NULL;
  path = NULL;
  
  mapper = g_hash_table_lookup (vfs->from_uri_hash, scheme);
  
  if (mapper)
    {
      info = g_vfs_uri_mapper_from_uri (mapper, uri);
      if (info != NULL)
	{
	  spec = g_mount_spec_new_from_data (info->keys, NULL);
	  path = info->path;
	  /* We took over ownership of info parts, custom free: */
	  g_free (info);
	}
    }
  
  if (spec == NULL)
    {
      GDecodedUri *decoded;
      MountableInfo *mountable;
      char *type, *p;
      int l;

      decoded = g_vfs_decode_uri (uri);
      if (decoded)
	{	
	  mountable = get_mountable_info_for_scheme (vfs, decoded->scheme);
      
	  if (mountable)
	    type = mountable->type;
	  else
	    type = decoded->scheme;
	  
	  spec = g_mount_spec_new (type);
	  
	  if (decoded->host && *decoded->host)
	    {
	      if (mountable && mountable->host_is_inet)
		{
		  /* Convert hostname to lower case */
		  for (p = decoded->host; *p != 0; p++)
		    *p = g_ascii_tolower (*p);
		  
		  /* Remove brackets aroung ipv6 addresses */
		  l = strlen (decoded->host);
		  if (decoded->host[0] == '[' &&
		      decoded->host[l - 1] == ']')
		    g_mount_spec_set_with_len (spec, "host", decoded->host+1, l - 2);
		  else
		    g_mount_spec_set (spec, "host", decoded->host);
		}
	      else
		g_mount_spec_set (spec, "host", decoded->host);
	    }
	  
	  if (decoded->userinfo && *decoded->userinfo)
	    g_mount_spec_set (spec, "user", decoded->userinfo);
	  
	  if (decoded->port != -1 &&
	      (mountable == NULL ||
	       mountable->default_port == 0 ||
	       mountable->default_port != decoded->port))
	    {
	      char *port = g_strdup_printf ("%d", decoded->port);
	      g_mount_spec_set (spec, "port", port);
	      g_free (port);
	    }
	  
	  path = g_strdup (decoded->path);
	  
	  g_vfs_decoded_uri_free (decoded);
	}
    }
  
  g_free (scheme);
  
  if (spec == NULL)
    return FALSE;

  *spec_out = spec;
  *path_out = path;
  
  return TRUE;
}

static void
g_daemon_vfs_init (GDaemonVfs *vfs)
{
  GType *mappers;
  guint n_mappers;
  const char * const *schemes, * const *mount_types;
  GVfsUriMapper *mapper;
  GList *modules;
  int i;

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  vfs->async_bus = dbus_bus_get_private (DBUS_BUS_SESSION, NULL);

  if (vfs->async_bus == NULL)
    return; /* Not supported, return here and return false in vfs_is_active() */

  g_assert (the_vfs == NULL);
  the_vfs = vfs;
  
  if (g_thread_supported ())
    dbus_threads_init_default ();

  /* We disable SIGPIPE globally. This is sort of bad
     for s library to do since its a global resource.
     However, without this there is no way to be able
     to handle mount daemons dying without client apps
     crashing, which is much worse.

     I blame Unix, there really should be a portable
     way to do this on all unixes, but there isn't,
     even for somewhat modern ones like solaris.
  */
  signal (SIGPIPE, SIG_IGN);

  fill_mountable_info (vfs);
  
  vfs->wrapped_vfs = g_vfs_get_local ();

  dbus_connection_set_exit_on_disconnect (vfs->async_bus, FALSE);

  _g_dbus_connection_integrate_with_main (vfs->async_bus);

  modules = g_io_modules_load_all_in_directory (GVFS_MODULE_DIR);

  vfs->from_uri_hash = g_hash_table_new (g_str_hash, g_str_equal);
  vfs->to_uri_hash = g_hash_table_new (g_str_hash, g_str_equal);
  
  mappers = g_type_children (G_VFS_TYPE_URI_MAPPER, &n_mappers);

  for (i = 0; i < n_mappers; i++)
    {
      int j;
      mapper = g_object_new (mappers[i], NULL);

      schemes = g_vfs_uri_mapper_get_handled_schemes (mapper);

      for (j = 0; schemes != NULL && schemes[j] != NULL; j++)
	g_hash_table_insert (vfs->from_uri_hash, (char *)schemes[j], mapper);
      
      mount_types = g_vfs_uri_mapper_get_handled_mount_types (mapper);
      for (j = 0; mount_types != NULL && mount_types[j] != NULL; j++)
	g_hash_table_insert (vfs->to_uri_hash, (char *)mount_types[j], mapper);
    }

  /* The above should have ref:ed the modules anyway */
  g_list_foreach (modules, (GFunc)g_type_module_unuse, NULL);
  g_list_free (modules);
  g_free (mappers);
}

GDaemonVfs *
g_daemon_vfs_new (void)
{
  return g_object_new (G_TYPE_DAEMON_VFS, NULL);
}

static GFile *
g_daemon_vfs_get_file_for_path (GVfs       *vfs,
				const char *path)
{
  /* TODO: detect fuse paths and convert to daemon vfs GFiles */
  
  return g_vfs_get_file_for_path (G_DAEMON_VFS (vfs)->wrapped_vfs, path);
}

static GFile *
g_daemon_vfs_get_file_for_uri (GVfs       *vfs,
			       const char *uri)
{
  GDaemonVfs *daemon_vfs;
  GFile *file;
  GMountSpec *spec;
  char *path;

  daemon_vfs = G_DAEMON_VFS (vfs);

  if (g_str_has_prefix (uri, "file:"))
    {
      path = g_filename_from_uri (uri, NULL, NULL);

      if (path == NULL)
	/* Dummy file */
	return g_vfs_get_file_for_uri (G_DAEMON_VFS (vfs)->wrapped_vfs, uri);
      
      file = g_daemon_vfs_get_file_for_path (vfs, path);
      g_free (path);
      return file;
    }
  
  if (get_mountspec_from_uri (daemon_vfs, uri, &spec, &path))
    {
      file = g_daemon_file_new (spec, path);
      g_mount_spec_unref (spec);
      g_free (path);
      return file;
    }

  /* Dummy file */
  return g_vfs_get_file_for_uri (G_DAEMON_VFS (vfs)->wrapped_vfs, uri);
}

GMountSpec *
_g_daemon_vfs_get_mount_spec_for_path (GMountSpec *spec,
				       const char *path,
				       const char *new_path)
{
  const char *type;
  GVfsUriMapper *mapper;
  GMountSpec *new_spec;

  type = g_mount_spec_get_type (spec);

  if (type == NULL)
    return g_mount_spec_ref (spec);
  
  new_spec = NULL;
  mapper = g_hash_table_lookup (the_vfs->to_uri_hash, type);
  if (mapper)
    {
      GVfsUriMountInfo info, *new_info;
      info.keys = spec->items;
      info.path = (char *)path;
      new_info = g_vfs_uri_mapper_get_mount_info_for_path (mapper, &info, new_path);
      if (new_info != NULL)
	{
	  new_spec = g_mount_spec_new_from_data (new_info->keys, NULL);
	  /* We took over ownership of parts of new_info, custom free: */
	  g_free (new_info->path);
	  g_free (new_info);
	}
    }

  if (new_spec == NULL)
    new_spec = g_mount_spec_ref (spec);

  return new_spec;
}

char *
_g_daemon_vfs_get_uri_for_mountspec (GMountSpec *spec,
				     char *path,
				     gboolean allow_utf8)
{
  char *uri;
  const char *type;
  GVfsUriMapper *mapper;

  type = g_mount_spec_get_type (spec);
  if (type == NULL)
    {
      GString *string = g_string_new ("unknown://");
      if (path)
	g_string_append_uri_escaped (string,
				     path,
				     "!$&'()*+,;=:@/",
				     allow_utf8);
      
      return g_string_free (string, FALSE);
    }

  uri = NULL;
  mapper = g_hash_table_lookup (the_vfs->to_uri_hash, type);
  if (mapper)
    {
      GVfsUriMountInfo info;
      info.keys = spec->items;
      info.path = path;
      uri = g_vfs_uri_mapper_to_uri (mapper, &info, allow_utf8);
    }

  if (uri == NULL)
    {
      GDecodedUri decoded;
      MountableInfo *mountable;
      const char *port;
      gboolean free_host;

      memset (&decoded, 0, sizeof (decoded));
      decoded.port = -1;

      mountable = get_mountable_info_for_type (the_vfs, type);

      if (mountable)
	decoded.scheme = mountable->scheme;
      else
	decoded.scheme = (char *)type;
      decoded.host = (char *)g_mount_spec_get (spec, "host");
      free_host = FALSE;
      if (mountable && mountable->host_is_inet && decoded.host != NULL && strchr (decoded.host, ':') != NULL)
	{
	  free_host = TRUE;
	  decoded.host = g_strconcat ("[", decoded.host, "]", NULL);
	}
      
      decoded.userinfo = (char *)g_mount_spec_get (spec, "user");
      port = g_mount_spec_get (spec, "port");
      if (port != NULL)
	decoded.port = atoi (port);
      
      if (path == NULL)
	decoded.path = "/";
      else
	decoded.path = path;
      
      uri = g_vfs_encode_uri (&decoded, FALSE);
      
      if (free_host)
	g_free (decoded.host);
    }
  
  return uri;
}

const char *
_g_daemon_vfs_mountspec_get_uri_scheme (GMountSpec *spec)
{
  const char *type, *scheme;
  GVfsUriMapper *mapper;
  MountableInfo *mountable;

  type = g_mount_spec_get_type (spec);
  mapper = g_hash_table_lookup (the_vfs->to_uri_hash, type);

  scheme = NULL;
  if (mapper)
    {
      GVfsUriMountInfo info;
      
      info.keys = spec->items;
      info.path = "/";
      
      scheme = g_vfs_uri_mapper_to_uri_scheme (mapper, &info);
    }
  
  if (scheme == NULL)
    {
      mountable = get_mountable_info_for_type (the_vfs, type);
      if (mountable)
	scheme = mountable->scheme;
      else
	scheme = type;
    }
  
  return scheme;
}

static int
find_string (GPtrArray *array, const char *find_me)
{
  int i;
  
  g_return_val_if_fail (find_me != NULL, -1);
  
  for (i = 0; i < array->len; ++i)
    {
      if (strcmp (g_ptr_array_index (array, i), find_me) == 0)
	return i;
    }
  
  return -1;
}


static void
fill_mountable_info (GDaemonVfs *vfs)
{
  DBusMessage *message, *reply;
  DBusError error;
  DBusMessageIter iter, array_iter, struct_iter;
  MountableInfo *info;
  GPtrArray *infos, *uri_schemes;
  gint i, count;

  message = dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
                                          G_VFS_DBUS_MOUNTTRACKER_PATH,
					  G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					  G_VFS_DBUS_MOUNTTRACKER_OP_LIST_MOUNTABLE_INFO);

  if (message == NULL)
    _g_dbus_oom ();
  
  dbus_message_set_auto_start (message, TRUE);
  
  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (vfs->async_bus,
                                                     message,
						     G_VFS_DBUS_TIMEOUT_MSECS,
						     &error);
  dbus_message_unref (message);
  
  if (dbus_error_is_set (&error))
    {
      dbus_error_free (&error);
      return;
    }
  
  if (reply == NULL)
    _g_dbus_oom ();

  dbus_message_iter_init (reply, &iter);

  dbus_message_iter_recurse (&iter, &array_iter);

  infos = g_ptr_array_new ();
  uri_schemes = g_ptr_array_new ();
  count = 0;
  do
    {
      char *type, *scheme, **scheme_aliases;
      int scheme_aliases_len;
      gint32 default_port;
      dbus_bool_t host_is_inet;
      
      if (dbus_message_iter_get_arg_type (&array_iter) != DBUS_TYPE_STRUCT)
	break;
      
      dbus_message_iter_recurse (&array_iter, &struct_iter);
      
      if (!_g_dbus_message_iter_get_args (&struct_iter, NULL,
					  DBUS_TYPE_STRING, &type,
					  DBUS_TYPE_STRING, &scheme,
					  DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &scheme_aliases, &scheme_aliases_len,
					  DBUS_TYPE_INT32, &default_port,
					  DBUS_TYPE_BOOLEAN, &host_is_inet,
					  0))
	break;

      info = g_new0 (MountableInfo, 1);
      info->type = g_strdup (type);
      if (*scheme != 0)
	{
	  info->scheme = g_strdup (scheme);
	  if (find_string (uri_schemes, scheme) == -1)
	    g_ptr_array_add (uri_schemes, g_strdup (scheme));
	}
      
      if (scheme_aliases_len > 0)
	{
	  info->scheme_aliases = g_new (char *, scheme_aliases_len + 1);
	  for (i = 0; i < scheme_aliases_len; i++)
	    {
	      info->scheme_aliases[i] = g_strdup (scheme_aliases[i]);
	      if (find_string (uri_schemes, scheme_aliases[i]) == -1)
		g_ptr_array_add (uri_schemes, g_strdup (scheme_aliases[i]));
	    }
	  info->scheme_aliases[scheme_aliases_len] = NULL;
	}
	
      info->default_port = default_port;
      info->host_is_inet = host_is_inet;
      
      g_ptr_array_add (infos, info);

      g_strfreev (scheme_aliases);
    }
  while (dbus_message_iter_next (&array_iter));

  dbus_message_unref (reply);

  g_ptr_array_add (uri_schemes, NULL);
  g_ptr_array_add (infos, NULL);
  vfs->mountable_info = (MountableInfo **)g_ptr_array_free (infos, FALSE);
  vfs->supported_uri_schemes = (char **)g_ptr_array_free (uri_schemes, FALSE);
}


static const gchar * const *
g_daemon_vfs_get_supported_uri_schemes (GVfs *vfs)
{
  return (const gchar * const *) G_DAEMON_VFS (vfs)->supported_uri_schemes;
}

static GMountInfo *
lookup_mount_info_in_cache_locked (GMountSpec *spec,
				   const char *path)
{
  GMountInfo *info;
  GList *l;

  info = NULL;
  for (l = the_vfs->mount_cache; l != NULL; l = l->next)
    {
      GMountInfo *mount_info = l->data;

      if (g_mount_spec_match_with_path (mount_info->mount_spec, spec, path))
	{
	  info = g_mount_info_ref (mount_info);
	  break;
	}
    }
  
  return info;
}

static GMountInfo *
lookup_mount_info_in_cache (GMountSpec *spec,
			   const char *path)
{
  GMountInfo *info;

  G_LOCK (mount_cache);
  info = lookup_mount_info_in_cache_locked (spec, path);
  G_UNLOCK (mount_cache);

  return info;
}

void
_g_daemon_vfs_invalidate_dbus_id (const char *dbus_id)
{
  GList *l, *next;

  G_LOCK (mount_cache);
  for (l = the_vfs->mount_cache; l != NULL; l = next)
    {
      GMountInfo *mount_info = l->data;
      next = l->next;

      if (strcmp (mount_info->dbus_id, dbus_id) == 0)
	{
	  the_vfs->mount_cache = g_list_delete_link (the_vfs->mount_cache, l);
	  g_mount_info_unref (mount_info);
	}
    }
  
  G_UNLOCK (mount_cache);
}


static GMountInfo *
handler_lookup_mount_reply (DBusMessage *reply,
			    GError **error)
{
  DBusError derror;
  GMountInfo *info;
  DBusMessageIter iter;
  GList *l;
  gboolean in_cache;
  

  if (_g_error_from_message (reply, error))
    return NULL;

  dbus_error_init (&derror);
  dbus_message_iter_init (reply, &iter);

  info = g_mount_info_from_dbus (&iter);
  if (info == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Error while getting mount info: %s"),
		   "Invalid reply");
      return NULL;
    }

  G_LOCK (mount_cache);

  in_cache = FALSE;
  /* Already in cache from other thread? */
  for (l = the_vfs->mount_cache; l != NULL; l = l->next)
    {
      GMountInfo *cached_info = l->data;
      
      if (g_mount_info_equal (info, cached_info))
	{
	  in_cache = TRUE;
	  g_mount_info_unref (info);
	  info = g_mount_info_ref (cached_info);
	  break;
	}
    }

  /* No, lets add it to the cache */
  if (!in_cache)
    the_vfs->mount_cache = g_list_prepend (the_vfs->mount_cache, g_mount_info_ref (info));

  G_UNLOCK (mount_cache);
  
  return info;
}

typedef struct {
  GMountInfoLookupCallback callback;
  gpointer user_data;
} GetMountInfoData;

static void
async_get_mount_info_response (DBusMessage *reply,
			       GError *io_error,
			       void *_data)
{
  GetMountInfoData *data = _data;
  GMountInfo *info;
  GError *error;

  if (reply == NULL)
    data->callback (NULL, data->user_data, io_error);
  else
    {
      error = NULL;
      info = handler_lookup_mount_reply (reply, &error);

      data->callback (info, data->user_data, error);

      if (info)
	g_mount_info_unref (info);

      if (error)
	g_error_free (error);
    }
  
  g_free (data);
}

void
_g_daemon_vfs_get_mount_info_async (GMountSpec *spec,
				    const char *path,
				    GMountInfoLookupCallback callback,
				    gpointer user_data)
{
  GMountInfo *info;
  GetMountInfoData *data;
  DBusMessage *message;
  DBusMessageIter iter;
  
  info = lookup_mount_info_in_cache (spec, path);

  if (info != NULL)
    {
      callback (info, user_data, NULL);
      g_mount_info_unref (info);
      return;
    }

  message =
    dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
				  G_VFS_DBUS_MOUNTTRACKER_PATH,
				  G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
				  G_VFS_DBUS_MOUNTTRACKER_OP_LOOKUP_MOUNT);
  dbus_message_set_auto_start (message, TRUE);
  
  dbus_message_iter_init_append (message, &iter);
  g_mount_spec_to_dbus_with_path (&iter, spec, path);

  data = g_new0 (GetMountInfoData, 1);
  data->callback = callback;
  data->user_data = user_data;
  
  _g_dbus_connection_call_async (the_vfs->async_bus, message, G_VFS_DBUS_TIMEOUT_MSECS,
				 async_get_mount_info_response,
				 data);
  
  dbus_message_unref (message);
}


GMountInfo *
_g_daemon_vfs_get_mount_info_sync (GMountSpec *spec,
				   const char *path,
				   GError **error)
{
  GMountInfo *info;
  DBusConnection *conn;
  DBusMessage *message, *reply;
  DBusMessageIter iter;
  DBusError derror;
	
  info = lookup_mount_info_in_cache (spec, path);

  if (info != NULL)
    return info;
  
  conn = _g_dbus_connection_get_sync (NULL, error);
  if (conn == NULL)
    return NULL;

  message =
    dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
				  G_VFS_DBUS_MOUNTTRACKER_PATH,
				  G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
				  G_VFS_DBUS_MOUNTTRACKER_OP_LOOKUP_MOUNT);
  dbus_message_set_auto_start (message, TRUE);
  
  dbus_message_iter_init_append (message, &iter);
  g_mount_spec_to_dbus_with_path (&iter, spec, path);

  dbus_error_init (&derror);
  reply = dbus_connection_send_with_reply_and_block (conn, message, -1, &derror);
  dbus_message_unref (message);

  if (!reply)
    {
      _g_error_from_dbus (&derror, error);
      dbus_error_free (&derror);
      return NULL;
    }

  info = handler_lookup_mount_reply (reply, error);

  dbus_message_unref (reply);
  
  return info;
}

static GFile *
g_daemon_vfs_parse_name (GVfs       *vfs,
			 const char *parse_name)
{
  GFile *file;
  
  if (g_path_is_absolute (parse_name) ||
      *parse_name == '~')
    {
      /* TODO: detect fuse paths and convert to daemon vfs GFiles ? */
      file = g_vfs_parse_name (G_DAEMON_VFS (vfs)->wrapped_vfs, parse_name);
    }
  else
    {
      file = g_daemon_vfs_get_file_for_uri (vfs, parse_name);
    }

  return file;
}

DBusConnection *
_g_daemon_vfs_get_async_bus (void)
{
  return the_vfs->async_bus;
}

static gboolean
g_daemon_vfs_is_active (GVfs *vfs)
{
  GDaemonVfs *daemon_vfs = G_DAEMON_VFS (vfs);
  return daemon_vfs->async_bus != NULL;
}

static void
g_daemon_vfs_class_finalize (GDaemonVfsClass *klass)
{
}

static void
g_daemon_vfs_class_init (GDaemonVfsClass *class)
{
  GObjectClass *object_class;
  GVfsClass *vfs_class;
  
  object_class = (GObjectClass *) class;

  g_daemon_vfs_parent_class = g_type_class_peek_parent (class);
  
  object_class->finalize = g_daemon_vfs_finalize;

  vfs_class = G_VFS_CLASS (class);

  vfs_class->is_active = g_daemon_vfs_is_active;
  vfs_class->get_file_for_path = g_daemon_vfs_get_file_for_path;
  vfs_class->get_file_for_uri = g_daemon_vfs_get_file_for_uri;
  vfs_class->get_supported_uri_schemes = g_daemon_vfs_get_supported_uri_schemes;
  vfs_class->parse_name = g_daemon_vfs_parse_name;
}

/* Module API */

void g_vfs_uri_mapper_smb_register (GIOModule *module);
void g_vfs_uri_mapper_http_register (GIOModule *module);
void g_vfs_uri_mapper_sftp_register (GIOModule *module);

void
g_io_module_load (GIOModule *module)
{
  /* This is so that system daemons can use gio
   * without spawning private dbus instances.
   * See bug 526454.
   */
  if (g_getenv ("DBUS_SESSION_BUS_ADDRESS") == NULL) 
    return;

  g_daemon_vfs_register_type (G_TYPE_MODULE (module));
  g_daemon_volume_monitor_register_types (G_TYPE_MODULE (module));

  g_io_extension_point_implement (G_VFS_EXTENSION_POINT_NAME,
				  G_TYPE_DAEMON_VFS,
				  "gvfs",
				  10);
  
  g_vfs_uri_mapper_register (module);
  g_vfs_uri_mapper_smb_register (module);
  g_vfs_uri_mapper_http_register (module);
}

void
g_io_module_unload (GIOModule   *module)
{
}
