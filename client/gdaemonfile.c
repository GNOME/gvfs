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

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "gdaemonfile.h"
#include "gdaemonvfs.h"
#include "gvfsdaemondbus.h"
#include "gdaemonmount.h"
#include <gvfsdaemonprotocol.h>
#include <gdaemonfileinputstream.h>
#include <gdaemonfileoutputstream.h>
#include <gdaemonfilemonitor.h>
#include <gdaemonfileenumerator.h>
#include <glib/gi18n-lib.h>
#include "gmountoperationdbus.h"
#include <gio/gio.h>
#include "metatree.h"
#include <metadata-dbus.h>
#include <gvfsdbus.h>
#include <gio/gunixfdlist.h>

static void g_daemon_file_file_iface_init (GFileIface       *iface);

static void g_daemon_file_read_async (GFile *file,
				      int io_priority,
				      GCancellable *cancellable,
				      GAsyncReadyCallback callback,
				      gpointer callback_data);

G_DEFINE_TYPE_WITH_CODE (GDaemonFile, g_daemon_file, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						g_daemon_file_file_iface_init))

static void
g_daemon_file_finalize (GObject *object)
{
  GDaemonFile *daemon_file;

  daemon_file = G_DAEMON_FILE (object);

  g_mount_spec_unref (daemon_file->mount_spec);
  g_free (daemon_file->path);
  
  if (G_OBJECT_CLASS (g_daemon_file_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_file_parent_class)->finalize) (object);
}

static void
g_daemon_file_class_init (GDaemonFileClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_daemon_file_finalize;
}

static void
g_daemon_file_init (GDaemonFile *daemon_file)
{
}

static guint32
get_pid_for_file (GFile *file)
{
  guint32 pid;

  pid = 0;
  if (file == NULL)
    goto out;

  /* The fuse client sets this to convey the pid of the client - see
   * set_pid_for_file() in gvfsfusedaemon.c
   */
  pid = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (file), "gvfs-fuse-client-pid"));
  if (pid != 0)
    goto out;

  /* otherwise assume the client is this process */
  pid = (guint32) getpid ();

 out:
  return pid;
}

GFile *
g_daemon_file_new (GMountSpec *mount_spec,
		   const char *path)
{
  GDaemonFile *daemon_file;

  daemon_file = g_object_new (G_TYPE_DAEMON_FILE, NULL);
  daemon_file->mount_spec = g_mount_spec_get_unique_for (mount_spec);
  daemon_file->path = g_mount_spec_canonicalize_path (path);
 
  return G_FILE (daemon_file);
}

static gboolean
g_daemon_file_is_native (GFile *file)
{
  return FALSE;
}

static gboolean
g_daemon_file_has_uri_scheme (GFile *file,
			      const char *uri_scheme)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  const char *scheme;

  scheme = _g_daemon_vfs_mountspec_get_uri_scheme (daemon_file->mount_spec);
  return g_ascii_strcasecmp (scheme, uri_scheme) == 0;
}

static char *
g_daemon_file_get_uri_scheme (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  const char *scheme;

  scheme = _g_daemon_vfs_mountspec_get_uri_scheme (daemon_file->mount_spec);
  
  return g_strdup (scheme);
}

static char *
g_daemon_file_get_basename (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  char *last_slash;    

  /* This code relies on the path being canonicalized */
  
  last_slash = strrchr (daemon_file->path, '/');
  /* If no slash, or only "/" fallback to full path */
  if (last_slash == NULL ||
      last_slash[1] == '\0')
    return g_strdup (daemon_file->path);

  return g_strdup (last_slash + 1);
}

static char *
g_daemon_file_get_path (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  GMountInfo *mount_info;
  const char *rel_path;
  char *path;

  /* This is a sync i/o call, which is a bit unfortunate, as
   * this is supposed to be a fast call. However, in almost all
   * cases this will be cached.
   */
  
  mount_info = _g_daemon_vfs_get_mount_info_sync (daemon_file->mount_spec,
						  daemon_file->path,
						  NULL,  /* TODO: cancellable */
						  NULL);

  if (mount_info == NULL)
    return NULL;

  path = NULL;
  
  if (mount_info->fuse_mountpoint)
    {
      rel_path = daemon_file->path +
	strlen (mount_info->mount_spec->mount_prefix);

      path = g_build_filename (mount_info->fuse_mountpoint, rel_path, NULL);
    }
  
  g_mount_info_unref (mount_info);
  
  return path;
}

static char *
g_daemon_file_get_uri (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);

  return _g_daemon_vfs_get_uri_for_mountspec (daemon_file->mount_spec,
					      daemon_file->path,
					      FALSE);
}

static char *
g_daemon_file_get_parse_name (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);

  return _g_daemon_vfs_get_uri_for_mountspec (daemon_file->mount_spec,
					      daemon_file->path,
					      TRUE);
}

static GFile *
new_file_for_new_path (GDaemonFile *daemon_file,
		       const char *new_path)
{
  GFile *new_file;
  GMountSpec *new_spec;

  new_spec = _g_daemon_vfs_get_mount_spec_for_path (daemon_file->mount_spec,
						    daemon_file->path,
						    new_path);

  new_file = g_daemon_file_new (new_spec, new_path);
  g_mount_spec_unref (new_spec);

  return new_file;
}

static GFile *
g_daemon_file_get_parent (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  const char *path;
  GFile *parent;
  const char *base;
  char *parent_path;
  gsize len;    

  path = daemon_file->path;
  base = strrchr (path, '/');
  if (base == NULL ||
      *(base+1) == 0)
    return NULL;

  while (base > path && *base == '/')
    base--;

  len = (guint) 1 + base - path;
  
  parent_path = g_new (gchar, len + 1);
  g_memmove (parent_path, path, len);
  parent_path[len] = 0;

  parent = new_file_for_new_path (daemon_file, parent_path);
  g_free (parent_path);
  
  return parent;
}

static GFile *
g_daemon_file_dup (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);

  return g_daemon_file_new (daemon_file->mount_spec,
			    daemon_file->path);
}

static guint
g_daemon_file_hash (GFile *file)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);

  return
    g_str_hash (daemon_file->path) ^
    GPOINTER_TO_UINT (daemon_file->mount_spec);  /* We have unique mount_spec objects so hash directly on it */
}

static gboolean
g_daemon_file_equal (GFile *file1,
		     GFile *file2)
{
  GDaemonFile *daemon_file1 = G_DAEMON_FILE (file1);
  GDaemonFile *daemon_file2 = G_DAEMON_FILE (file2);

  return daemon_file1->mount_spec == daemon_file2->mount_spec && 
    g_str_equal (daemon_file1->path, daemon_file2->path);
}


static const char *
match_prefix (const char *path, const char *prefix)
{
  int prefix_len;

  prefix_len = strlen (prefix);
  if (strncmp (path, prefix, prefix_len) != 0)
    return NULL;

  /* Handle the case where prefix is the root, so that
   * the IS_DIR_SEPRARATOR check below works */
  if (prefix_len > 0 &&
      prefix[prefix_len-1] == '/')
    prefix_len--;
  
  
  return path + prefix_len;
}

static gboolean
g_daemon_file_prefix_matches (GFile *parent,
			      GFile *descendant)
{
  GDaemonFile *parent_daemon = G_DAEMON_FILE (parent);
  GDaemonFile *descendant_daemon = G_DAEMON_FILE (descendant);
  const char *remainder;

  if (descendant_daemon->mount_spec == parent_daemon->mount_spec)
    {
      remainder = match_prefix (descendant_daemon->path, parent_daemon->path);
      if (remainder != NULL && *remainder == '/')
        return TRUE;
      else
        return FALSE;
    }
  else
    {
      /* If descendant was created with g_file_new_for_uri(), it's
         mount_prefix is /, but parent might have a different mount_prefix,
         for example if obtained by g_mount_get_root()
      */
      char *full_path;
      gboolean ok;

      full_path = g_build_path ("/", descendant_daemon->mount_spec->mount_prefix,
                                descendant_daemon->path, NULL);
      ok = g_mount_spec_match_with_path (parent_daemon->mount_spec,
                                         descendant_daemon->mount_spec,
                                         full_path);

      g_free (full_path);
      return ok;
    }
}

static char *
g_daemon_file_get_relative_path (GFile *parent,
				 GFile *descendant)
{
  GDaemonFile *parent_daemon = G_DAEMON_FILE (parent);
  GDaemonFile *descendant_daemon = G_DAEMON_FILE (descendant);

  if (descendant_daemon->mount_spec == parent_daemon->mount_spec)
    {
      const char *remainder;

      remainder = match_prefix (descendant_daemon->path, parent_daemon->path);

      if (remainder != NULL && *remainder == '/')
        return g_strdup (remainder + 1);
      else
        return NULL;
    }
  else
    {
      char *full_path_descendant;
      char *full_path_parent;
      char *ret;
      const char *remainder;

      full_path_descendant = g_build_path ("/", descendant_daemon->mount_spec->mount_prefix,
                                           descendant_daemon->path, NULL);

      if (!g_mount_spec_match_with_path (parent_daemon->mount_spec,
                                         descendant_daemon->mount_spec,
                                         full_path_descendant))
        {
          g_free (full_path_descendant);
          return NULL;
        }

      full_path_parent = g_build_path ("/", parent_daemon->mount_spec->mount_prefix,
                                       parent_daemon->path, NULL);

      remainder = match_prefix (full_path_descendant, full_path_parent);
      if (remainder == NULL || *remainder != '/')
        ret = g_strdup (remainder + 1);
      else
        ret = NULL;

      g_free (full_path_parent);
      g_free (full_path_descendant);
      return ret;
    }
}

static GFile *
g_daemon_file_resolve_relative_path (GFile *file,
				     const char *relative_path)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  char *path;
  GFile *child;

  if (*relative_path == '/')
    return new_file_for_new_path (daemon_file, relative_path);

  path = g_build_path ("/", daemon_file->path, relative_path, NULL);
  child = new_file_for_new_path (daemon_file, path);
  g_free (path);
  
  return child;
}

static GVfsDBusMount *
create_proxy_for_file2 (GFile *file1,
                        GFile *file2,
                        GMountInfo **mount_info1_out,
                        GMountInfo **mount_info2_out,
                        gchar **path1_out,
                        gchar **path2_out,
                        GDBusConnection **connection_out,
                        GCancellable *cancellable,
                        GError **error)
{
  GVfsDBusMount *proxy;
  GDaemonFile *daemon_file1 = G_DAEMON_FILE (file1);
  GDaemonFile *daemon_file2 = G_DAEMON_FILE (file2);
  GMountInfo *mount_info1, *mount_info2;
  GDBusConnection *connection;

  proxy = NULL;
  mount_info2 = NULL;
  
  mount_info1 = _g_daemon_vfs_get_mount_info_sync (daemon_file1->mount_spec,
                                                   daemon_file1->path,
                                                   cancellable,
                                                   error);
  
  if (mount_info1 == NULL)
    goto out;

  if (daemon_file2)
    {
      mount_info2 = _g_daemon_vfs_get_mount_info_sync (daemon_file2->mount_spec,
                                                       daemon_file2->path,
                                                       cancellable,
                                                       error);
      if (mount_info2 == NULL)
        goto out;

      if (! g_mount_info_equal (mount_info1, mount_info2))
        {
          /* For copy this will cause the fallback code to be involved */
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                               _("Operation not supported, files on different mounts"));
          goto out;
        }
    }

  connection = _g_dbus_connection_get_sync (mount_info1->dbus_id, cancellable, error);
  if (connection == NULL)
    goto out;

  proxy = gvfs_dbus_mount_proxy_new_sync (connection,
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                          mount_info1->dbus_id,
                                          mount_info1->object_path,
                                          cancellable,
                                          error);
  
  if (proxy == NULL)
    goto out;
  
  /* Set infinite timeout, see bug 687534 */
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), G_MAXINT);

  if (mount_info1_out)
    *mount_info1_out = g_mount_info_ref (mount_info1);
  if (mount_info2_out && mount_info2)
    *mount_info2_out = g_mount_info_ref (mount_info2);
  if (path1_out)
    *path1_out = g_strdup (g_mount_info_resolve_path (mount_info1, daemon_file1->path));
  if (path2_out && mount_info2)
    *path2_out = g_strdup (g_mount_info_resolve_path (mount_info2, daemon_file2->path));
  if (connection_out)
    *connection_out = connection;

 out:
  if (mount_info1)
    g_mount_info_unref (mount_info1);
  if (mount_info2)
    g_mount_info_unref (mount_info2);
  if (error && *error)
    g_dbus_error_strip_remote_error (*error);

  return proxy;
}

static GVfsDBusMount *
create_proxy_for_file (GFile *file,
                       GMountInfo **mount_info_out,
                       gchar **path_out,
                       GDBusConnection **connection_out,
                       GCancellable *cancellable,
                       GError **error)
{
  return create_proxy_for_file2 (file, NULL,
                                 mount_info_out, NULL,
                                 path_out, NULL,
                                 connection_out,
                                 cancellable,
                                 error);
}


typedef void (*CreateProxyAsyncCallback) (GVfsDBusMount *proxy,
                                          GDBusConnection *connection,
                                          GMountInfo *mount_info,
                                          const gchar *path,
                                          GSimpleAsyncResult *result,
                                          GError *error,
                                          GCancellable *cancellable,
                                          gpointer callback_data);

typedef struct {
  GSimpleAsyncResult *result;
  GFile *file;
  char *op;
  GCancellable *cancellable;
  CreateProxyAsyncCallback callback;
  gpointer callback_data;
  GDestroyNotify notify;
  GMountInfo *mount_info;
  GDBusConnection *connection;
  GVfsDBusMount *proxy;
} AsyncProxyCreate;

static void
async_proxy_create_free (AsyncProxyCreate *data)
{
  if (data->notify)
    data->notify (data->callback_data);

  g_clear_object (&data->result);
  g_clear_object (&data->file);
  g_free (data->op);
  g_clear_object (&data->cancellable);
  if (data->mount_info)
    g_mount_info_unref (data->mount_info);
  g_clear_object (&data->connection);
  g_clear_object (&data->proxy);
  g_free (data);
}

static void
async_proxy_new_cb (GObject *source_object,
                    GAsyncResult *res,
                    gpointer user_data)
{
  AsyncProxyCreate *data = user_data;
  GDaemonFile *daemon_file = G_DAEMON_FILE (data->file);
  const char *path;
  GVfsDBusMount *proxy;
  GError *error = NULL;
  GSimpleAsyncResult *result;
  
  proxy = gvfs_dbus_mount_proxy_new_finish (res, &error);
  if (proxy == NULL)
    {
      _g_simple_async_result_take_error_stripped (data->result, error);
      _g_simple_async_result_complete_with_cancellable (data->result, data->cancellable);
      async_proxy_create_free (data);
      return;
    }
  
  data->proxy = proxy;

  /* Set infinite timeout, see bug 687534 */
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (data->proxy), G_MAXINT);

  path = g_mount_info_resolve_path (data->mount_info, daemon_file->path);

  /* Complete the create_proxy_for_file_async() call */
  result = data->result;
  g_object_weak_ref (G_OBJECT (result), (GWeakNotify)async_proxy_create_free, data);
  data->result = NULL;
  
  data->callback (proxy,
                  data->connection,
                  data->mount_info,
                  path,
                  result,
                  NULL,
                  data->cancellable,
                  data->callback_data);

  /* Free data here, or later if callback ref:ed the result */
  g_object_unref (result);
}

static void
async_construct_proxy (GDBusConnection *connection,
                       AsyncProxyCreate *data)
{
  data->connection = g_object_ref (connection);
  gvfs_dbus_mount_proxy_new (connection,
                             G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                             data->mount_info->dbus_id,
                             data->mount_info->object_path,
                             data->cancellable,
                             async_proxy_new_cb,
                             data);
}

static void
bus_get_cb (GObject *source_object,
            GAsyncResult *res,
            gpointer user_data)
{
  AsyncProxyCreate *data = user_data;
  GDBusConnection *connection;
  GError *error = NULL;
  
  connection = g_bus_get_finish (res, &error);
  
  if (connection == NULL)
    {
      _g_simple_async_result_take_error_stripped (data->result, error);
      _g_simple_async_result_complete_with_cancellable (data->result, data->cancellable);
      async_proxy_create_free (data);
      return;
    }

  async_construct_proxy (connection, data);
}

static void
async_got_connection_cb (GDBusConnection *connection,
                         GError *io_error,
                         gpointer callback_data)
{
  AsyncProxyCreate *data = callback_data;
  
  if (connection == NULL)
    {
      /* TODO: we should probably test if we really want a session bus;
       *       for now, this code is on par with the old dbus code */ 
      g_bus_get (G_BUS_TYPE_SESSION,
                 data->cancellable,
                 bus_get_cb,
                 data);
      return;
    }
  
  async_construct_proxy (connection, data);
}

static void
async_got_mount_info (GMountInfo *mount_info,
                      gpointer _data,
                      GError *error)
{
  AsyncProxyCreate *data = _data;

  if (error != NULL)
    {
      g_dbus_error_strip_remote_error (error);
      g_simple_async_result_set_from_error (data->result, error);      
      _g_simple_async_result_complete_with_cancellable (data->result, data->cancellable);
      async_proxy_create_free (data);
      return;
    }

  data->mount_info = g_mount_info_ref (mount_info);

  _g_dbus_connection_get_for_async (mount_info->dbus_id,
                                    async_got_connection_cb,
                                    data,
                                    data->cancellable);
}

static void
create_proxy_for_file_async (GFile *file,
                             GCancellable *cancellable,
                             GAsyncReadyCallback op_callback,
                             gpointer op_callback_data,
                             CreateProxyAsyncCallback callback,
                             gpointer callback_data,
                             GDestroyNotify notify)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  AsyncProxyCreate *data;

  data = g_new0 (AsyncProxyCreate, 1);

  data->result = g_simple_async_result_new (G_OBJECT (file),
                                            op_callback, op_callback_data,
                                            NULL);

  data->file = g_object_ref (file);
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);
  data->callback = callback;
  data->callback_data = callback_data;
  data->notify = notify;
  
  _g_daemon_vfs_get_mount_info_async (daemon_file->mount_spec,
                                      daemon_file->path,
                                      async_got_mount_info,
                                      data);
}

static GFileEnumerator *
g_daemon_file_enumerate_children (GFile      *file,
				  const char *attributes,
				  GFileQueryInfoFlags flags,
				  GCancellable *cancellable,
				  GError **error)
{
  char *obj_path;
  char *path;
  GDaemonFileEnumerator *enumerator;
  GDBusConnection *connection;
  char *uri;
  GVfsDBusMount *proxy;
  gboolean res;
  GError *local_error = NULL;

  proxy = create_proxy_for_file (file, NULL, &path, &connection, cancellable, error);
  if (proxy == NULL)
    return NULL;

  enumerator = g_daemon_file_enumerator_new (file, proxy, attributes, TRUE);

  obj_path = g_daemon_file_enumerator_get_object_path (enumerator);
  uri = g_file_get_uri (file);

  res = gvfs_dbus_mount_call_enumerate_sync (proxy,
                                             path,
                                             obj_path,
                                             attributes ? attributes : "",
                                             flags,
                                             uri,
                                             cancellable,
                                             &local_error);

  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);
    }
  
  g_free (path);
  g_free (uri);
  g_free (obj_path);
  g_object_unref (proxy);

  if (! res)
    goto out;
  
  g_daemon_file_enumerator_set_sync_connection (enumerator, connection);
  
  return G_FILE_ENUMERATOR (enumerator);
  
out:
  g_clear_object (&enumerator);
  return NULL;
}


static gboolean
enumerate_keys_callback (const char *key,
			 MetaKeyType type,
			 gpointer value,
			 gpointer user_data)
{
  GFileInfo  *info = user_data;
  char *attr;

  attr = g_strconcat ("metadata::", key, NULL);

  if (type == META_KEY_TYPE_STRING)
    g_file_info_set_attribute_string (info, attr, (char *)value);
  else
    g_file_info_set_attribute_stringv (info, attr, (char **)value);

  g_free (attr);

  return TRUE;
}

static void
add_metadata (GFile *file,
	      const char *attributes,
	      GFileInfo *info)
{
  GDaemonFile *daemon_file;
  GFileAttributeMatcher *matcher;
  const char *first;
  char *treename;
  gboolean all;
  MetaTree *tree;

  daemon_file = G_DAEMON_FILE (file);

  matcher = g_file_attribute_matcher_new (attributes);
  all = g_file_attribute_matcher_enumerate_namespace (matcher, "metadata");

  first = NULL;
  if (!all)
    {
      first = g_file_attribute_matcher_enumerate_next (matcher);

      if (first == NULL)
	{
	  g_file_attribute_matcher_unref (matcher);
	  return; /* No match */
	}
    }

  treename = g_mount_spec_to_string (daemon_file->mount_spec);
  tree = meta_tree_lookup_by_name (treename, FALSE);
  g_free (treename);

  if (tree)
    {
      g_file_info_set_attribute_mask (info, matcher);
      meta_tree_enumerate_keys (tree, daemon_file->path,
                                enumerate_keys_callback, info);
      g_file_info_unset_attribute_mask (info);

      meta_tree_unref (tree);
    }

  g_file_attribute_matcher_unref (matcher);
}

static GFileInfo *
g_daemon_file_query_info (GFile                *file,
			  const char           *attributes,
			  GFileQueryInfoFlags   flags,
			  GCancellable         *cancellable,
			  GError              **error)
{
  char *path;
  GFileInfo *info;
  char *uri;
  GVfsDBusMount *proxy;
  GVariant *iter_info;
  gboolean res;
  GError *local_error = NULL;

  proxy = create_proxy_for_file (file, NULL, &path, NULL, cancellable, error);
  if (proxy == NULL)
    return NULL;

  uri = g_file_get_uri (file);

  iter_info = NULL;
  res = gvfs_dbus_mount_call_query_info_sync (proxy,
                                              path,
                                              attributes ? attributes : "",
                                              flags,
                                              uri,
                                              &iter_info,
                                              cancellable,
                                              &local_error);

  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);
    }

  g_free (path);
  g_free (uri);
  g_object_unref (proxy);

  if (! res)
    return NULL;

  info = _g_dbus_get_file_info (iter_info, error);
  g_variant_unref (iter_info);

  if (info)
    add_metadata (file, attributes, info);
  
  return info;
}


typedef struct {
  GFile *file;
  char *attributes;
  GFileQueryInfoFlags flags;
  int io_priority;
  GSimpleAsyncResult *result;
  GCancellable *cancellable;
  gulong cancelled_tag;
} AsyncCallQueryInfo;

static void
async_call_query_info_free (AsyncCallQueryInfo *data)
{
  g_clear_object (&data->file);
  g_clear_object (&data->result);
  g_clear_object (&data->cancellable);
  g_free (data->attributes);
  g_free (data);
}

static void
query_info_async_cb (GVfsDBusMount *proxy,
                     GAsyncResult *res,
                     gpointer user_data)
{
  AsyncCallQueryInfo *data = user_data;
  GError *error = NULL;
  GSimpleAsyncResult *orig_result;
  GVariant *iter_info;
  GFileInfo *info;
  GFile *file;
  
  orig_result = data->result;
  
  if (! gvfs_dbus_mount_call_query_info_finish (proxy, &iter_info, res, &error))
    {
      _g_simple_async_result_take_error_stripped (orig_result, error);
      goto out;
    }
  
  info = _g_dbus_get_file_info (iter_info, &error);
  g_variant_unref (iter_info);

  if (info == NULL)
    {
      _g_simple_async_result_take_error_stripped (orig_result, error);
      goto out;
    }

  file = G_FILE (g_async_result_get_source_object (G_ASYNC_RESULT (orig_result)));
  add_metadata (file, data->attributes, info);
  g_object_unref (file);

  g_simple_async_result_set_op_res_gpointer (orig_result, info, g_object_unref);
  
out:
  _g_simple_async_result_complete_with_cancellable (orig_result, data->cancellable);
  _g_dbus_async_unsubscribe_cancellable (data->cancellable, data->cancelled_tag);
  data->result = NULL;
  g_object_unref (orig_result);   /* trigger async_proxy_create_free() */
}

static void
query_info_async_get_proxy_cb (GVfsDBusMount *proxy,
                               GDBusConnection *connection,
                               GMountInfo *mount_info,
                               const gchar *path,
                               GSimpleAsyncResult *result,
                               GError *error,
                               GCancellable *cancellable,
                               gpointer callback_data)
{
  AsyncCallQueryInfo *data = callback_data;
  char *uri;

  uri = g_file_get_uri (data->file);
  
  data->result = g_object_ref (result);
  
  gvfs_dbus_mount_call_query_info (proxy,
                                   path,
                                   data->attributes ? data->attributes : "",
                                   data->flags,
                                   uri,
                                   cancellable,
                                   (GAsyncReadyCallback) query_info_async_cb,
                                   data);
  data->cancelled_tag = _g_dbus_async_subscribe_cancellable (connection, cancellable);
  
  g_free (uri);
}

static void
g_daemon_file_query_info_async (GFile                      *file,
				const char                 *attributes,
				GFileQueryInfoFlags         flags,
				int                         io_priority,
				GCancellable               *cancellable,
				GAsyncReadyCallback         callback,
				gpointer                    user_data)
{
  AsyncCallQueryInfo *data;

  data = g_new0 (AsyncCallQueryInfo, 1);
  data->file = g_object_ref (file);
  data->attributes = g_strdup (attributes);
  data->flags = flags;
  data->io_priority = io_priority;
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);

  create_proxy_for_file_async (file,
                               cancellable,
                               callback, user_data,
                               query_info_async_get_proxy_cb,
                               data, (GDestroyNotify) async_call_query_info_free);
}

static GFileInfo *
g_daemon_file_query_info_finish (GFile                      *file,
				 GAsyncResult               *res,
				 GError                    **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GFileInfo *info;

  info = g_simple_async_result_get_op_res_gpointer (simple);
  if (info)
    return g_object_ref (info);
  
  return NULL;
}


typedef struct {
  GFile *file;
  guint16 mode;
  int io_priority;
  gchar *etag;
  gboolean make_backup;
  GFileCreateFlags flags;
  GSimpleAsyncResult *result;
  GCancellable *cancellable;
  gulong cancelled_tag;
} AsyncCallFileReadWrite;

static void
async_call_file_read_write_free (AsyncCallFileReadWrite *data)
{
  g_clear_object (&data->file);
  g_clear_object (&data->result);
  g_clear_object (&data->cancellable);
  g_free (data->etag);
  g_free (data);
}

static void
read_async_cb (GVfsDBusMount *proxy,
               GAsyncResult *res,
               gpointer user_data)
{
  AsyncCallFileReadWrite *data = user_data;
  GError *error = NULL;
  GSimpleAsyncResult *orig_result;
  gboolean can_seek;
  GUnixFDList *fd_list;
  int fd;
  GVariant *fd_id_val;
  guint fd_id;
  GFileInputStream *stream;

  orig_result = data->result;
  
  if (! gvfs_dbus_mount_call_open_for_read_finish (proxy, &fd_id_val, &can_seek, &fd_list, res, &error))
    {
      _g_simple_async_result_take_error_stripped (orig_result, error);
      goto out;
    }

  fd_id = g_variant_get_handle (fd_id_val);
  g_variant_unref (fd_id_val);

  if (fd_list == NULL || g_unix_fd_list_get_length (fd_list) != 1 ||
      (fd = g_unix_fd_list_get (fd_list, fd_id, NULL)) == -1)
    {
      g_simple_async_result_set_error (orig_result,
                                       G_IO_ERROR, G_IO_ERROR_FAILED,
                                       _("Couldn't get stream file descriptor"));
    }
  else
    {
      stream = g_daemon_file_input_stream_new (fd, can_seek);
      g_simple_async_result_set_op_res_gpointer (orig_result, stream, g_object_unref);
      g_object_unref (fd_list);
    }

out:
  _g_simple_async_result_complete_with_cancellable (orig_result, data->cancellable);
  _g_dbus_async_unsubscribe_cancellable (data->cancellable, data->cancelled_tag);
  data->result = NULL;
  g_object_unref (orig_result);   /* trigger async_proxy_create_free() */
}

static void
file_read_async_get_proxy_cb (GVfsDBusMount *proxy,
                               GDBusConnection *connection,
                               GMountInfo *mount_info,
                               const gchar *path,
                               GSimpleAsyncResult *result,
                               GError *error,
                               GCancellable *cancellable,
                               gpointer callback_data)
{
  AsyncCallFileReadWrite *data = callback_data;
  guint32 pid;

  pid = get_pid_for_file (data->file);
  
  data->result = g_object_ref (result);
  
  gvfs_dbus_mount_call_open_for_read (proxy,
                                     path,
                                     pid,
                                     NULL,
                                     cancellable,
                                     (GAsyncReadyCallback) read_async_cb,
                                     data);
  data->cancelled_tag = _g_dbus_async_subscribe_cancellable (connection, cancellable);
}

static void
g_daemon_file_read_async (GFile *file,
			  int io_priority,
			  GCancellable *cancellable,
			  GAsyncReadyCallback callback,
			  gpointer callback_data)
{
  AsyncCallFileReadWrite *data;

  data = g_new0 (AsyncCallFileReadWrite, 1);
  data->file = g_object_ref (file);
  data->io_priority = io_priority;
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);

  create_proxy_for_file_async (file,
                               cancellable,
                               callback, callback_data,
                               file_read_async_get_proxy_cb,
                               data, (GDestroyNotify) async_call_file_read_write_free);
}

static GFileInputStream *
g_daemon_file_read_finish (GFile                  *file,
			   GAsyncResult           *res,
			   GError                **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  gpointer op;

  op = g_simple_async_result_get_op_res_gpointer (simple);
  if (op)
    return g_object_ref (op);
  
  return NULL;
}

static GFileInputStream *
g_daemon_file_read (GFile *file,
		    GCancellable *cancellable,
		    GError **error)
{
  GVfsDBusMount *proxy;
  char *path;
  gboolean res;
  gboolean can_seek;
  GUnixFDList *fd_list;
  int fd;
  GVariant *fd_id_val = NULL;
  guint32 pid;
  GError *local_error = NULL;

  pid = get_pid_for_file (file);

  proxy = create_proxy_for_file (file, NULL, &path, NULL, cancellable, error);
  if (proxy == NULL)
    return NULL;

  res = gvfs_dbus_mount_call_open_for_read_sync (proxy,
                                                 path,
                                                 pid,
                                                 NULL,
                                                 &fd_id_val,
                                                 &can_seek,
                                                 &fd_list,
                                                 cancellable,
                                                 &local_error);

  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);
    }

  g_free (path);
  g_object_unref (proxy);

  if (! res)
    return NULL;

  if (fd_list == NULL || fd_id_val == NULL ||
      g_unix_fd_list_get_length (fd_list) != 1 ||
      (fd = g_unix_fd_list_get (fd_list, g_variant_get_handle (fd_id_val), NULL)) == -1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Didn't get stream file descriptor"));
      return NULL;
    }

  g_variant_unref (fd_id_val);
  g_object_unref (fd_list);
  
  return g_daemon_file_input_stream_new (fd, can_seek);
}

static GFileOutputStream *
file_open_write (GFile *file,
		 guint16 mode,
                 const char *etag,
                 gboolean make_backup,
                 GFileCreateFlags flags,
		 GCancellable *cancellable,
		 GError **error)
{
  GVfsDBusMount *proxy;
  char *path;
  gboolean res;
  guint32 ret_flags;
  GUnixFDList *fd_list;
  int fd;
  GVariant *fd_id_val = NULL;
  guint32 pid;
  guint64 initial_offset;
  GError *local_error = NULL;

  pid = get_pid_for_file (file);

  if (etag == NULL)
    etag = "";

  proxy = create_proxy_for_file (file, NULL, &path, NULL, cancellable, error);
  if (proxy == NULL)
    return NULL;

  res = gvfs_dbus_mount_call_open_for_write_flags_sync (proxy,
                                                        path,
                                                        mode,
                                                        etag,
                                                        make_backup,
                                                        flags,
                                                        pid,
                                                        NULL,
                                                        &fd_id_val,
                                                        &ret_flags,
                                                        &initial_offset,
                                                        &fd_list,
                                                        cancellable,
                                                        &local_error);

  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);
    }

  g_free (path);
  g_object_unref (proxy);

  if (! res)
    return NULL;
  
  if (fd_list == NULL || fd_id_val == NULL ||
      g_unix_fd_list_get_length (fd_list) != 1 ||
      (fd = g_unix_fd_list_get (fd_list, g_variant_get_handle (fd_id_val), NULL)) == -1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Didn't get stream file descriptor"));
      return NULL;
    }

  g_variant_unref (fd_id_val);
  g_object_unref (fd_list);
  
  return g_daemon_file_output_stream_new (fd, ret_flags, initial_offset);
}

static GFileOutputStream *
g_daemon_file_append_to (GFile *file,
                         GFileCreateFlags flags,
                         GCancellable *cancellable,
                         GError **error)
{
  return file_open_write (file, 1, "", FALSE, flags, cancellable, error);
}

static GFileOutputStream *
g_daemon_file_create (GFile *file,
		      GFileCreateFlags flags,
		      GCancellable *cancellable,
		      GError **error)
{
  return file_open_write (file, 0, "", FALSE, flags, cancellable, error);
}

static GFileOutputStream *
g_daemon_file_replace (GFile *file,
		       const char *etag,
		       gboolean make_backup,
		       GFileCreateFlags flags,
		       GCancellable *cancellable,
		       GError **error)
{
  return file_open_write (file, 2, etag, make_backup, flags, cancellable, error);
}


typedef struct {
  GSimpleAsyncResult *result;
  GCancellable *cancellable;
  guint32 flags;
  GMountOperation *mount_operation;
  gulong cancelled_tag;
} AsyncMountOp;

static void
free_async_mount_op (AsyncMountOp *data)
{
  g_clear_object (&data->result);
  g_clear_object (&data->cancellable);
  g_clear_object (&data->mount_operation);
  g_free (data);
}

static void
mount_mountable_location_mounted_cb (GObject *source_object,
				     GAsyncResult *res,
				     gpointer user_data)
{
  GSimpleAsyncResult *result = user_data;
  GError *error = NULL;

  if (!g_file_mount_enclosing_volume_finish (G_FILE (source_object), res, &error))
    {
      _g_simple_async_result_take_error_stripped (result, error);
    }

  g_simple_async_result_complete (result);
  g_object_unref (result);
}

static void
mount_mountable_async_cb (GVfsDBusMount *proxy,
                          GAsyncResult *res,
                          gpointer user_data)
{
  AsyncMountOp *data = user_data;
  GSimpleAsyncResult *orig_result;
  GError *error = NULL;
  gboolean is_uri;
  gchar *out_path;
  gboolean must_mount_location;
  GVariant *iter_mountspec;
  GFile *file;
  GMountSpec *mount_spec;
  
  orig_result = data->result;
  data->result = NULL;

  is_uri = FALSE;
  out_path = NULL;
  must_mount_location = FALSE;
  iter_mountspec = NULL;
  if (! gvfs_dbus_mount_call_mount_mountable_finish (proxy,
                                                     &is_uri,
                                                     &out_path,
                                                     &must_mount_location,
                                                     &iter_mountspec,
                                                     res,
                                                     &error))
    {
      _g_simple_async_result_take_error_stripped (orig_result, error);
      goto out;
    }
  
  if (is_uri)
    {
      file = g_file_new_for_uri (out_path);
    }
  else
    {
      mount_spec = g_mount_spec_from_dbus (iter_mountspec);
      g_variant_unref (iter_mountspec);
      
      if (mount_spec == NULL)
        {
          g_simple_async_result_set_error (orig_result,
                                           G_IO_ERROR, G_IO_ERROR_FAILED,
                                           _("Invalid return value from %s"), "call");
          goto out;
        }
      
      file = g_daemon_file_new (mount_spec, out_path);
      g_mount_spec_unref (mount_spec);
    }
  
  g_free (out_path);
  g_simple_async_result_set_op_res_gpointer (orig_result, file, g_object_unref);

  if (must_mount_location)
    {
      g_file_mount_enclosing_volume (file,
                                     0,
                                     data->mount_operation,
                                     data->cancellable,
                                     mount_mountable_location_mounted_cb,
                                     orig_result);
      return;
    }

out:
  _g_simple_async_result_complete_with_cancellable (orig_result, data->cancellable);
  _g_dbus_async_unsubscribe_cancellable (data->cancellable, data->cancelled_tag);
  g_object_unref (orig_result);   /* trigger async_proxy_create_free() */
}

static void
mount_mountable_got_proxy_cb (GVfsDBusMount *proxy,
                              GDBusConnection *connection,
                              GMountInfo *mount_info,
                              const gchar *path,
                              GSimpleAsyncResult *result,
                              GError *error,
                              GCancellable *cancellable,
                              gpointer callback_data)
{
  AsyncMountOp *data = callback_data;
  GMountSource *mount_source;
  const char *dbus_id, *obj_path;

  data->result = g_object_ref (result);

  mount_source = g_mount_operation_dbus_wrap (data->mount_operation, _g_daemon_vfs_get_async_bus ());

  dbus_id = g_mount_source_get_dbus_id (mount_source);
  obj_path = g_mount_source_get_obj_path (mount_source);
  
  gvfs_dbus_mount_call_mount_mountable (proxy,
                                        path,
                                        dbus_id,
                                        obj_path,
                                        cancellable,
                                        (GAsyncReadyCallback) mount_mountable_async_cb,
                                        data);
  data->cancelled_tag = _g_dbus_async_subscribe_cancellable (connection, cancellable);
  
  g_object_unref (mount_source);
}

static void
g_daemon_file_mount_mountable (GFile               *file,
			       GMountMountFlags     flags,
			       GMountOperation     *mount_operation,
			       GCancellable        *cancellable,
			       GAsyncReadyCallback  callback,
			       gpointer             user_data)
{
  AsyncMountOp *data;
 
  data = g_new0 (AsyncMountOp, 1);
  data->flags = flags;
  if (mount_operation)
    data->mount_operation = g_object_ref (mount_operation);
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);

  create_proxy_for_file_async (file,
                               cancellable,
                               callback, user_data,
                               mount_mountable_got_proxy_cb,
                               data, (GDestroyNotify) free_async_mount_op);
}

static GFile *
g_daemon_file_mount_mountable_finish (GFile               *file,
				      GAsyncResult        *result,
				      GError             **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  GFile *result_file;
  
  result_file = g_simple_async_result_get_op_res_gpointer (simple);
  if (result_file)
    return g_object_ref (result_file);
  
  return NULL;
}

static void
start_mountable_async_cb (GVfsDBusMount *proxy,
                          GAsyncResult *res,
                          gpointer user_data)
{
  AsyncMountOp *data = user_data;
  GSimpleAsyncResult *orig_result;
  GError *error = NULL;

  orig_result = data->result;

  if (! gvfs_dbus_mount_call_start_mountable_finish (proxy, res, &error))
    _g_simple_async_result_take_error_stripped (orig_result, error);

  _g_simple_async_result_complete_with_cancellable (orig_result, data->cancellable);
  _g_dbus_async_unsubscribe_cancellable (data->cancellable, data->cancelled_tag);
  data->result = NULL;
  g_object_unref (orig_result);   /* trigger async_proxy_create_free() */
}

static void
start_mountable_got_proxy_cb (GVfsDBusMount *proxy,
                              GDBusConnection *connection,
                              GMountInfo *mount_info,
                              const gchar *path,
                              GSimpleAsyncResult *result,
                              GError *error,
                              GCancellable *cancellable,
                              gpointer callback_data)
{
  AsyncMountOp *data = callback_data;
  GMountSource *mount_source;
  const char *dbus_id, *obj_path;

  data->result = g_object_ref (result);

  mount_source = g_mount_operation_dbus_wrap (data->mount_operation, _g_daemon_vfs_get_async_bus ());

  dbus_id = g_mount_source_get_dbus_id (mount_source);
  obj_path = g_mount_source_get_obj_path (mount_source);
  
  gvfs_dbus_mount_call_start_mountable (proxy,
                                        path,
                                        dbus_id,
                                        obj_path,
                                        cancellable,
                                        (GAsyncReadyCallback) start_mountable_async_cb,
                                        data);
  data->cancelled_tag = _g_dbus_async_subscribe_cancellable (connection, cancellable);
  
  g_object_unref (mount_source);
}

static void
g_daemon_file_start_mountable (GFile               *file,
			       GDriveStartFlags     flags,
			       GMountOperation     *mount_operation,
			       GCancellable        *cancellable,
			       GAsyncReadyCallback  callback,
			       gpointer             user_data)
{
  AsyncMountOp *data;
  
  data = g_new0 (AsyncMountOp, 1);
  data->flags = flags;
  if (mount_operation)
    data->mount_operation = g_object_ref (mount_operation);
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);

  create_proxy_for_file_async (file,
                               cancellable,
                               callback, user_data,
                               start_mountable_got_proxy_cb,
                               data, (GDestroyNotify) free_async_mount_op);
}

static gboolean
g_daemon_file_start_mountable_finish (GFile               *file,
				      GAsyncResult        *result,
				      GError             **error)
{
  return TRUE;
}

static void
stop_mountable_async_cb (GVfsDBusMount *proxy,
                          GAsyncResult *res,
                          gpointer user_data)
{
  AsyncMountOp *data = user_data;
  GSimpleAsyncResult *orig_result;
  GError *error = NULL;

  orig_result = data->result;

  if (! gvfs_dbus_mount_call_stop_mountable_finish (proxy, res, &error))
    _g_simple_async_result_take_error_stripped (orig_result, error);

  _g_simple_async_result_complete_with_cancellable (orig_result, data->cancellable);
  _g_dbus_async_unsubscribe_cancellable (data->cancellable, data->cancelled_tag);
  data->result = NULL;
  g_object_unref (orig_result);   /* trigger async_proxy_create_free() */
}

static void
stop_mountable_got_proxy_cb (GVfsDBusMount *proxy,
                             GDBusConnection *connection,
                             GMountInfo *mount_info,
                             const gchar *path,
                             GSimpleAsyncResult *result,
                             GError *error,
                             GCancellable *cancellable,
                             gpointer callback_data)
{
  AsyncMountOp *data = callback_data;
  GMountSource *mount_source;
  const char *dbus_id, *obj_path;

  data->result = g_object_ref (result);

  mount_source = g_mount_operation_dbus_wrap (data->mount_operation, _g_daemon_vfs_get_async_bus ());

  dbus_id = g_mount_source_get_dbus_id (mount_source);
  obj_path = g_mount_source_get_obj_path (mount_source);
 
  gvfs_dbus_mount_call_stop_mountable (proxy,
                                       path,
                                       data->flags,
                                       dbus_id,
                                       obj_path,
                                       cancellable,
                                       (GAsyncReadyCallback) stop_mountable_async_cb,
                                       data);
  data->cancelled_tag = _g_dbus_async_subscribe_cancellable (connection, cancellable);

  g_object_unref (mount_source);
}

static void
g_daemon_file_stop_mountable (GFile               *file,
                              GMountUnmountFlags   flags,
                              GMountOperation     *mount_operation,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  AsyncMountOp *data;
  
  data = g_new0 (AsyncMountOp, 1);
  data->flags = flags;
  if (mount_operation)
    data->mount_operation = g_object_ref (mount_operation);
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);

  create_proxy_for_file_async (file,
                               cancellable,
                               callback, user_data,
                               stop_mountable_got_proxy_cb,
                               data, (GDestroyNotify) free_async_mount_op);
}

static gboolean
g_daemon_file_stop_mountable_finish (GFile               *file,
                                     GAsyncResult        *result,
                                     GError             **error)
{
  return TRUE;
}

static void
eject_mountable_async_cb (GVfsDBusMount *proxy,
                          GAsyncResult *res,
                          gpointer user_data)
{
  AsyncMountOp *data = user_data;
  GSimpleAsyncResult *orig_result;
  GError *error = NULL;

  orig_result = data->result;

  if (! gvfs_dbus_mount_call_eject_mountable_finish (proxy, res, &error))
    _g_simple_async_result_take_error_stripped (orig_result, error);

  _g_simple_async_result_complete_with_cancellable (orig_result, data->cancellable);
  _g_dbus_async_unsubscribe_cancellable (data->cancellable, data->cancelled_tag);
  data->result = NULL;
  g_object_unref (orig_result);   /* trigger async_proxy_create_free() */
}

static void
eject_mountable_got_proxy_cb (GVfsDBusMount *proxy,
                              GDBusConnection *connection,
                              GMountInfo *mount_info,
                              const gchar *path,
                              GSimpleAsyncResult *result,
                              GError *error,
                              GCancellable *cancellable,
                              gpointer callback_data)
{
  AsyncMountOp *data = callback_data;
  GMountSource *mount_source;
  const char *dbus_id, *obj_path;

  data->result = g_object_ref (result);

  mount_source = g_mount_operation_dbus_wrap (data->mount_operation, _g_daemon_vfs_get_async_bus ());

  dbus_id = g_mount_source_get_dbus_id (mount_source);
  obj_path = g_mount_source_get_obj_path (mount_source);
 
  gvfs_dbus_mount_call_eject_mountable (proxy,
                                        path,
                                        data->flags,
                                        dbus_id,
                                        obj_path,
                                        cancellable,
                                        (GAsyncReadyCallback) eject_mountable_async_cb,
                                        data);
  data->cancelled_tag = _g_dbus_async_subscribe_cancellable (connection, cancellable);

  g_object_unref (mount_source);
}

static void
g_daemon_file_eject_mountable_with_operation (GFile               *file,
                                              GMountUnmountFlags   flags,
                                              GMountOperation     *mount_operation,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data)
{
  AsyncMountOp *data;
  
  data = g_new0 (AsyncMountOp, 1);
  data->flags = flags;
  if (mount_operation)
    data->mount_operation = g_object_ref (mount_operation);
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);

  create_proxy_for_file_async (file,
                               cancellable,
                               callback, user_data,
                               eject_mountable_got_proxy_cb,
                               data, (GDestroyNotify) free_async_mount_op);
}

static gboolean
g_daemon_file_eject_mountable_with_operation_finish (GFile               *file,
                                                     GAsyncResult        *result,
                                                     GError             **error)
{
  return TRUE;
}

static void
g_daemon_file_eject_mountable (GFile               *file,
			       GMountUnmountFlags   flags,
			       GCancellable        *cancellable,
			       GAsyncReadyCallback  callback,
			       gpointer             user_data)
{
  g_daemon_file_eject_mountable_with_operation (file, flags, NULL, cancellable, callback, user_data);
}

static gboolean
g_daemon_file_eject_mountable_finish (GFile               *file,
				      GAsyncResult        *result,
				      GError             **error)
{
  return g_daemon_file_eject_mountable_with_operation_finish (file, result, error);
}

static void
unmount_mountable_async_cb (GVfsDBusMount *proxy,
                            GAsyncResult *res,
                            gpointer user_data)
{
  AsyncMountOp *data = user_data;
  GSimpleAsyncResult *orig_result;
  GError *error = NULL;

  orig_result = data->result;

  if (! gvfs_dbus_mount_call_unmount_mountable_finish (proxy, res, &error))
    _g_simple_async_result_take_error_stripped (orig_result, error);

  _g_simple_async_result_complete_with_cancellable (orig_result, data->cancellable);
  _g_dbus_async_unsubscribe_cancellable (data->cancellable, data->cancelled_tag);
  data->result = NULL;
  g_object_unref (orig_result);   /* trigger async_proxy_create_free() */
}

static void
unmount_mountable_got_proxy_cb (GVfsDBusMount *proxy,
                                GDBusConnection *connection,
                                GMountInfo *mount_info,
                                const gchar *path,
                                GSimpleAsyncResult *result,
                                GError *error,
                                GCancellable *cancellable,
                                gpointer callback_data)
{
  AsyncMountOp *data = callback_data;
  GMountSource *mount_source;
  const char *dbus_id, *obj_path;

  data->result = g_object_ref (result);

  mount_source = g_mount_operation_dbus_wrap (data->mount_operation, _g_daemon_vfs_get_async_bus ());

  dbus_id = g_mount_source_get_dbus_id (mount_source);
  obj_path = g_mount_source_get_obj_path (mount_source);
 
  gvfs_dbus_mount_call_unmount_mountable (proxy,
                                          path,
                                          data->flags,
                                          dbus_id,
                                          obj_path,
                                          cancellable,
                                          (GAsyncReadyCallback) unmount_mountable_async_cb,
                                          data);
  data->cancelled_tag = _g_dbus_async_subscribe_cancellable (connection, cancellable);

  g_object_unref (mount_source);
}

static void
g_daemon_file_unmount_mountable_with_operation (GFile               *file,
                                                GMountUnmountFlags   flags,
                                                GMountOperation     *mount_operation,
                                                GCancellable        *cancellable,
                                                GAsyncReadyCallback  callback,
                                                gpointer             user_data)
{
  AsyncMountOp *data;
  
  data = g_new0 (AsyncMountOp, 1);
  data->flags = flags;
  if (mount_operation)
    data->mount_operation = g_object_ref (mount_operation);
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);

  create_proxy_for_file_async (file,
                               cancellable,
                               callback, user_data,
                               unmount_mountable_got_proxy_cb,
                               data, (GDestroyNotify) free_async_mount_op);
}

static gboolean
g_daemon_file_unmount_mountable_with_operation_finish (GFile               *file,
                                                       GAsyncResult        *result,
                                                       GError             **error)
{
  return TRUE;
}

static void
poll_mountable_async_cb (GVfsDBusMount *proxy,
                         GAsyncResult *res,
                         gpointer user_data)
{
  AsyncMountOp *data = user_data;
  GSimpleAsyncResult *orig_result;
  GError *error = NULL;
  
  orig_result = data->result;
  
  if (! gvfs_dbus_mount_call_poll_mountable_finish (proxy, res, &error))
    _g_simple_async_result_take_error_stripped (orig_result, error);

  _g_simple_async_result_complete_with_cancellable (orig_result, data->cancellable);
  _g_dbus_async_unsubscribe_cancellable (data->cancellable, data->cancelled_tag);
  data->result = NULL;
  g_object_unref (orig_result);   /* trigger async_proxy_create_free() */
}

static void
poll_mountable_got_proxy_cb (GVfsDBusMount *proxy,
                             GDBusConnection *connection,
                             GMountInfo *mount_info,
                             const gchar *path,
                             GSimpleAsyncResult *result,
                             GError *error,
                             GCancellable *cancellable,
                             gpointer callback_data)
{
  AsyncMountOp *data = callback_data;

  data->result = g_object_ref (result);
  
  gvfs_dbus_mount_call_poll_mountable (proxy,
                                       path,
                                       cancellable,
                                       (GAsyncReadyCallback) poll_mountable_async_cb,
                                       data);
  data->cancelled_tag = _g_dbus_async_subscribe_cancellable (connection, cancellable);
}

static void
g_daemon_file_poll_mountable (GFile               *file,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  AsyncMountOp *data;
  
  data = g_new0 (AsyncMountOp, 1);
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);

  create_proxy_for_file_async (file,
                               cancellable,
                               callback, user_data,
                               poll_mountable_got_proxy_cb,
                               data, (GDestroyNotify) free_async_mount_op);
}

static gboolean
g_daemon_file_poll_mountable_finish (GFile               *file,
                                     GAsyncResult        *result,
                                     GError             **error)
{
  return TRUE;
}

static void
g_daemon_file_unmount_mountable (GFile               *file,
				 GMountUnmountFlags   flags,
				 GCancellable        *cancellable,
				 GAsyncReadyCallback  callback,
				 gpointer             user_data)
{
  g_daemon_file_unmount_mountable_with_operation (file, flags, NULL, cancellable, callback, user_data);
}

static gboolean
g_daemon_file_unmount_mountable_finish (GFile               *file,
					GAsyncResult        *result,
					GError             **error)
{
  return g_daemon_file_unmount_mountable_with_operation_finish (file, result, error);
}

typedef struct {
  GFile *file;
  GMountOperation *mount_operation;
  GAsyncReadyCallback callback;
  GCancellable *cancellable;
  gpointer user_data;
} MountData;

static void g_daemon_file_mount_enclosing_volume (GFile *location,
						  GMountMountFlags flags,
						  GMountOperation *mount_operation,
						  GCancellable *cancellable,
						  GAsyncReadyCallback callback,
						  gpointer user_data);

static void
free_mount_data (MountData *data)
{
  g_object_unref (data->file);
  g_clear_object (&data->cancellable);
  g_clear_object (&data->mount_operation);
  g_free (data);
}

static void
mount_reply (GVfsDBusMountTracker *proxy,
             GAsyncResult *res,
             gpointer user_data)
{
  MountData *data = user_data;
  GSimpleAsyncResult *ares;
  GError *error = NULL;

  if (!gvfs_dbus_mount_tracker_call_mount_location_finish (proxy, res, &error))
    {
      g_dbus_error_strip_remote_error (error);
      ares = g_simple_async_result_new_take_error (G_OBJECT (data->file),
						   data->callback,
						   data->user_data,
						   error);
    }
  else
    {
      ares = g_simple_async_result_new (G_OBJECT (data->file),
				       data->callback,
				       data->user_data,
				       g_daemon_file_mount_enclosing_volume);
    }

  _g_simple_async_result_complete_with_cancellable (ares, data->cancellable);
  g_object_unref (ares);

  free_mount_data (data);
}

static void
mount_enclosing_volume_proxy_cb (GObject *source_object,
                                 GAsyncResult *res,
                                 gpointer user_data)
{
  MountData *data = user_data;
  GVfsDBusMountTracker *proxy;
  GError *error = NULL;
  GSimpleAsyncResult *ares;
  GDaemonFile *daemon_file;
  GMountSpec *spec;
  GMountSource *mount_source;

  daemon_file = G_DAEMON_FILE (data->file);

  proxy = gvfs_dbus_mount_tracker_proxy_new_for_bus_finish (res, &error);
  if (proxy == NULL)
    {
      g_dbus_error_strip_remote_error (error);
      ares = g_simple_async_result_new_take_error (G_OBJECT (data->file),
                                                   data->callback,
                                                   data->user_data,
                                                   error);
      _g_simple_async_result_complete_with_cancellable (ares, data->cancellable);
      g_object_unref (ares);
      free_mount_data (data);
      return;
    }
  
  spec = g_mount_spec_copy (daemon_file->mount_spec);
  g_mount_spec_set_mount_prefix (spec, daemon_file->path);
  mount_source = g_mount_operation_dbus_wrap (data->mount_operation, _g_daemon_vfs_get_async_bus ());

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), G_VFS_DBUS_MOUNT_TIMEOUT_MSECS);

  gvfs_dbus_mount_tracker_call_mount_location (proxy,
                                               g_mount_spec_to_dbus (spec),
                                               g_mount_source_to_dbus (mount_source),
                                               data->cancellable,
                                               (GAsyncReadyCallback) mount_reply,
                                               data);

  g_mount_spec_unref (spec);
  g_object_unref (mount_source);
  g_object_unref (proxy);
}

static void
g_daemon_file_mount_enclosing_volume (GFile *location,
				      GMountMountFlags  flags,
				      GMountOperation *mount_operation,
				      GCancellable *cancellable,
				      GAsyncReadyCallback callback,
				      gpointer user_data)
{
  MountData *data;
  
  data = g_new0 (MountData, 1);
  data->callback = callback;
  if (data->cancellable)
    data->cancellable = g_object_ref (data->cancellable);
  data->user_data = user_data;
  data->file = g_object_ref (location);
  if (mount_operation)
    data->mount_operation = g_object_ref (mount_operation);

  gvfs_dbus_mount_tracker_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                             G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS | G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                             G_VFS_DBUS_DAEMON_NAME,
                                             G_VFS_DBUS_MOUNTTRACKER_PATH,
                                             NULL,
                                             mount_enclosing_volume_proxy_cb,
                                             data);
}

static gboolean
g_daemon_file_mount_enclosing_volume_finish (GFile                  *location,
					     GAsyncResult           *result,
					     GError                **error)
{
  /* Errors handled in generic code */
  return TRUE;
}

static GFileInfo *
g_daemon_file_query_filesystem_info (GFile                *file,
				     const char           *attributes,
				     GCancellable         *cancellable,
				     GError              **error)
{
  GFileInfo *info;
  GVfsDBusMount *proxy;
  char *path;
  gboolean res;
  GVariant *iter_info;
  GError *local_error = NULL;

  proxy = create_proxy_for_file (file, NULL, &path, NULL, cancellable, error);
  if (proxy == NULL)
    return NULL;

  iter_info = NULL;
  res = gvfs_dbus_mount_call_query_filesystem_info_sync (proxy,
                                                         path,
                                                         attributes ? attributes : "",
                                                         &iter_info,
                                                         cancellable,
                                                         &local_error);

  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);
    }

  g_free (path);
  g_object_unref (proxy);

  if (! res)
    return NULL;

  info = _g_dbus_get_file_info (iter_info, error);
  g_variant_unref (iter_info);

  return info;
}


typedef struct {
  GFile *file;
  char *attributes;
  GFileQueryInfoFlags flags;
  int io_priority;
  GSimpleAsyncResult *result;
  GCancellable *cancellable;
  gulong cancelled_tag;
} AsyncCallQueryFsInfo;

static void
async_call_query_fs_info_free (AsyncCallQueryFsInfo *data)
{
  g_clear_object (&data->file);
  g_clear_object (&data->result);
  g_clear_object (&data->cancellable);
  g_free (data->attributes);
  g_free (data);
}

static void
query_fs_info_async_cb (GVfsDBusMount *proxy,
                        GAsyncResult *res,
                        gpointer user_data)
{
  AsyncCallQueryFsInfo *data = user_data;
  GFileInfo *info;
  GError *error = NULL;
  GSimpleAsyncResult *orig_result;
  GVariant *iter_info;

  orig_result = data->result;
  
  if (! gvfs_dbus_mount_call_query_filesystem_info_finish (proxy, &iter_info, res, &error))
    {
      _g_simple_async_result_take_error_stripped (orig_result, error);
      goto out;
    }

  info = _g_dbus_get_file_info (iter_info, &error);
  g_variant_unref (iter_info);

  if (info == NULL)
    {
      _g_simple_async_result_take_error_stripped (orig_result, error);
      goto out;
    }

  g_simple_async_result_set_op_res_gpointer (orig_result, info, g_object_unref);
  
out:
  _g_simple_async_result_complete_with_cancellable (orig_result, data->cancellable);
  _g_dbus_async_unsubscribe_cancellable (data->cancellable, data->cancelled_tag);
  data->result = NULL;
  g_object_unref (orig_result);   /* trigger async_proxy_create_free() */
}

static void
query_info_fs_async_get_proxy_cb (GVfsDBusMount *proxy,
                                  GDBusConnection *connection,
                                  GMountInfo *mount_info,
                                  const gchar *path,
                                  GSimpleAsyncResult *result,
                                  GError *error,
                                  GCancellable *cancellable,
                                  gpointer callback_data)
{
  AsyncCallQueryFsInfo *data = callback_data;
  char *uri;

  uri = g_file_get_uri (data->file);
  
  data->result = g_object_ref (result);
  
  gvfs_dbus_mount_call_query_filesystem_info (proxy,
                                              path,
                                              data->attributes ? data->attributes : "",
                                              cancellable,
                                              (GAsyncReadyCallback) query_fs_info_async_cb,
                                              data);
  data->cancelled_tag = _g_dbus_async_subscribe_cancellable (connection, cancellable);
  
  g_free (uri);
}

static void
g_daemon_file_query_filesystem_info_async (GFile                      *file,
					   const char                 *attributes,
					   int                         io_priority,
					   GCancellable               *cancellable,
					   GAsyncReadyCallback         callback,
					   gpointer                    user_data)
{
  AsyncCallQueryFsInfo *data;

  data = g_new0 (AsyncCallQueryFsInfo, 1);
  data->file = g_object_ref (file);
  data->attributes = g_strdup (attributes);
  data->io_priority = io_priority;
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);

  create_proxy_for_file_async (file,
                               cancellable,
                               callback, user_data,
                               query_info_fs_async_get_proxy_cb,
                               data, (GDestroyNotify) async_call_query_fs_info_free);
}

static GFileInfo *
g_daemon_file_query_filesystem_info_finish (GFile                      *file,
					    GAsyncResult               *res,
					    GError                    **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GFileInfo *info;

  info = g_simple_async_result_get_op_res_gpointer (simple);
  if (info)
    return g_object_ref (info);
  
  return NULL;
}

static GMount *
g_daemon_file_find_enclosing_mount (GFile *file,
                                    GCancellable *cancellable,
                                    GError **error)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  GMountInfo *mount_info;
  GDaemonMount *mount;
  
  mount_info = _g_daemon_vfs_get_mount_info_sync (daemon_file->mount_spec,
						  daemon_file->path,
						  cancellable,
						  error);
  if (mount_info == NULL)
    goto out;

  if (mount_info->user_visible)
    {
      /* if we have a daemon volume monitor then return one of it's mounts */
      mount = g_daemon_volume_monitor_find_mount_by_mount_info (mount_info);
      if (mount == NULL)
        {
          mount = g_daemon_mount_new (mount_info, NULL);
        }
      g_mount_info_unref (mount_info);
      
      if (mount)
	return G_MOUNT (mount);
    }

  g_set_error_literal (error, G_IO_ERROR,
		       G_IO_ERROR_NOT_FOUND,
  /* translators: this is an error message when there is no user visible "mount" object
     corresponding to a particular path/uri */
		       _("Could not find enclosing mount"));
 
out:
  if (error && *error)
    g_dbus_error_strip_remote_error (*error);

  return NULL;
}

static GFile *
g_daemon_file_get_child_for_display_name (GFile        *file,
					  const char   *display_name,
					  GError      **error)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  GMountInfo *mount_info;
  char *basename;
  GFile *child;

  mount_info = _g_daemon_vfs_get_mount_info_sync (daemon_file->mount_spec,
						 daemon_file->path,
                                                 NULL,  /* TODO: cancellable */
						 NULL); /* TODO: error? */


  if (mount_info && mount_info->prefered_filename_encoding)
    {
      basename = g_convert (display_name, -1,
			    mount_info->prefered_filename_encoding,
			    "UTF-8",
			    NULL, NULL,
			    NULL);
      if (basename == NULL)
	{
	  g_set_error (error, G_IO_ERROR,
		       G_IO_ERROR_INVALID_FILENAME,
		       _("Invalid filename %s"), display_name);
	  return NULL;
	}
      
      child = g_file_get_child (file, basename);
      g_free (basename);
    }
  else
    child = g_file_get_child (file, display_name);
  
  return child;
}

static GFile *
g_daemon_file_set_display_name (GFile *file,
				const char *display_name,
				GCancellable *cancellable,
				GError **error)
{
  GDaemonFile *daemon_file;
  GMountInfo  *mount_info;
  char *new_path;
  GVfsDBusMount *proxy;
  char *path;
  gboolean res;
  GError *local_error = NULL;

  daemon_file = G_DAEMON_FILE (file);
  mount_info = NULL;

  proxy = create_proxy_for_file (file, &mount_info, &path, NULL, cancellable, error);
  if (proxy == NULL)
    return NULL;

  res = gvfs_dbus_mount_call_set_display_name_sync (proxy,
                                                    path,
                                                    display_name ? display_name : "",
                                                    &new_path,
                                                    cancellable,
                                                    &local_error);

  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);

      file = NULL;
    }

  g_free (path);
  g_object_unref (proxy);

  if (! res)
    goto out;
  
  g_mount_info_apply_prefix (mount_info, &new_path);
  file = new_file_for_new_path (daemon_file, new_path);
  g_free (new_path);

 out:
  g_mount_info_unref (mount_info);
  return file;
}

static gboolean
g_daemon_file_delete (GFile *file,
		      GCancellable *cancellable,
		      GError **error)
{
  GVfsDBusMount *proxy;
  char *path;
  gboolean res;
  GError *local_error = NULL;

  proxy = create_proxy_for_file (file, NULL, &path, NULL, cancellable, error);
  if (proxy == NULL)
    return FALSE;

  res = gvfs_dbus_mount_call_delete_sync (proxy,
                                          path,
                                          cancellable,
                                          &local_error);

  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);
    }

  g_free (path);
  g_object_unref (proxy);
  
  return res;
}

static gboolean
g_daemon_file_trash (GFile *file,
		     GCancellable *cancellable,
		     GError **error)
{
  GVfsDBusMount *proxy;
  char *path;
  gboolean res;
  GError *local_error = NULL;

  proxy = create_proxy_for_file (file, NULL, &path, NULL, cancellable, error);
  if (proxy == NULL)
    return FALSE;

  res = gvfs_dbus_mount_call_trash_sync (proxy,
                                         path,
                                         cancellable,
                                         &local_error);

  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);
    }

  g_free (path);
  g_object_unref (proxy);
  
  return res;
}

static gboolean
g_daemon_file_make_directory (GFile *file,
			      GCancellable *cancellable,
			      GError **error)
{
  GVfsDBusMount *proxy;
  char *path;
  gboolean res;
  GError *local_error = NULL;

  proxy = create_proxy_for_file (file, NULL, &path, NULL, cancellable, error);
  if (proxy == NULL)
    return FALSE;

  res = gvfs_dbus_mount_call_make_directory_sync (proxy,
                                                  path,
                                                  cancellable,
                                                  &local_error);

  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);
    }

  g_free (path);
  g_object_unref (proxy);
  
  return res;
}

static gboolean
g_daemon_file_make_symbolic_link (GFile *file,
				  const char *symlink_value,
				  GCancellable *cancellable,
				  GError **error)
{
  GVfsDBusMount *proxy;
  char *path;
  gboolean res;
  GError *local_error = NULL;

  proxy = create_proxy_for_file (file, NULL, &path, NULL, cancellable, error);
  if (proxy == NULL)
    return FALSE;

  res = gvfs_dbus_mount_call_make_symbolic_link_sync (proxy,
                                                      path,
                                                      symlink_value ? symlink_value : "",
                                                      cancellable,
                                                      &local_error);

  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);
    }

  g_free (path);
  g_object_unref (proxy);
  
  return res;
}

static GFileAttributeInfoList *
g_daemon_file_query_settable_attributes (GFile                      *file,
					 GCancellable               *cancellable,
					 GError                    **error)
{
  GVfsDBusMount *proxy;
  char *path;
  gboolean res;
  GVariant *iter_list;
  GFileAttributeInfoList *list;
  GError *local_error = NULL;

  proxy = create_proxy_for_file (file, NULL, &path, NULL, cancellable, error);
  if (proxy == NULL)
    return FALSE;

  iter_list = NULL;
  res = gvfs_dbus_mount_call_query_settable_attributes_sync (proxy,
                                                             path,
                                                             &iter_list,
                                                             cancellable,
                                                             &local_error);

  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);
    }

  g_free (path);
  g_object_unref (proxy);

  if (!res)
    return NULL;

  list = _g_dbus_get_attribute_info_list (iter_list, error);
  g_variant_unref (iter_list);
  
  return list;
}

static GFileAttributeInfoList *
g_daemon_file_query_writable_namespaces (GFile                      *file,
					 GCancellable               *cancellable,
					 GError                    **error)
{
  GVfsDBusMount *proxy;
  char *path;
  gboolean res;
  GVariant *iter_list;
  GFileAttributeInfoList *list;
  GError *local_error = NULL;

  proxy = create_proxy_for_file (file, NULL, &path, NULL, cancellable, error);
  if (proxy == NULL)
    return FALSE;

  iter_list = NULL;
  res = gvfs_dbus_mount_call_query_writable_namespaces_sync (proxy,
                                                             path,
                                                             &iter_list,
                                                             cancellable,
                                                             &local_error);

  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);
    }

  g_free (path);
  g_object_unref (proxy);

  if (res)
    {
      list = _g_dbus_get_attribute_info_list (iter_list, error);
      g_variant_unref (iter_list);
    }
  else
    {
      list = g_file_attribute_info_list_new ();
    }

  g_file_attribute_info_list_add (list,
                                  "metadata",
                                  G_FILE_ATTRIBUTE_TYPE_STRING, /* Also STRINGV, but no way express this ... */
                                  G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                  G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
  
  return list;
}

static gboolean
set_metadata_attribute (GFile *file,
			const char *attribute,
			GFileAttributeType type,
			gpointer value,
			GCancellable *cancellable,
			GError **error)
{
  GDaemonFile *daemon_file;
  char *treename;
  const char *metatreefile;
  MetaTree *tree;
  int appended;
  gboolean res;
  GVfsMetadata *proxy;
  GVariantBuilder *builder;

  daemon_file = G_DAEMON_FILE (file);

  treename = g_mount_spec_to_string (daemon_file->mount_spec);
  tree = meta_tree_lookup_by_name (treename, FALSE);
  g_free (treename);
  
  if (!tree)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("Error setting file metadata: %s"),
                   _("can't open metadata tree"));
      return FALSE;
    }

  res = FALSE;
  proxy = _g_daemon_vfs_get_metadata_proxy (cancellable, error);

  if (proxy)
    {
      builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);

      metatreefile = meta_tree_get_filename (tree);

      appended = _g_daemon_vfs_append_metadata_for_set (builder,
                                                        tree,
                                                        daemon_file->path,
                                                        attribute,
                                                        type,
                                                        value);
    
      res = TRUE;
      if (appended == -1)
        {
          res = FALSE;
          g_set_error (error, G_IO_ERROR,
                       G_IO_ERROR_INVALID_ARGUMENT,
                       _("Error setting file metadata: %s"),
                       _("values must be string or list of strings"));
        }
      else if (appended > 0 &&
               !gvfs_metadata_call_set_sync (proxy,
                                             metatreefile,
                                             daemon_file->path,
                                             g_variant_builder_end (builder),
                                             cancellable,
                                             error))
        res = FALSE;

      g_variant_builder_unref (builder);
    }

  meta_tree_unref (tree);
  if (error && *error)
    g_dbus_error_strip_remote_error (*error);

  return res;
}

static gboolean
g_daemon_file_set_attribute (GFile *file,
			     const char *attribute,
			     GFileAttributeType    type,
			     gpointer              value_p,
			     GFileQueryInfoFlags flags,
			     GCancellable *cancellable,
			     GError **error)
{
  GVfsDBusMount *proxy;
  char *path;
  gboolean res;
  GError *my_error;

  if (g_str_has_prefix (attribute, "metadata::"))
    return set_metadata_attribute (file, attribute, type, value_p, cancellable, error);

 retry:
  proxy = create_proxy_for_file (file, NULL, &path, NULL, cancellable, error);
  if (proxy == NULL)
    return FALSE;

  my_error = NULL;
  res = gvfs_dbus_mount_call_set_attribute_sync (proxy,
                                                 path,
                                                 flags,
                                                 _g_dbus_append_file_attribute (attribute, 0, type, value_p),
                                                 cancellable,
                                                 &my_error);
  g_free (path);

  if (! res)
    {
      if (g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      else
      if (g_error_matches (my_error, G_VFS_ERROR, G_VFS_ERROR_RETRY))
	{
	  g_clear_error (&my_error);
	  g_object_unref (proxy);
	  goto retry;
	}
      _g_propagate_error_stripped (error, my_error);
      return FALSE;
    }

  g_object_unref (proxy);

  return TRUE;
}


typedef struct
{
  GAsyncResult *res;
  GMainContext *context;
  GMainLoop *loop;
  GFileProgressCallback progress_callback;
  gpointer progress_callback_data;
} FileTransferSyncData;

static gboolean
handle_progress (GVfsDBusProgress *object,
                 GDBusMethodInvocation *invocation,
                 guint64 arg_current,
                 guint64 arg_total,
                 FileTransferSyncData *data)
{
  data->progress_callback (arg_current, arg_total, data->progress_callback_data);
  
  gvfs_dbus_progress_complete_progress (object, invocation);
  
  return TRUE;
}

static void
copy_cb (GObject *source_object,
         GAsyncResult *res,
         gpointer user_data)
{
  FileTransferSyncData *data = user_data;

  data->res = g_object_ref (res);
  g_main_loop_quit (data->loop);
}

static gboolean
file_transfer (GFile                  *source,
               GFile                  *destination,
               GFileCopyFlags          flags,
               gboolean                remove_source,
               GCancellable           *cancellable,
               GFileProgressCallback   progress_callback,
               gpointer                progress_callback_data,
               GError                **error)
{
  char *obj_path;
  FileTransferSyncData data = {0, };
  char *local_path = NULL;
  gboolean source_is_daemon;
  gboolean dest_is_daemon;
  gboolean native_transfer;
  gboolean send_progress;
  GVfsDBusMount *proxy;
  gchar *path1, *path2;
  GDBusConnection *connection;
  gboolean res;
  GVfsDBusProgress *progress_skeleton;
  GFile *file1, *file2;
  GError *my_error;
  guint32 serial;

  res = FALSE;
  progress_skeleton = NULL;
  native_transfer  = FALSE;
  source_is_daemon = G_IS_DAEMON_FILE (source);
  dest_is_daemon   = G_IS_DAEMON_FILE (destination);
  send_progress    = progress_callback != NULL;
  serial           = 0;

  if (source_is_daemon && dest_is_daemon)
    native_transfer = TRUE;
  else if (dest_is_daemon && !source_is_daemon)
    local_path = g_file_get_path (source);
  else if (source_is_daemon && !dest_is_daemon)
    local_path = g_file_get_path (destination);
  else
    {
      /* Fall back to default copy/move */
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Operation not supported");
      return FALSE;
    }

  if (!native_transfer && local_path == NULL)
    {
      /* This will cause the fallback code to be involved */
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           _("Operation not supported, files on different mounts"));
      return FALSE;

    }

  if (send_progress)
    obj_path = g_strdup_printf ("/org/gtk/vfs/callback/%p", &obj_path);
  else
    obj_path = g_strdup ("/org/gtk/vfs/void");

  /* need to create proxy with daemon files only */ 
  if (native_transfer)
    {
      file1 = source;
      file2 = destination;
    }
  else
    {
      if (dest_is_daemon)
        {
          file1 = destination;
          file2 = NULL;
        }
      else
        {
          file1 = source;
          file2 = NULL;
        }
    }
  
retry:
  my_error = NULL;

  proxy = create_proxy_for_file2 (file1, file2,
                                  NULL, NULL,
                                  &path1, &path2,
                                  &connection,
                                  cancellable,
                                  &my_error);
  
  if (proxy == NULL)
    goto out;

  data.progress_callback = progress_callback;
  data.progress_callback_data = progress_callback_data;
  data.context = g_main_context_new ();
  data.loop = g_main_loop_new (data.context, FALSE);

  g_main_context_push_thread_default (data.context);

  if (send_progress)
    {
      /* Register progress interface skeleton */
      progress_skeleton = gvfs_dbus_progress_skeleton_new ();
      g_signal_connect (progress_skeleton, "handle-progress", G_CALLBACK (handle_progress), &data);

      if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (progress_skeleton),
                                             connection,
                                             obj_path,
                                             &my_error))
        goto out;
    }

  if (native_transfer == TRUE)
    {
      if (remove_source == FALSE)
        {
          gvfs_dbus_mount_call_copy (proxy,
                                     path1, path2,
                                     flags,
                                     obj_path,
                                     cancellable,
                                     copy_cb,
                                     &data);
          serial = g_dbus_connection_get_last_serial (connection);
          g_main_loop_run (data.loop);
          res = gvfs_dbus_mount_call_copy_finish (proxy, data.res, &my_error);
        }
      else
        {
          gvfs_dbus_mount_call_move (proxy,
                                     path1, path2,
                                     flags,
                                     obj_path,
                                     cancellable,
                                     copy_cb,
                                     &data);
          serial = g_dbus_connection_get_last_serial (connection);
          g_main_loop_run (data.loop);
          res = gvfs_dbus_mount_call_move_finish (proxy, data.res, &my_error);
        }
    }
  else if (dest_is_daemon == TRUE)
    {
      gvfs_dbus_mount_call_push (proxy,
                                 path1,
                                 local_path,
                                 send_progress,
                                 flags,
                                 obj_path,
                                 remove_source,
                                 cancellable,
                                 copy_cb,
                                 &data);
      serial = g_dbus_connection_get_last_serial (connection);
      g_main_loop_run (data.loop);
      res = gvfs_dbus_mount_call_push_finish (proxy, data.res, &my_error);
    }
  else
    {
      gvfs_dbus_mount_call_pull (proxy,
                                 path1,
                                 local_path,
                                 send_progress,
                                 flags,
                                 obj_path,
                                 remove_source,
                                 cancellable,
                                 copy_cb,
                                 &data);
      serial = g_dbus_connection_get_last_serial (connection);
      g_main_loop_run (data.loop);
      res = gvfs_dbus_mount_call_pull_finish (proxy, data.res, &my_error);
    }

  g_object_unref (data.res);

 out:
  if (progress_skeleton)
    {
      g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (progress_skeleton));
      g_object_unref (progress_skeleton);
    }
  if (data.context)
    {
      g_main_context_pop_thread_default (data.context);
      g_main_context_unref (data.context);
      g_main_loop_unref (data.loop);
    }

  if (! res)
    {
      if (serial != 0 &&
          g_error_matches (my_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          _g_dbus_send_cancelled_with_serial_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)),
                                                   serial);
        }
      else
      if (g_error_matches (my_error, G_VFS_ERROR, G_VFS_ERROR_RETRY))
        {
          g_clear_error (&my_error);
          g_clear_object (&proxy);
          goto retry;
        }
      _g_propagate_error_stripped (error, my_error);
    }

  g_clear_object (&proxy);

  g_free (local_path);
  g_free (obj_path);

  return res;
}

static gboolean
g_daemon_file_copy (GFile                  *source,
		    GFile                  *destination,
		    GFileCopyFlags          flags,
		    GCancellable           *cancellable,
		    GFileProgressCallback   progress_callback,
		    gpointer                progress_callback_data,
		    GError                **error)
{
  gboolean result;

  result = file_transfer (source,
                          destination,
                          flags,
                          FALSE,
                          cancellable,
                          progress_callback,
                          progress_callback_data,
                          error);

  return result;
}

static gboolean
g_daemon_file_move (GFile                  *source,
		    GFile                  *destination,
		    GFileCopyFlags          flags,
		    GCancellable           *cancellable,
		    GFileProgressCallback   progress_callback,
		    gpointer                progress_callback_data,
		    GError                **error)
{
  gboolean result;

  result = file_transfer (source,
                          destination,
                          flags,
                          TRUE,
                          cancellable,
                          progress_callback,
                          progress_callback_data,
                          error);

  return result;
}

static GFileMonitor*
g_daemon_file_monitor_dir (GFile* file,
			   GFileMonitorFlags flags,
			   GCancellable *cancellable,
			   GError **error)
{
  GFileMonitor *monitor;
  GMountInfo *mount_info;
  GVfsDBusMount *proxy;
  char *path;
  char *obj_path;
  gboolean res;
  GError *local_error = NULL;

  monitor = NULL;
  mount_info = NULL;
  obj_path = NULL;

  proxy = create_proxy_for_file (file, &mount_info, &path, NULL, cancellable, error);
  if (proxy == NULL)
    return FALSE;

  
  res = gvfs_dbus_mount_call_create_directory_monitor_sync (proxy,
                                                            path,
                                                            flags,
                                                            &obj_path,
                                                            cancellable,
                                                            &local_error);

  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);
    }

  g_free (path);
  g_object_unref (proxy);

  if (! res)
    goto out;
  
  monitor = g_daemon_file_monitor_new (mount_info->dbus_id,
				       obj_path);
  
 out:
  g_mount_info_unref (mount_info);
  g_free (obj_path);

  return monitor;
}

static GFileMonitor*
g_daemon_file_monitor_file (GFile* file,
			    GFileMonitorFlags flags,
			    GCancellable *cancellable,
			    GError **error)
{
  GFileMonitor *monitor;
  GMountInfo *mount_info;
  GVfsDBusMount *proxy;
  char *path;
  char *obj_path;
  gboolean res;
  GError *local_error = NULL;

  monitor = NULL;
  mount_info = NULL;
  obj_path = NULL;

  proxy = create_proxy_for_file (file, &mount_info, &path, NULL, cancellable, error);
  if (proxy == NULL)
    return FALSE;

  
  res = gvfs_dbus_mount_call_create_file_monitor_sync (proxy,
                                                       path,
                                                       flags,
                                                       &obj_path,
                                                       cancellable,
                                                       &local_error);

  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);
    }

  g_free (path);
  g_object_unref (proxy);

  if (! res)
    goto out;
  
  monitor = g_daemon_file_monitor_new (mount_info->dbus_id,
                                       obj_path);
  
 out:
  g_mount_info_unref (mount_info);
  g_free (obj_path);

  return monitor;
}


static void
file_open_write_async_cb (GVfsDBusMount *proxy,
                          GAsyncResult *res,
                          gpointer user_data)
{
  AsyncCallFileReadWrite *data = user_data;
  GError *error = NULL;
  GSimpleAsyncResult *orig_result;
  guint32 flags;
  GUnixFDList *fd_list;
  int fd;
  GVariant *fd_id_val;
  guint fd_id;
  guint64 initial_offset;
  GFileOutputStream *output_stream;

  orig_result = data->result;
  
  if (! gvfs_dbus_mount_call_open_for_write_flags_finish (proxy,
                                                          &fd_id_val,
                                                          &flags,
                                                          &initial_offset,
                                                          &fd_list,
                                                          res,
                                                          &error))
    {
      _g_simple_async_result_take_error_stripped (orig_result, error);
      goto out;
    }

  fd_id = g_variant_get_handle (fd_id_val);
  g_variant_unref (fd_id_val);

  if (fd_list == NULL || g_unix_fd_list_get_length (fd_list) != 1 ||
      (fd = g_unix_fd_list_get (fd_list, fd_id, NULL)) == -1)
    {
      g_simple_async_result_set_error (orig_result,
                                       G_IO_ERROR, G_IO_ERROR_FAILED,
                                       _("Couldn't get stream file descriptor"));
    }
  else
    {
      output_stream = g_daemon_file_output_stream_new (fd, flags, initial_offset);
      g_simple_async_result_set_op_res_gpointer (orig_result, output_stream, g_object_unref);
      g_object_unref (fd_list);
    }

out:
  _g_simple_async_result_complete_with_cancellable (orig_result, data->cancellable);
  _g_dbus_async_unsubscribe_cancellable (data->cancellable, data->cancelled_tag);
  data->result = NULL;
  g_object_unref (orig_result);   /* trigger async_proxy_create_free() */
}

static void
file_open_write_async_get_proxy_cb (GVfsDBusMount *proxy,
                                    GDBusConnection *connection,
                                    GMountInfo *mount_info,
                                    const gchar *path,
                                    GSimpleAsyncResult *result,
                                    GError *error,
                                    GCancellable *cancellable,
                                    gpointer callback_data)
{
  AsyncCallFileReadWrite *data = callback_data;
  guint32 pid;

  pid = get_pid_for_file (data->file);
  
  data->result = g_object_ref (result);
  
  gvfs_dbus_mount_call_open_for_write_flags (proxy,
                                             path,
                                             data->mode,
                                             data->etag,
                                             data->make_backup,
                                             data->flags,
                                             pid,
                                             NULL,
                                             cancellable,
                                             (GAsyncReadyCallback) file_open_write_async_cb,
                                             data);
  data->cancelled_tag = _g_dbus_async_subscribe_cancellable (connection, cancellable);
}

static void
file_open_write_async (GFile                      *file,
                       guint16                     mode,
                       const char                 *etag,
                       gboolean                    make_backup,
                       GFileCreateFlags            flags,
                       int                         io_priority,
                       GCancellable               *cancellable,
                       GAsyncReadyCallback         callback,
                       gpointer                    user_data)
{
  AsyncCallFileReadWrite *data;

  data = g_new0 (AsyncCallFileReadWrite, 1);
  data->file = g_object_ref (file);
  data->mode = mode;
  data->etag = g_strdup (etag ? etag : "");
  data->make_backup = make_backup;
  data->io_priority = io_priority;
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);

  create_proxy_for_file_async (file,
                               cancellable,
                               callback, user_data,
                               file_open_write_async_get_proxy_cb,
                               data, (GDestroyNotify) async_call_file_read_write_free);
}

static void
g_daemon_file_append_to_async (GFile                      *file,
                               GFileCreateFlags            flags,
                               int                         io_priority,
                               GCancellable               *cancellable,
                               GAsyncReadyCallback         callback,
                               gpointer                    user_data)
{
  file_open_write_async (file,
                         1, "", FALSE, flags, io_priority,
                         cancellable,
                         callback, user_data);
}

static GFileOutputStream *
g_daemon_file_append_to_finish (GFile                      *file,
                                GAsyncResult               *res,
                                GError                    **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GFileOutputStream *output_stream;

  output_stream = g_simple_async_result_get_op_res_gpointer (simple);
  if (output_stream)
    return g_object_ref (output_stream);

  return NULL;
}

static void
g_daemon_file_create_async (GFile                      *file,
                            GFileCreateFlags            flags,
                            int                         io_priority,
                            GCancellable               *cancellable,
                            GAsyncReadyCallback         callback,
                            gpointer                    user_data)
{
  file_open_write_async (file,
                         0, "", FALSE, flags, io_priority,
                         cancellable,
                         callback, user_data);
}

static GFileOutputStream *
g_daemon_file_create_finish (GFile                      *file,
                             GAsyncResult               *res,
                             GError                    **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GFileOutputStream *output_stream;

  output_stream = g_simple_async_result_get_op_res_gpointer (simple);
  if (output_stream)
    return g_object_ref (output_stream);

  return NULL;
}


typedef struct {
  GFile *file;
  char *attributes;
  GFileQueryInfoFlags flags;
  int io_priority;
  GSimpleAsyncResult *result;
  GCancellable *cancellable;
  GDaemonFileEnumerator *enumerator;
  gulong cancelled_tag;
} AsyncCallEnumerate;

static void
async_call_enumerate_free (AsyncCallEnumerate *data)
{
  g_clear_object (&data->file);
  g_clear_object (&data->result);
  g_clear_object (&data->cancellable);
  g_clear_object (&data->enumerator);
  g_free (data->attributes);
  g_free (data);
}

static void
enumerate_children_async_cb (GVfsDBusMount *proxy,
                             GAsyncResult *res,
                             gpointer user_data)
{
  AsyncCallEnumerate *data = user_data;
  GError *error = NULL;
  GSimpleAsyncResult *orig_result;

  orig_result = data->result;
  
  if (! gvfs_dbus_mount_call_enumerate_finish (proxy, res, &error))
    {
      _g_simple_async_result_take_error_stripped (orig_result, error);
      goto out;
    }
  
  g_object_ref (data->enumerator);

  g_simple_async_result_set_op_res_gpointer (orig_result, data->enumerator, g_object_unref);

out:
  _g_simple_async_result_complete_with_cancellable (orig_result, data->cancellable);
  _g_dbus_async_unsubscribe_cancellable (data->cancellable, data->cancelled_tag);
  data->result = NULL;
  g_object_unref (orig_result);   /* trigger async_proxy_create_free() */
}

static void
enumerate_children_async_get_proxy_cb (GVfsDBusMount *proxy,
                                       GDBusConnection *connection,
                                       GMountInfo *mount_info,
                                       const gchar *path,
                                       GSimpleAsyncResult *result,
                                       GError *error,
                                       GCancellable *cancellable,
                                       gpointer callback_data)
{
  AsyncCallEnumerate *data = callback_data;
  char *obj_path;
  char *uri;

  data->enumerator = g_daemon_file_enumerator_new (data->file, proxy, data->attributes, FALSE);

  obj_path = g_daemon_file_enumerator_get_object_path (data->enumerator);
  uri = g_file_get_uri (data->file);
  
  data->result = g_object_ref (result);
  
  gvfs_dbus_mount_call_enumerate (proxy,
                                  path,
                                  obj_path,
                                  data->attributes ? data->attributes : "",
                                  data->flags,
                                  uri,
                                  cancellable,
                                  (GAsyncReadyCallback) enumerate_children_async_cb,
                                  data);
  data->cancelled_tag = _g_dbus_async_subscribe_cancellable (connection, cancellable);
  
  g_free (uri);
  g_free (obj_path);
}

static void
g_daemon_file_enumerate_children_async (GFile                      *file,
                                        const char                 *attributes,
                                        GFileQueryInfoFlags         flags,
                                        int                         io_priority,
                                        GCancellable               *cancellable,
                                        GAsyncReadyCallback         callback,
                                        gpointer                    user_data)
{
  AsyncCallEnumerate *data;

  data = g_new0 (AsyncCallEnumerate, 1);
  data->file = g_object_ref (file);
  data->attributes = g_strdup (attributes);
  data->flags = flags;
  data->io_priority = io_priority;
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);

  create_proxy_for_file_async (file,
                               cancellable,
                               callback, user_data,
                               enumerate_children_async_get_proxy_cb,
                               data, (GDestroyNotify) async_call_enumerate_free);
}

static GFileEnumerator *
g_daemon_file_enumerate_children_finish (GFile                      *file,
                                         GAsyncResult               *res,
                                         GError                    **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GDaemonFileEnumerator *enumerator;

  enumerator = g_simple_async_result_get_op_res_gpointer (simple);
  if (enumerator)
    return g_object_ref (enumerator);

  return NULL;
}

typedef struct
{
  GFile              *file;
  GSimpleAsyncResult *result;
  GCancellable       *cancellable;
}
FindEnclosingMountData;

static void
find_enclosing_mount_cb (GMountInfo *mount_info,
                         gpointer user_data,
                         GError *error)
{
  FindEnclosingMountData *data = user_data;
  GError *my_error = NULL;

  if (data->cancellable && g_cancellable_set_error_if_cancelled (data->cancellable, &my_error))
    {
      _g_simple_async_result_take_error_stripped (data->result, my_error);
      goto out;
    }

  if (error)
    {
      g_dbus_error_strip_remote_error (error);
      g_simple_async_result_set_from_error (data->result, error);
      goto out;
    }

  if (!mount_info)
    {
      g_simple_async_result_set_error (data->result, G_IO_ERROR, G_IO_ERROR_FAILED,
                                       "Internal error: \"%s\"",
                                       "No error but no mount info from g_daemon_vfs_get_mount_info_async");
      goto out;
    }

  if (mount_info->user_visible)
    {
      GDaemonMount *mount;

      /* if we have a daemon volume monitor then return one of it's mounts */
      mount = g_daemon_volume_monitor_find_mount_by_mount_info (mount_info);
      if (mount == NULL)
        mount = g_daemon_mount_new (mount_info, NULL);
      
      if (mount)
        g_simple_async_result_set_op_res_gpointer (data->result, mount, g_object_unref);
      else
        g_simple_async_result_set_error (data->result, G_IO_ERROR, G_IO_ERROR_FAILED,
                                         "Internal error: \"%s\"",
                                         "Mount info did not yield a mount");
    }

out:
  _g_simple_async_result_complete_with_cancellable (data->result, data->cancellable);

  g_clear_object (&data->cancellable);
  g_object_unref (data->file);
  g_object_unref (data->result);
  g_free (data);
}

static void
g_daemon_file_find_enclosing_mount_async (GFile                *file,
                                          int                   io_priority,
                                          GCancellable         *cancellable,
                                          GAsyncReadyCallback   callback,
                                          gpointer              user_data)
{
  GDaemonFile            *daemon_file = G_DAEMON_FILE (file);
  FindEnclosingMountData *data;

  data = g_new0 (FindEnclosingMountData, 1);

  data->result = g_simple_async_result_new (G_OBJECT (file),
                                            callback, user_data,
                                            NULL);
  data->file = g_object_ref (file);

  if (cancellable)
    data->cancellable = g_object_ref (cancellable);

  _g_daemon_vfs_get_mount_info_async (daemon_file->mount_spec,
                                      daemon_file->path,
                                      find_enclosing_mount_cb,
                                      data);
}

static GMount *
g_daemon_file_find_enclosing_mount_finish (GFile              *file,
                                           GAsyncResult       *res,
                                           GError            **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GMount             *mount;

  mount = g_simple_async_result_get_op_res_gpointer (simple);
  if (mount)
    return g_object_ref (mount);

  return NULL;
}

static void
g_daemon_file_replace_async (GFile                      *file,
                             const char                 *etag,
                             gboolean                    make_backup,
                             GFileCreateFlags            flags,
                             int                         io_priority,
                             GCancellable               *cancellable,
                             GAsyncReadyCallback         callback,
                             gpointer                    user_data)
{
  file_open_write_async (file,
                         2, etag, make_backup, flags, io_priority,
                         cancellable,
                         callback, user_data);
}

static GFileOutputStream *
g_daemon_file_replace_finish (GFile                      *file,
                              GAsyncResult               *res,
                              GError                    **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GFileOutputStream *output_stream;

  output_stream = g_simple_async_result_get_op_res_gpointer (simple);
  if (output_stream)
    return g_object_ref (output_stream);

  return NULL;
}

typedef struct {
  GFile *file;
  char *display_name;
  int io_priority;
  GMountInfo *mount_info;
  GSimpleAsyncResult *result;
  GCancellable *cancellable;
  gulong cancelled_tag;
} AsyncCallSetDisplayName;

static void
async_call_set_display_name_free (AsyncCallSetDisplayName *data)
{
  g_clear_object (&data->file);
  g_clear_object (&data->result);
  g_clear_object (&data->cancellable);
  if (data->mount_info)
    g_mount_info_unref (data->mount_info);
  g_free (data->display_name);
  g_free (data);
}

static void
set_display_name_async_cb (GVfsDBusMount *proxy,
                           GAsyncResult *res,
                           gpointer user_data)
{
  AsyncCallSetDisplayName *data = user_data;
  GFile *file;
  GError *error = NULL;
  gchar *new_path;
  GSimpleAsyncResult *orig_result;

  orig_result = data->result;

  if (! gvfs_dbus_mount_call_set_display_name_finish (proxy, &new_path, res, &error))
    {
      _g_simple_async_result_take_error_stripped (orig_result, error);
      goto out;
    }

  g_mount_info_apply_prefix (data->mount_info, &new_path);
  file = new_file_for_new_path (G_DAEMON_FILE (data->file), new_path);

  g_free (new_path);

  g_simple_async_result_set_op_res_gpointer (orig_result, file, g_object_unref);

  out:
    _g_simple_async_result_complete_with_cancellable (orig_result, data->cancellable);
    _g_dbus_async_unsubscribe_cancellable (data->cancellable, data->cancelled_tag);
    data->result = NULL;
    g_object_unref (orig_result);   /* trigger async_proxy_create_free() */
}

static void
set_display_name_async_get_proxy_cb (GVfsDBusMount *proxy,
                                     GDBusConnection *connection,
                                     GMountInfo *mount_info,
                                     const gchar *path,
                                     GSimpleAsyncResult *result,
                                     GError *error,
                                     GCancellable *cancellable,
                                     gpointer callback_data)
{
  AsyncCallSetDisplayName *data = callback_data;

  data->result = g_object_ref (result);
  data->mount_info = g_mount_info_ref (mount_info);
  
  gvfs_dbus_mount_call_set_display_name (proxy,
                                         path,
                                         data->display_name ? data->display_name : "",
                                         cancellable,
                                         (GAsyncReadyCallback) set_display_name_async_cb,
                                         data);
  data->cancelled_tag = _g_dbus_async_subscribe_cancellable (connection, cancellable);
}

static void
g_daemon_file_set_display_name_async (GFile                      *file,
                                      const char                 *display_name,
                                      int                         io_priority,
                                      GCancellable               *cancellable,
                                      GAsyncReadyCallback         callback,
                                      gpointer                    user_data)
{
  AsyncCallSetDisplayName *data;

  data = g_new0 (AsyncCallSetDisplayName, 1);
  data->file = g_object_ref (file);
  data->display_name = g_strdup (display_name);
  data->io_priority = io_priority;
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);

  create_proxy_for_file_async (file,
                               cancellable,
                               callback, user_data,
                               set_display_name_async_get_proxy_cb,
                               data, (GDestroyNotify) async_call_set_display_name_free);
}

static GFile *
g_daemon_file_set_display_name_finish (GFile                      *file,
                                       GAsyncResult               *res,
                                       GError                    **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  GFile *new_file;

  new_file = g_simple_async_result_get_op_res_gpointer (simple);
  return new_file;
}

#if 0

static void
g_daemon_file_set_attributes_async (GFile                      *file,
                                    GFileInfo                  *info,
                                    GFileQueryInfoFlags        flags,
                                    int                         io_priority,
                                    GCancellable               *cancellable,
                                    GAsyncReadyCallback         callback,
                                    gpointer                    user_data)
{
  /* TODO */
}

static gboolean
g_daemon_file_set_attributes_finish (GFile                      *file,
                                     GAsyncResult               *result,
                                     GFileInfo                 **info,
                                     GError                    **error)
{
  /* TODO */
}

#endif

static void
g_daemon_file_file_iface_init (GFileIface *iface)
{
  iface->dup = g_daemon_file_dup;
  iface->hash = g_daemon_file_hash;
  iface->equal = g_daemon_file_equal;
  iface->is_native = g_daemon_file_is_native;
  iface->has_uri_scheme = g_daemon_file_has_uri_scheme;
  iface->get_uri_scheme = g_daemon_file_get_uri_scheme;
  iface->get_basename = g_daemon_file_get_basename;
  iface->get_path = g_daemon_file_get_path;
  iface->get_uri = g_daemon_file_get_uri;
  iface->get_parse_name = g_daemon_file_get_parse_name;
  iface->get_parent = g_daemon_file_get_parent;
  iface->prefix_matches = g_daemon_file_prefix_matches;
  iface->get_relative_path = g_daemon_file_get_relative_path;
  iface->resolve_relative_path = g_daemon_file_resolve_relative_path;
  iface->get_child_for_display_name = g_daemon_file_get_child_for_display_name;
  iface->enumerate_children = g_daemon_file_enumerate_children;
  iface->query_info = g_daemon_file_query_info;
  iface->query_info_async = g_daemon_file_query_info_async;
  iface->query_info_finish = g_daemon_file_query_info_finish;
  iface->find_enclosing_mount = g_daemon_file_find_enclosing_mount;
  iface->read_fn = g_daemon_file_read;
  iface->append_to = g_daemon_file_append_to;
  iface->create = g_daemon_file_create;
  iface->replace = g_daemon_file_replace;
  iface->read_async = g_daemon_file_read_async;
  iface->read_finish = g_daemon_file_read_finish;
  iface->mount_enclosing_volume = g_daemon_file_mount_enclosing_volume;
  iface->mount_enclosing_volume_finish = g_daemon_file_mount_enclosing_volume_finish;
  iface->mount_mountable = g_daemon_file_mount_mountable;
  iface->mount_mountable_finish = g_daemon_file_mount_mountable_finish;
  iface->unmount_mountable = g_daemon_file_unmount_mountable;
  iface->unmount_mountable_finish = g_daemon_file_unmount_mountable_finish;
  iface->unmount_mountable_with_operation = g_daemon_file_unmount_mountable_with_operation;
  iface->unmount_mountable_with_operation_finish = g_daemon_file_unmount_mountable_with_operation_finish;
  iface->eject_mountable = g_daemon_file_eject_mountable;
  iface->eject_mountable_finish = g_daemon_file_eject_mountable_finish;
  iface->eject_mountable_with_operation = g_daemon_file_eject_mountable_with_operation;
  iface->eject_mountable_with_operation_finish = g_daemon_file_eject_mountable_with_operation_finish;
  iface->poll_mountable = g_daemon_file_poll_mountable;
  iface->poll_mountable_finish = g_daemon_file_poll_mountable_finish;
  iface->query_filesystem_info = g_daemon_file_query_filesystem_info;
  iface->query_filesystem_info_async = g_daemon_file_query_filesystem_info_async;
  iface->query_filesystem_info_finish = g_daemon_file_query_filesystem_info_finish;
  iface->set_display_name = g_daemon_file_set_display_name;
  iface->delete_file = g_daemon_file_delete;
  iface->trash = g_daemon_file_trash;
  iface->make_directory = g_daemon_file_make_directory;
  iface->copy = g_daemon_file_copy;
  iface->move = g_daemon_file_move;
  iface->query_settable_attributes = g_daemon_file_query_settable_attributes;
  iface->query_writable_namespaces = g_daemon_file_query_writable_namespaces;
  iface->set_attribute = g_daemon_file_set_attribute;
  iface->make_symbolic_link = g_daemon_file_make_symbolic_link;
  iface->monitor_dir = g_daemon_file_monitor_dir;
  iface->monitor_file = g_daemon_file_monitor_file;
  iface->start_mountable = g_daemon_file_start_mountable;
  iface->start_mountable_finish = g_daemon_file_start_mountable_finish;
  iface->stop_mountable = g_daemon_file_stop_mountable;
  iface->stop_mountable_finish = g_daemon_file_stop_mountable_finish;

  /* Async operations */

  iface->append_to_async = g_daemon_file_append_to_async;
  iface->append_to_finish = g_daemon_file_append_to_finish;
  iface->create_async = g_daemon_file_create_async;
  iface->create_finish = g_daemon_file_create_finish;
  iface->enumerate_children_async = g_daemon_file_enumerate_children_async;
  iface->enumerate_children_finish = g_daemon_file_enumerate_children_finish;
  iface->find_enclosing_mount_async = g_daemon_file_find_enclosing_mount_async;
  iface->find_enclosing_mount_finish = g_daemon_file_find_enclosing_mount_finish;
  iface->replace_async = g_daemon_file_replace_async;
  iface->replace_finish = g_daemon_file_replace_finish;
  iface->set_display_name_async = g_daemon_file_set_display_name_async;
  iface->set_display_name_finish = g_daemon_file_set_display_name_finish;
#if 0
  iface->set_attributes_async = g_daemon_file_set_attributes_async;
  iface->set_attributes_finish = g_daemon_file_set_attributes_finish;
#endif
}
