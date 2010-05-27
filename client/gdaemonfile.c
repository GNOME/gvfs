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

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "gdaemonfile.h"
#include "gvfsdaemondbus.h"
#include "gdaemonmount.h"
#include <gvfsdaemonprotocol.h>
#include <gdaemonfileinputstream.h>
#include <gdaemonfileoutputstream.h>
#include <gdaemonfilemonitor.h>
#include <gdaemonfileenumerator.h>
#include <glib/gi18n-lib.h>
#include "gvfsdbusutils.h"
#include "gmountoperationdbus.h"
#include <gio/gio.h>
#include "metatree.h"

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

  if (descendant_daemon->mount_spec != parent_daemon->mount_spec)
    return FALSE;
  
  remainder = match_prefix (descendant_daemon->path, parent_daemon->path);
  if (remainder != NULL && *remainder == '/')
    return TRUE;
  return FALSE;
}

static char *
g_daemon_file_get_relative_path (GFile *parent,
				 GFile *descendant)
{
  GDaemonFile *parent_daemon = G_DAEMON_FILE (parent);
  GDaemonFile *descendant_daemon = G_DAEMON_FILE (descendant);
  const char *remainder;

  if (descendant_daemon->mount_spec != parent_daemon->mount_spec)
    return NULL;
  
  remainder = match_prefix (descendant_daemon->path, parent_daemon->path);
  
  if (remainder != NULL && *remainder == '/')
    return g_strdup (remainder + 1);
  return NULL;
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

static DBusMessage *
create_empty_message (GFile *file,
		      const char *op,
		      GMountInfo **mount_info_out,
		      GError **error)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  DBusMessage *message;
  GMountInfo *mount_info;
  const char *path;

  mount_info = _g_daemon_vfs_get_mount_info_sync (daemon_file->mount_spec,
						  daemon_file->path,
						  error);
  if (mount_info == NULL)
    return NULL;

  if (mount_info_out)
    *mount_info_out = g_mount_info_ref (mount_info);
  
  message =
    dbus_message_new_method_call (mount_info->dbus_id,
				  mount_info->object_path,
				  G_VFS_DBUS_MOUNT_INTERFACE,
				  op);

  path = g_mount_info_resolve_path (mount_info,
				    daemon_file->path);
  _g_dbus_message_append_args (message, G_DBUS_TYPE_CSTRING, &path, 0);

  g_mount_info_unref (mount_info);
  return message;
}

static DBusMessage *
do_sync_path_call (GFile *file,
		   const char *op,
		   GMountInfo **mount_info_out,
		   DBusConnection **connection_out,
		   GCancellable *cancellable,
		   GError **error,
		   int first_arg_type,
		   ...)
{
  DBusMessage *message, *reply;
  va_list var_args;
  GError *my_error;

 retry:
  
  message = create_empty_message (file, op, mount_info_out, error);
  if (!message)
    return NULL;

  va_start (var_args, first_arg_type);
  _g_dbus_message_append_args_valist (message,
				      first_arg_type,
				      var_args);
  va_end (var_args);


  my_error = NULL;
  reply = _g_vfs_daemon_call_sync (message,
				   connection_out,
				   NULL, NULL, NULL,
				   cancellable, &my_error);
  dbus_message_unref (message);

  if (reply == NULL)
    {
      if (g_error_matches (my_error, G_VFS_ERROR, G_VFS_ERROR_RETRY))
	{
	  g_error_free (my_error);
	  goto retry;
	}
      g_propagate_error (error, my_error);
    }

  return reply;
}

static DBusMessage *
do_sync_2_path_call (GFile *file1,
		     GFile *file2,
		     const char *op,
		     const char *callback_obj_path,
		     DBusObjectPathMessageFunction callback,
		     gpointer callback_user_data, 
		     DBusConnection **connection_out,
		     GCancellable *cancellable,
		     GError **error,
		     int first_arg_type,
		     ...)
{
  GDaemonFile *daemon_file1 = G_DAEMON_FILE (file1);
  GDaemonFile *daemon_file2 = G_DAEMON_FILE (file2);
  DBusMessage *message, *reply;
  GMountInfo *mount_info1, *mount_info2;
  const char *path1, *path2;
  va_list var_args;
  GError *my_error;

 retry:
  
  mount_info1 = _g_daemon_vfs_get_mount_info_sync (daemon_file1->mount_spec,
						   daemon_file1->path,
						   error);
  if (mount_info1 == NULL)
    return NULL;

  mount_info2 = NULL;
  if (daemon_file2)
    {
      mount_info2 = _g_daemon_vfs_get_mount_info_sync (daemon_file2->mount_spec,
						       daemon_file2->path,
						       error);
      if (mount_info2 == NULL)
	{
	  g_mount_info_unref (mount_info1);
	  return NULL;
	}

      if (mount_info1 != mount_info2)
	{
	  g_mount_info_unref (mount_info1);
	  /* For copy this will cause the fallback code to be involved */
	  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			       _("Operation not supported, files on different mounts"));
	  return NULL;
	}
    }
      
  message =
    dbus_message_new_method_call (mount_info1->dbus_id,
				  mount_info1->object_path,
				  G_VFS_DBUS_MOUNT_INTERFACE,
				  op);

  path1 = g_mount_info_resolve_path (mount_info1,
				     daemon_file1->path);
  _g_dbus_message_append_args (message, G_DBUS_TYPE_CSTRING, &path1, 0);

  if (daemon_file2)
    {
      path2 = g_mount_info_resolve_path (mount_info2,
					 daemon_file2->path);
      _g_dbus_message_append_args (message, G_DBUS_TYPE_CSTRING, &path2, 0);
    }
  
  va_start (var_args, first_arg_type);
  _g_dbus_message_append_args_valist (message,
				      first_arg_type,
				      var_args);
  va_end (var_args);

  my_error = NULL;
  reply = _g_vfs_daemon_call_sync (message,
				   connection_out,
				   callback_obj_path,
				   callback,
				   callback_user_data, 
				   cancellable, &my_error);
  dbus_message_unref (message);

  g_mount_info_unref (mount_info1);
  if (mount_info2)
    g_mount_info_unref (mount_info2);

  if (reply == NULL)
    {
      if (g_error_matches (my_error, G_VFS_ERROR, G_VFS_ERROR_RETRY))
	{
	  g_error_free (my_error);
	  goto retry;
	}
      g_propagate_error (error, my_error);
    }
  
  return reply;
}

typedef void (*AsyncPathCallCallback) (DBusMessage *reply,
				       DBusConnection *connection,
				       GSimpleAsyncResult *result,
				       GCancellable *cancellable,
				       gpointer callback_data);


typedef struct {
  GSimpleAsyncResult *result;
  GFile *file;
  char *op;
  GCancellable *cancellable;
  DBusMessage *args;
  AsyncPathCallCallback callback;
  gpointer callback_data;
  GDestroyNotify notify;
} AsyncPathCall;

static void
async_path_call_free (AsyncPathCall *data)
{
  if (data->notify)
    data->notify (data->callback_data);

  if (data->result)
    g_object_unref (data->result);
  g_object_unref (data->file);
  g_free (data->op);
  if (data->cancellable)
    g_object_unref (data->cancellable);
  if (data->args)
    dbus_message_unref (data->args);
  g_free (data);
}

static void
async_path_call_done (DBusMessage *reply,
		      DBusConnection *connection,
		      GError *io_error,
		      gpointer _data)
{
  AsyncPathCall *data = _data;
  GSimpleAsyncResult *result;

  if (io_error != NULL)
    {
      g_simple_async_result_set_from_error (data->result, io_error);
      _g_simple_async_result_complete_with_cancellable (data->result, data->cancellable);
      async_path_call_free (data);
    }
  else
    {
      result = data->result;
      g_object_weak_ref (G_OBJECT (result), (GWeakNotify)async_path_call_free, data);
      data->result = NULL;
      
      data->callback (reply, connection,
		      result,
		      data->cancellable,
		      data->callback_data);

      /* Free data here, or later if callback ref:ed the result */
      g_object_unref (result);
    }
}

static void
do_async_path_call_callback (GMountInfo *mount_info,
			     gpointer _data,
			     GError *error)
{
  AsyncPathCall *data = _data;
  GDaemonFile *daemon_file = G_DAEMON_FILE (data->file);
  const char *path;
  DBusMessage *message;
  DBusMessageIter arg_source, arg_dest;
  
  if (error != NULL)
    {
      g_simple_async_result_set_from_error (data->result, error);      
      _g_simple_async_result_complete_with_cancellable (data->result, data->cancellable);
      async_path_call_free (data);
      return;
    }

  message =
    dbus_message_new_method_call (mount_info->dbus_id,
				  mount_info->object_path,
				  G_VFS_DBUS_MOUNT_INTERFACE,
				  data->op);
  
  path = g_mount_info_resolve_path (mount_info, daemon_file->path);
  _g_dbus_message_append_args (message, G_DBUS_TYPE_CSTRING, &path, 0);

  /* Append more args from data->args */

  if (data->args)
    {
      dbus_message_iter_init (data->args, &arg_source);
      dbus_message_iter_init_append (message, &arg_dest);

      _g_dbus_message_iter_copy (&arg_dest, &arg_source);
    }

  _g_vfs_daemon_call_async (message,
			    async_path_call_done, data,
			    data->cancellable);
  
  dbus_message_unref (message);
}

static void
do_async_path_call (GFile *file,
		    const char *op,
		    GCancellable *cancellable,
		    GAsyncReadyCallback op_callback,
		    gpointer op_callback_data,
		    AsyncPathCallCallback callback,
		    gpointer callback_data,
		    GDestroyNotify notify,
		    int first_arg_type,
		    ...)
{
  GDaemonFile *daemon_file = G_DAEMON_FILE (file);
  va_list var_args;
  AsyncPathCall *data;

  data = g_new0 (AsyncPathCall, 1);

  data->result = g_simple_async_result_new (G_OBJECT (file),
					    op_callback, op_callback_data,
					    NULL);

  data->file = g_object_ref (file);
  data->op = g_strdup (op);
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);
  data->callback = callback;
  data->callback_data = callback_data;
  data->notify = notify;
  
  if (first_arg_type != 0)
    {
      data->args = dbus_message_new (DBUS_MESSAGE_TYPE_METHOD_CALL);
      if (data->args == NULL)
	_g_dbus_oom ();
      
      va_start (var_args, first_arg_type);
      _g_dbus_message_append_args_valist (data->args,
					  first_arg_type,
					  var_args);
      va_end (var_args);
    }
  
  
  _g_daemon_vfs_get_mount_info_async (daemon_file->mount_spec,
				     daemon_file->path,
				     do_async_path_call_callback,
				     data);
}


static GFileEnumerator *
g_daemon_file_enumerate_children (GFile      *file,
				  const char *attributes,
				  GFileQueryInfoFlags flags,
				  GCancellable *cancellable,
				  GError **error)
{
  DBusMessage *reply;
  dbus_uint32_t flags_dbus;
  char *obj_path;
  GDaemonFileEnumerator *enumerator;
  DBusConnection *connection;
  char *uri;

  enumerator = g_daemon_file_enumerator_new (file, attributes);
  obj_path = g_daemon_file_enumerator_get_object_path (enumerator);


  uri = g_file_get_uri (file);
  
  if (attributes == NULL)
    attributes = "";
  flags_dbus = flags;
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_ENUMERATE,
			     NULL, &connection,
			     cancellable, error,
			     DBUS_TYPE_STRING, &obj_path,
			     DBUS_TYPE_STRING, &attributes,
			     DBUS_TYPE_UINT32, &flags_dbus,
			     DBUS_TYPE_STRING, &uri,
			     0);
  g_free (uri);
  g_free (obj_path);

  if (reply == NULL)
    goto error;

  dbus_message_unref (reply);

  g_daemon_file_enumerator_set_sync_connection (enumerator, connection);
  
  return G_FILE_ENUMERATOR (enumerator);

 error:
  if (reply)
    dbus_message_unref (reply);
  g_object_unref (enumerator);
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

  g_file_info_set_attribute_mask (info, matcher);
  meta_tree_enumerate_keys (tree, daemon_file->path,
			    enumerate_keys_callback, info);
  g_file_info_unset_attribute_mask (info);

  meta_tree_unref (tree);
  g_file_attribute_matcher_unref (matcher);
}

static GFileInfo *
g_daemon_file_query_info (GFile                *file,
			  const char           *attributes,
			  GFileQueryInfoFlags   flags,
			  GCancellable         *cancellable,
			  GError              **error)
{
  DBusMessage *reply;
  dbus_uint32_t flags_dbus;
  DBusMessageIter iter;
  GFileInfo *info;
  char *uri;

  uri = g_file_get_uri (file);

  if (attributes == NULL)
    attributes = "";
  flags_dbus = flags;
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_QUERY_INFO,
			     NULL, NULL,
			     cancellable, error,
			     DBUS_TYPE_STRING, &attributes,
			     DBUS_TYPE_UINT32, &flags_dbus,
			     DBUS_TYPE_STRING, &uri,
			     0);

  g_free (uri);
  
  if (reply == NULL)
    return NULL;

  info = NULL;
  
  if (!dbus_message_iter_init (reply, &iter) ||
      (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_STRUCT))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   /* Translators: %s is the name of a programming function */
			   _("Invalid return value from %s"), "get_info");
      goto out;
    }

  info = _g_dbus_get_file_info (&iter, error);

  if (info)
    add_metadata (file, attributes, info);

 out:
  dbus_message_unref (reply);
  return info;
}

static void
query_info_async_cb (DBusMessage *reply,
		     DBusConnection *connection,
		     GSimpleAsyncResult *result,
		     GCancellable *cancellable,
		     gpointer callback_data)
{
  const char *attributes = callback_data;
  DBusMessageIter iter;
  GFileInfo *info;
  GError *error;
  GFile *file;

  info = NULL;
  
  if (!dbus_message_iter_init (reply, &iter) ||
      (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_STRUCT))
    {
      g_simple_async_result_set_error (result,
				       G_IO_ERROR, G_IO_ERROR_FAILED,
				       _("Invalid return value from %s"), "query_info");
      _g_simple_async_result_complete_with_cancellable (result, cancellable);
      return;
    }

  error = NULL;
  info = _g_dbus_get_file_info (&iter, &error);
  if (info == NULL)
    {
      g_simple_async_result_set_from_error (result, error);
      g_error_free (error);
      _g_simple_async_result_complete_with_cancellable (result, cancellable);
      return;
    }

  file = G_FILE (g_async_result_get_source_object (G_ASYNC_RESULT (result)));
  add_metadata (file, attributes, info);
  g_object_unref (file);

  g_simple_async_result_set_op_res_gpointer (result, info, g_object_unref);
  _g_simple_async_result_complete_with_cancellable (result, cancellable);
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
  guint32 dbus_flags;
  char *uri;

  uri = g_file_get_uri (file);

  dbus_flags = flags;
  do_async_path_call (file,
		      G_VFS_DBUS_MOUNT_OP_QUERY_INFO,
		      cancellable,
		      callback, user_data,
		      query_info_async_cb, g_strdup (attributes), g_free,
		      DBUS_TYPE_STRING, &attributes,
		      DBUS_TYPE_UINT32, &dbus_flags,
		      DBUS_TYPE_STRING, &uri,
		      0);

  g_free (uri);
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
  GSimpleAsyncResult *result;
  GCancellable *cancellable;
  gboolean can_seek;
} GetFDData;

static void
read_async_get_fd_cb (int fd,
		      gpointer callback_data)
{
  GetFDData *data = callback_data;
  GFileInputStream *stream;
  
  if (fd == -1)
    {
      g_simple_async_result_set_error (data->result,
				       G_IO_ERROR, G_IO_ERROR_FAILED,
				       _("Couldn't get stream file descriptor"));
    }
  else
    {
      stream = g_daemon_file_input_stream_new (fd, data->can_seek);
      g_simple_async_result_set_op_res_gpointer (data->result, stream, g_object_unref);
    }

  _g_simple_async_result_complete_with_cancellable (data->result, data->cancellable);

  g_object_unref (data->result);
  g_free (data);
}

static void
read_async_cb (DBusMessage *reply,
	       DBusConnection *connection,
	       GSimpleAsyncResult *result,
	       GCancellable *cancellable,
	       gpointer callback_data)
{
  guint32 fd_id;
  dbus_bool_t can_seek;
  GetFDData *get_fd_data;
  
  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_INVALID))
    {
      g_simple_async_result_set_error (result,
				       G_IO_ERROR, G_IO_ERROR_FAILED,
				       _("Invalid return value from %s"), "open");
      _g_simple_async_result_complete_with_cancellable (result, cancellable);
      return;
    }
  
  get_fd_data = g_new0 (GetFDData, 1);
  get_fd_data->result = g_object_ref (result);
  get_fd_data->can_seek = can_seek;
  
  _g_dbus_connection_get_fd_async (connection, fd_id,
				   read_async_get_fd_cb, get_fd_data);
}

static void
g_daemon_file_read_async (GFile *file,
			  int io_priority,
			  GCancellable *cancellable,
			  GAsyncReadyCallback callback,
			  gpointer callback_data)
{
  guint32 pid;

  pid = get_pid_for_file (file);

  do_async_path_call (file,
		      G_VFS_DBUS_MOUNT_OP_OPEN_FOR_READ,
		      cancellable,
		      callback, callback_data,
		      read_async_cb, NULL, NULL,
                      DBUS_TYPE_UINT32, &pid,
		      0);
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
  DBusConnection *connection;
  int fd;
  DBusMessage *reply;
  guint32 fd_id;
  dbus_bool_t can_seek;
  guint32 pid;

  pid = get_pid_for_file (file);

  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_OPEN_FOR_READ,
			     NULL, &connection,
			     cancellable, error,
                             DBUS_TYPE_UINT32, &pid,
			     0);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Invalid return value from %s"), "open");
      return NULL;
    }
  
  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Didn't get stream file descriptor"));
      return NULL;
    }
  
  return g_daemon_file_input_stream_new (fd, can_seek);
}

static GFileOutputStream *
g_daemon_file_append_to (GFile *file,
			 GFileCreateFlags flags,
			 GCancellable *cancellable,
			 GError **error)
{
  DBusConnection *connection;
  int fd;
  DBusMessage *reply;
  guint32 fd_id;
  dbus_bool_t can_seek;
  guint16 mode;
  guint64 initial_offset;
  dbus_bool_t make_backup;
  guint32 dbus_flags;
  char *etag;
  guint32 pid;

  pid = get_pid_for_file (file);

  mode = 1;
  etag = "";
  make_backup = FALSE;
  dbus_flags = flags;
  
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_OPEN_FOR_WRITE,
			     NULL, &connection,
			     cancellable, error,
			     DBUS_TYPE_UINT16, &mode,
			     DBUS_TYPE_STRING, &etag,
			     DBUS_TYPE_BOOLEAN, &make_backup,
			     DBUS_TYPE_UINT32, &dbus_flags,
                             DBUS_TYPE_UINT32, &pid,
			     0);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_UINT64, &initial_offset,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Invalid return value from %s"), "open");
      return NULL;
    }
  
  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Didn't get stream file descriptor"));
      return NULL;
    }
  
  return g_daemon_file_output_stream_new (fd, can_seek, initial_offset);
}

static GFileOutputStream *
g_daemon_file_create (GFile *file,
		      GFileCreateFlags flags,
		      GCancellable *cancellable,
		      GError **error)
{
  DBusConnection *connection;
  int fd;
  DBusMessage *reply;
  guint32 fd_id;
  dbus_bool_t can_seek;
  guint16 mode;
  guint64 initial_offset;
  dbus_bool_t make_backup;
  char *etag;
  guint32 dbus_flags;
  guint32 pid;

  pid = get_pid_for_file (file);

  mode = 0;
  etag = "";
  make_backup = FALSE;
  dbus_flags = flags;
  
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_OPEN_FOR_WRITE,
			     NULL, &connection,
			     cancellable, error,
			     DBUS_TYPE_UINT16, &mode,
			     DBUS_TYPE_STRING, &etag,
			     DBUS_TYPE_BOOLEAN, &make_backup,
			     DBUS_TYPE_UINT32, &dbus_flags,
                             DBUS_TYPE_UINT32, &pid,
			     0);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_UINT64, &initial_offset,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Invalid return value from %s"), "open");
      return NULL;
    }
  
  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Didn't get stream file descriptor"));
      return NULL;
    }
  
  return g_daemon_file_output_stream_new (fd, can_seek, initial_offset);
}

static GFileOutputStream *
g_daemon_file_replace (GFile *file,
		       const char *etag,
		       gboolean make_backup,
		       GFileCreateFlags flags,
		       GCancellable *cancellable,
		       GError **error)
{
  DBusConnection *connection;
  int fd;
  DBusMessage *reply;
  guint32 fd_id;
  dbus_bool_t can_seek;
  guint16 mode;
  guint64 initial_offset;
  dbus_bool_t dbus_make_backup;
  guint32 dbus_flags;
  guint32 pid;

  pid = get_pid_for_file (file);

  mode = 2;
  dbus_make_backup = make_backup;
  dbus_flags = flags;

  if (etag == NULL)
    etag = "";
  
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_OPEN_FOR_WRITE,
			     NULL, &connection,
			     cancellable, error,
			     DBUS_TYPE_UINT16, &mode,
			     DBUS_TYPE_STRING, &etag,
			     DBUS_TYPE_BOOLEAN, &dbus_make_backup,
			     DBUS_TYPE_UINT32, &dbus_flags,
                             DBUS_TYPE_UINT32, &pid,
			     0);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_UINT64, &initial_offset,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Invalid return value from %s"), "open");
      return NULL;
    }
  
  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Didn't get stream file descriptor"));
      return NULL;
    }
  
  return g_daemon_file_output_stream_new (fd, can_seek, initial_offset);
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
      g_simple_async_result_set_from_error (result, error);
      g_error_free (error);
    }

  g_simple_async_result_complete (result);
  g_object_unref (result);

}

static void
mount_mountable_async_cb (DBusMessage *reply,
			  DBusConnection *connection,
			  GSimpleAsyncResult *result,
			  GCancellable *cancellable,
			  gpointer callback_data)
{
  GMountOperation *mount_operation = callback_data;
  GMountSpec *mount_spec;
  char *path;
  DBusMessageIter iter;
  GFile *file;
  dbus_bool_t must_mount_location, is_uri;

  path = NULL;

  dbus_message_iter_init (reply, &iter);
  
  if (!_g_dbus_message_iter_get_args (&iter, NULL,
				      DBUS_TYPE_BOOLEAN, &is_uri,
				      G_DBUS_TYPE_CSTRING, &path,
				      DBUS_TYPE_BOOLEAN, &must_mount_location,
				      0))
    {
      g_simple_async_result_set_error (result,
				       G_IO_ERROR, G_IO_ERROR_FAILED,
				       _("Invalid return value from %s"), "call");
      _g_simple_async_result_complete_with_cancellable (result, cancellable);

      return;
    }

  if (is_uri)
    {
      file = g_file_new_for_uri (path);
    }
  else
    {
      mount_spec = g_mount_spec_from_dbus (&iter);
      if (mount_spec == NULL)
	{
	  g_simple_async_result_set_error (result,
					   G_IO_ERROR, G_IO_ERROR_FAILED,
					   _("Invalid return value from %s"), "call");
          _g_simple_async_result_complete_with_cancellable (result, cancellable);
	  return;
	}
      
      file = g_daemon_file_new (mount_spec, path);
      g_mount_spec_unref (mount_spec);
    }
  
  g_free (path);
  g_simple_async_result_set_op_res_gpointer (result, file, g_object_unref);

  if (must_mount_location)
    {
      g_file_mount_enclosing_volume (file,
				     0,
				     mount_operation,
				     cancellable,
				     mount_mountable_location_mounted_cb,
				     g_object_ref (result));
      
    }
  else
    _g_simple_async_result_complete_with_cancellable (result, cancellable);
}

static void
g_daemon_file_mount_mountable (GFile               *file,
			       GMountMountFlags     flags,
			       GMountOperation     *mount_operation,
			       GCancellable        *cancellable,
			       GAsyncReadyCallback  callback,
			       gpointer             user_data)
{
  GMountSource *mount_source;
  const char *dbus_id, *obj_path;
  
  mount_source = g_mount_operation_dbus_wrap (mount_operation, _g_daemon_vfs_get_async_bus ());
  
  dbus_id = g_mount_source_get_dbus_id (mount_source);
  obj_path = g_mount_source_get_obj_path (mount_source);

  if (mount_operation)
    g_object_ref (mount_operation);
  
  do_async_path_call (file,
		      G_VFS_DBUS_MOUNT_OP_MOUNT_MOUNTABLE,
		      cancellable,
		      callback, user_data,
		      mount_mountable_async_cb,
		      mount_operation, mount_operation ? g_object_unref : NULL,
		      DBUS_TYPE_STRING, &dbus_id,
		      DBUS_TYPE_OBJECT_PATH, &obj_path,
		      0);

  g_object_unref (mount_source);
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
start_mountable_async_cb (DBusMessage *reply,
			  DBusConnection *connection,
			  GSimpleAsyncResult *result,
			  GCancellable *cancellable,
			  gpointer callback_data)
{
  _g_simple_async_result_complete_with_cancellable (result, cancellable);
}

static void
g_daemon_file_start_mountable (GFile               *file,
			       GDriveStartFlags     flags,
			       GMountOperation     *mount_operation,
			       GCancellable        *cancellable,
			       GAsyncReadyCallback  callback,
			       gpointer             user_data)
{
  GMountSource *mount_source;
  const char *dbus_id, *obj_path;

  mount_source = g_mount_operation_dbus_wrap (mount_operation, _g_daemon_vfs_get_async_bus ());

  dbus_id = g_mount_source_get_dbus_id (mount_source);
  obj_path = g_mount_source_get_obj_path (mount_source);

  if (mount_operation)
    g_object_ref (mount_operation);

  do_async_path_call (file,
		      G_VFS_DBUS_MOUNT_OP_START_MOUNTABLE,
		      cancellable,
		      callback, user_data,
		      start_mountable_async_cb,
		      mount_operation, mount_operation ? g_object_unref : NULL,
		      DBUS_TYPE_STRING, &dbus_id,
		      DBUS_TYPE_OBJECT_PATH, &obj_path,
		      0);

  g_object_unref (mount_source);
}

static gboolean
g_daemon_file_start_mountable_finish (GFile               *file,
				      GAsyncResult        *result,
				      GError             **error)
{
  return TRUE;
}

static void
stop_mountable_async_cb (DBusMessage *reply,
                         DBusConnection *connection,
                         GSimpleAsyncResult *result,
                         GCancellable *cancellable,
                         gpointer callback_data)
{
  _g_simple_async_result_complete_with_cancellable (result, cancellable);
}

static void
g_daemon_file_stop_mountable (GFile               *file,
                              GMountUnmountFlags   flags,
                              GMountOperation     *mount_operation,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  guint32 dbus_flags;
  GMountSource *mount_source;
  const char *dbus_id, *obj_path;

  mount_source = g_mount_operation_dbus_wrap (mount_operation, _g_daemon_vfs_get_async_bus ());

  dbus_id = g_mount_source_get_dbus_id (mount_source);
  obj_path = g_mount_source_get_obj_path (mount_source);

  if (mount_operation)
    g_object_ref (mount_operation);

  dbus_flags = flags;
  do_async_path_call (file,
		      G_VFS_DBUS_MOUNT_OP_STOP_MOUNTABLE,
		      cancellable,
		      callback, user_data,
		      stop_mountable_async_cb,
		      mount_operation, mount_operation ? g_object_unref : NULL,
		      DBUS_TYPE_UINT32, &dbus_flags,
                      DBUS_TYPE_STRING, &dbus_id, DBUS_TYPE_OBJECT_PATH, &obj_path,
		      0);

  g_object_unref (mount_source);
}

static gboolean
g_daemon_file_stop_mountable_finish (GFile               *file,
                                     GAsyncResult        *result,
                                     GError             **error)
{
  return TRUE;
}


static void
eject_mountable_async_cb (DBusMessage *reply,
			  DBusConnection *connection,
			  GSimpleAsyncResult *result,
			  GCancellable *cancellable,
			  gpointer callback_data)
{
  _g_simple_async_result_complete_with_cancellable (result, cancellable);
}

static void
g_daemon_file_eject_mountable_with_operation (GFile               *file,
                                              GMountUnmountFlags   flags,
                                              GMountOperation     *mount_operation,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data)
{
  guint32 dbus_flags;
  GMountSource *mount_source;
  const char *dbus_id, *obj_path;

  mount_source = g_mount_operation_dbus_wrap (mount_operation, _g_daemon_vfs_get_async_bus ());

  dbus_id = g_mount_source_get_dbus_id (mount_source);
  obj_path = g_mount_source_get_obj_path (mount_source);

  if (mount_operation)
    g_object_ref (mount_operation);

  dbus_flags = flags;
  do_async_path_call (file,
		      G_VFS_DBUS_MOUNT_OP_EJECT_MOUNTABLE,
		      cancellable,
		      callback, user_data,
		      eject_mountable_async_cb,
		      mount_operation, mount_operation ? g_object_unref : NULL,
		      DBUS_TYPE_UINT32, &dbus_flags,
                      DBUS_TYPE_STRING, &dbus_id, DBUS_TYPE_OBJECT_PATH, &obj_path,
		      0);

  g_object_unref (mount_source);
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
unmount_mountable_async_cb (DBusMessage *reply,
			    DBusConnection *connection,
			    GSimpleAsyncResult *result,
			    GCancellable *cancellable,
			    gpointer callback_data)
{
  _g_simple_async_result_complete_with_cancellable (result, cancellable);
}

static void
g_daemon_file_unmount_mountable_with_operation (GFile               *file,
                                                GMountUnmountFlags   flags,
                                                GMountOperation     *mount_operation,
                                                GCancellable        *cancellable,
                                                GAsyncReadyCallback  callback,
                                                gpointer             user_data)
{
  guint32 dbus_flags;
  GMountSource *mount_source;
  const char *dbus_id, *obj_path;

  mount_source = g_mount_operation_dbus_wrap (mount_operation, _g_daemon_vfs_get_async_bus ());

  dbus_id = g_mount_source_get_dbus_id (mount_source);
  obj_path = g_mount_source_get_obj_path (mount_source);

  if (mount_operation)
    g_object_ref (mount_operation);

  dbus_flags = flags;
  do_async_path_call (file,
		      G_VFS_DBUS_MOUNT_OP_UNMOUNT_MOUNTABLE,
		      cancellable,
		      callback, user_data,
		      unmount_mountable_async_cb,
		      mount_operation, mount_operation ? g_object_unref : NULL,
		      DBUS_TYPE_UINT32, &dbus_flags,
                      DBUS_TYPE_STRING, &dbus_id, DBUS_TYPE_OBJECT_PATH, &obj_path,
		      0);

  g_object_unref (mount_source);
}

static gboolean
g_daemon_file_unmount_mountable_with_operation_finish (GFile               *file,
                                                       GAsyncResult        *result,
                                                       GError             **error)
{
  return TRUE;
}

static void
poll_mountable_async_cb (DBusMessage *reply,
                         DBusConnection *connection,
                         GSimpleAsyncResult *result,
                         GCancellable *cancellable,
                         gpointer callback_data)
{
  _g_simple_async_result_complete_with_cancellable (result, cancellable);
}

static void
g_daemon_file_poll_mountable (GFile               *file,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  do_async_path_call (file,
		      G_VFS_DBUS_MOUNT_OP_POLL_MOUNTABLE,
		      cancellable,
		      callback, user_data,
		      poll_mountable_async_cb,
		      NULL,
		      0,
                      0);
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
mount_reply (DBusMessage *reply,
	     GError *error,
	     gpointer user_data)
{
  MountData *data = user_data;
  GSimpleAsyncResult *res;

  if (reply == NULL)
    {
      res = g_simple_async_result_new_from_error (G_OBJECT (data->file),
						  data->callback,
						  data->user_data,
						  error);
    }
  else
    {
      res = g_simple_async_result_new (G_OBJECT (data->file),
				       data->callback,
				       data->user_data,
				       g_daemon_file_mount_enclosing_volume);
    }

  _g_simple_async_result_complete_with_cancellable (res, data->cancellable);
  
  g_object_unref (data->file);
  if (data->cancellable)
    g_object_unref (data->cancellable);
  if (data->mount_operation)
    g_object_unref (data->mount_operation);
  g_free (data);
}

static void
g_daemon_file_mount_enclosing_volume (GFile *location,
				      GMountMountFlags  flags,
				      GMountOperation *mount_operation,
				      GCancellable *cancellable,
				      GAsyncReadyCallback callback,
				      gpointer user_data)
{
  GDaemonFile *daemon_file;
  DBusMessage *message;
  GMountSpec *spec;
  GMountSource *mount_source;
  DBusMessageIter iter;
  MountData *data;
  
  daemon_file = G_DAEMON_FILE (location);
  
  message = dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
					  G_VFS_DBUS_MOUNTTRACKER_PATH,
					  G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					  G_VFS_DBUS_MOUNTTRACKER_OP_MOUNT_LOCATION);

  spec = g_mount_spec_copy (daemon_file->mount_spec);
  g_mount_spec_set_mount_prefix (spec, daemon_file->path);
  dbus_message_iter_init_append (message, &iter);
  g_mount_spec_to_dbus (&iter, spec);
  g_mount_spec_unref (spec);

  mount_source = g_mount_operation_dbus_wrap (mount_operation, _g_daemon_vfs_get_async_bus ());
  g_mount_source_to_dbus (mount_source, message);
  g_object_unref (mount_source);

  data = g_new0 (MountData, 1);
  data->callback = callback;
  if (data->cancellable)
    data->cancellable = g_object_ref (data->cancellable);
  data->user_data = user_data;
  data->file = g_object_ref (location);
  if (mount_operation)
    data->mount_operation = g_object_ref (mount_operation);

  /* TODO: Ignoring cancellable here */
  _g_dbus_connection_call_async (_g_daemon_vfs_get_async_bus (),
				 message,
				 G_VFS_DBUS_MOUNT_TIMEOUT_MSECS,
				 mount_reply, data);
 
  dbus_message_unref (message);
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
  DBusMessage *reply;
  DBusMessageIter iter;
  GFileInfo *info;

  if (attributes == NULL)
    attributes = "";
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_QUERY_FILESYSTEM_INFO,
			     NULL, NULL,
			     cancellable, error,
			     DBUS_TYPE_STRING, &attributes,
			     0);
  if (reply == NULL)
    return NULL;

  info = NULL;
  
  if (!dbus_message_iter_init (reply, &iter) ||
      (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_STRUCT))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Invalid return value from %s"), "get_filesystem_info");
      goto out;
    }

  info = _g_dbus_get_file_info (&iter, error);

 out:
  dbus_message_unref (reply);
  return info;
}

static void
query_fs_info_async_cb (DBusMessage *reply,
			DBusConnection *connection,
			GSimpleAsyncResult *result,
			GCancellable *cancellable,
			gpointer callback_data)
{
  DBusMessageIter iter;
  GFileInfo *info;
  GError *error;

  info = NULL;
  
  if (!dbus_message_iter_init (reply, &iter) ||
      (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_STRUCT))
    {
      g_simple_async_result_set_error (result,
				       G_IO_ERROR, G_IO_ERROR_FAILED,
				       _("Invalid return value from %s"), "query_info");
      _g_simple_async_result_complete_with_cancellable (result, cancellable);
      return;
    }

  error = NULL;
  info = _g_dbus_get_file_info (&iter, &error);
  if (info == NULL)
    {
      g_simple_async_result_set_from_error (result, error);
      g_error_free (error);
      _g_simple_async_result_complete_with_cancellable (result, cancellable);
      return;
    }

  g_simple_async_result_set_op_res_gpointer (result, info, g_object_unref);
  _g_simple_async_result_complete_with_cancellable (result, cancellable);
}

static void
g_daemon_file_query_filesystem_info_async (GFile                      *file,
					   const char                 *attributes,
					   int                         io_priority,
					   GCancellable               *cancellable,
					   GAsyncReadyCallback         callback,
					   gpointer                    user_data)
{
  do_async_path_call (file,
		      G_VFS_DBUS_MOUNT_OP_QUERY_FILESYSTEM_INFO,
		      cancellable,
		      callback, user_data,
		      query_fs_info_async_cb, NULL, NULL,
		      DBUS_TYPE_STRING, &attributes,
		      0);
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
						  error);
  if (mount_info == NULL)
    return NULL;

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
						NULL);


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
  DBusMessage *reply;
  DBusMessageIter iter;
  char *new_path;

  daemon_file = G_DAEMON_FILE (file);
  
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_SET_DISPLAY_NAME,
			     NULL, NULL,
			     cancellable, error,
			     DBUS_TYPE_STRING, &display_name,
			     0);
  if (reply == NULL)
    return NULL;


  if (!dbus_message_iter_init (reply, &iter) ||
      !_g_dbus_message_iter_get_args (&iter, NULL,
				      G_DBUS_TYPE_CSTRING, &new_path,
				      0))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Invalid return value from %s"), "query_filesystem_info");
      goto out;
    }

  file = new_file_for_new_path (daemon_file, new_path);
  g_free (new_path);

 out:
  dbus_message_unref (reply);
  return file;
}

static gboolean
g_daemon_file_delete (GFile *file,
		      GCancellable *cancellable,
		      GError **error)
{
  DBusMessage *reply;

  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_DELETE,
			     NULL, NULL,
			     cancellable, error,
			     0);
  if (reply == NULL)
    return FALSE;

  dbus_message_unref (reply);
  return TRUE;
}

static gboolean
g_daemon_file_trash (GFile *file,
		     GCancellable *cancellable,
		     GError **error)
{
  DBusMessage *reply;

  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_TRASH,
			     NULL, NULL,
			     cancellable, error,
			     0);
  if (reply == NULL)
    return FALSE;

  dbus_message_unref (reply);
  return TRUE;
}

static gboolean
g_daemon_file_make_directory (GFile *file,
			      GCancellable *cancellable,
			      GError **error)
{
  DBusMessage *reply;

  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_MAKE_DIRECTORY,
			     NULL, NULL,
			     cancellable, error,
			     0);
  if (reply == NULL)
    return FALSE;

  dbus_message_unref (reply);
  return TRUE;
}

static gboolean
g_daemon_file_make_symbolic_link (GFile *file,
				  const char *symlink_value,
				  GCancellable *cancellable,
				  GError **error)
{
  DBusMessage *reply;

  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_MAKE_SYMBOLIC_LINK,
			     NULL, NULL,
			     cancellable, error,
			     G_DBUS_TYPE_CSTRING, &symlink_value,
			     0);
  if (reply == NULL)
    return FALSE;

  dbus_message_unref (reply);
  return TRUE;
}

static GFileAttributeInfoList *
g_daemon_file_query_settable_attributes (GFile                      *file,
					 GCancellable               *cancellable,
					 GError                    **error)
{
  DBusMessage *reply;
  GFileAttributeInfoList *list;
  DBusMessageIter iter;

  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_QUERY_SETTABLE_ATTRIBUTES,
			     NULL, NULL,
			     cancellable, error,
			     0);
  if (reply == NULL)
    return NULL;

  dbus_message_iter_init (reply, &iter);
  list = _g_dbus_get_attribute_info_list (&iter, error);
  
  dbus_message_unref (reply);
  
  return list;
}

static GFileAttributeInfoList *
g_daemon_file_query_writable_namespaces (GFile                      *file,
					 GCancellable               *cancellable,
					 GError                    **error)
{
  DBusMessage *reply;
  GFileAttributeInfoList *list;
  DBusMessageIter iter;

  reply = do_sync_path_call (file,
			     G_VFS_DBUS_MOUNT_OP_QUERY_WRITABLE_NAMESPACES,
			     NULL, NULL,
			     cancellable, error,
			     0);
  if (reply)
    {
      dbus_message_iter_init (reply, &iter);
      list = _g_dbus_get_attribute_info_list (&iter, error);
      dbus_message_unref (reply);
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
  DBusMessage *message;
  char *treename;
  const char *metatreefile;
  MetaTree *tree;
  int appended;
  gboolean res;

  daemon_file = G_DAEMON_FILE (file);

  treename = g_mount_spec_to_string (daemon_file->mount_spec);
  tree = meta_tree_lookup_by_name (treename, FALSE);
  g_free (treename);

  message =
    dbus_message_new_method_call (G_VFS_DBUS_METADATA_NAME,
				  G_VFS_DBUS_METADATA_PATH,
				  G_VFS_DBUS_METADATA_INTERFACE,
				  G_VFS_DBUS_METADATA_OP_SET);
  g_assert (message != NULL);
  metatreefile = meta_tree_get_filename (tree);
  _g_dbus_message_append_args (message,
			       G_DBUS_TYPE_CSTRING, &metatreefile,
			       G_DBUS_TYPE_CSTRING, &daemon_file->path,
			       0);

  appended = _g_daemon_vfs_append_metadata_for_set (message,
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
      !_g_daemon_vfs_send_message_sync (message,
					cancellable, error))
    res = FALSE;

  dbus_message_unref (message);

  meta_tree_unref (tree);

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
  DBusMessage *message, *reply;
  DBusMessageIter iter;
  dbus_uint32_t flags_dbus;
  GError *my_error;

  if (g_str_has_prefix (attribute, "metadata::"))
    return set_metadata_attribute (file, attribute, type, value_p, cancellable, error);

 retry:
  
  message = create_empty_message (file, G_VFS_DBUS_MOUNT_OP_SET_ATTRIBUTE, NULL, error);
  if (!message)
    return FALSE;

  dbus_message_iter_init_append (message, &iter);

  flags_dbus = flags;
  dbus_message_iter_append_basic (&iter,
				  DBUS_TYPE_UINT32,
				  &flags_dbus);

  _g_dbus_append_file_attribute (&iter, attribute, 0, type, value_p);

  my_error = NULL;
  reply = _g_vfs_daemon_call_sync (message,
				   NULL,
				   NULL, NULL, NULL,
				   cancellable, &my_error);

  dbus_message_unref (message);

  if (reply == NULL)
    {
      if (g_error_matches (my_error, G_VFS_ERROR, G_VFS_ERROR_RETRY))
	{
	  g_error_free (my_error);
	  goto retry;
	}
      g_propagate_error (error, my_error);
      return FALSE;
    }

  dbus_message_unref (reply);
  return TRUE;
}

struct ProgressCallbackData {
  GFileProgressCallback progress_callback;
  gpointer progress_callback_data;
};

static DBusHandlerResult
progress_callback_message (DBusConnection  *connection,
			   DBusMessage     *message,
			   void            *user_data)
{
  struct ProgressCallbackData *data = user_data;
  dbus_uint64_t current_dbus, total_dbus;
  
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_PROGRESS_INTERFACE,
				   G_VFS_DBUS_PROGRESS_OP_PROGRESS))
    {
      if (dbus_message_get_args (message, NULL, 
				 DBUS_TYPE_UINT64, &current_dbus,
				 DBUS_TYPE_UINT64, &total_dbus,
				 0))
	data->progress_callback (current_dbus, total_dbus, data->progress_callback_data);
    }
  else
    g_warning ("Unknown progress callback message type\n");
  
  /* TODO: demarshal args and call reall callback */
  return DBUS_HANDLER_RESULT_HANDLED;
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
  DBusMessage *reply;
  char *obj_path, *dbus_obj_path;
  dbus_uint32_t flags_dbus;
  dbus_bool_t dbus_remove_source;
  struct ProgressCallbackData data;
  char *local_path = NULL;
  gboolean source_is_daemon;
  gboolean dest_is_daemon;
  gboolean native_transfer;
  gboolean send_progress;

  native_transfer  = FALSE;
  source_is_daemon = G_IS_DAEMON_FILE (source);
  dest_is_daemon   = G_IS_DAEMON_FILE (destination);
  send_progress    = progress_callback != NULL;

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

  if (progress_callback)
    {
      obj_path = g_strdup_printf ("/org/gtk/vfs/callback/%p", &obj_path);
      dbus_obj_path = obj_path;
    }
  else
    {
      obj_path = NULL;
      /* Can't pass NULL obj path as arg */
      dbus_obj_path = "/org/gtk/vfs/void";
    }

  data.progress_callback = progress_callback;
  data.progress_callback_data = progress_callback_data;

  flags_dbus = flags;
  dbus_remove_source = remove_source;

  if (native_transfer == TRUE)
    {
      const char *method_string;

      if (remove_source == FALSE)
        method_string = G_VFS_DBUS_MOUNT_OP_COPY;
      else
        method_string = G_VFS_DBUS_MOUNT_OP_MOVE;

      reply = do_sync_2_path_call (source, destination,
                                   method_string,
                                   obj_path, progress_callback_message, &data,
                                   NULL, cancellable, error,
                                   DBUS_TYPE_UINT32, &flags_dbus,
                                   DBUS_TYPE_OBJECT_PATH, &dbus_obj_path,
                                   0);
    }
  else if (dest_is_daemon == TRUE)
    {
      reply = do_sync_2_path_call (destination, NULL,
                                   G_VFS_DBUS_MOUNT_OP_PUSH,
                                   obj_path, progress_callback_message, &data,
                                   NULL, cancellable, error,
                                   G_DBUS_TYPE_CSTRING, &local_path,
                                   DBUS_TYPE_BOOLEAN, &send_progress,
                                   DBUS_TYPE_UINT32, &flags_dbus,
                                   DBUS_TYPE_OBJECT_PATH, &dbus_obj_path,
                                   DBUS_TYPE_BOOLEAN, &dbus_remove_source,
                                   0);
    }
  else
    {
      reply = do_sync_2_path_call (source, NULL,
                                   G_VFS_DBUS_MOUNT_OP_PULL,
                                   obj_path, progress_callback_message, &data,
                                   NULL, cancellable, error,
                                   G_DBUS_TYPE_CSTRING, &local_path,
                                   DBUS_TYPE_BOOLEAN, &send_progress,
                                   DBUS_TYPE_UINT32, &flags_dbus,
                                   DBUS_TYPE_OBJECT_PATH, &dbus_obj_path,
                                   DBUS_TYPE_BOOLEAN, &dbus_remove_source,
                                   0);

    }

  g_free (local_path);
  g_free (obj_path);

  if (reply == NULL)
    return FALSE;

  dbus_message_unref (reply);
  return TRUE;
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
  char *obj_path;
  dbus_uint32_t flags_dbus;
  GMountInfo *mount_info;
  DBusMessage *reply;

  flags_dbus = flags;

  mount_info = NULL;
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_CREATE_DIR_MONITOR,
			     &mount_info, NULL,
			     cancellable, error,
			     DBUS_TYPE_UINT32, &flags_dbus,
			     0);
  
  if (reply == NULL)
    {
      if (mount_info)
	g_mount_info_unref (mount_info);
      return NULL;
    }
  
  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_OBJECT_PATH, &obj_path,
			      DBUS_TYPE_INVALID))
    {
      g_mount_info_unref (mount_info);
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Invalid return value from %s"), "monitor_dir");
      return NULL;
    }
  
  monitor = g_daemon_file_monitor_new (mount_info->dbus_id,
				       obj_path);
  
  g_mount_info_unref (mount_info);
  dbus_message_unref (reply);
  
  return monitor;
}

static GFileMonitor*
g_daemon_file_monitor_file (GFile* file,
			    GFileMonitorFlags flags,
			    GCancellable *cancellable,
			    GError **error)
{
  GFileMonitor *monitor;
  char *obj_path;
  dbus_uint32_t flags_dbus;
  GMountInfo *mount_info;
  DBusMessage *reply;

  flags_dbus = flags;

  mount_info = NULL;
  reply = do_sync_path_call (file, 
			     G_VFS_DBUS_MOUNT_OP_CREATE_FILE_MONITOR,
			     &mount_info, NULL,
			     cancellable, error,
			     DBUS_TYPE_UINT32, &flags_dbus,
			     0);
  
  if (reply == NULL)
    {
      if (mount_info)
	g_mount_info_unref (mount_info);
      return NULL;
    }
  
  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_OBJECT_PATH, &obj_path,
			      DBUS_TYPE_INVALID))
    {
      g_mount_info_unref (mount_info);
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Invalid return value from %s"), "monitor_file");
      return NULL;
    }
  
  monitor = g_daemon_file_monitor_new (mount_info->dbus_id,
				       obj_path);
  
  g_mount_info_unref (mount_info);
  dbus_message_unref (reply);
  
  return monitor;
}

typedef struct
{
  GSimpleAsyncResult *result;
  GCancellable       *cancellable;
  dbus_bool_t         can_seek;
  guint64             initial_offset;
}
StreamOpenParams;

static void
stream_open_cb (gint fd, StreamOpenParams *params)
{
  GFileOutputStream *output_stream;

  if (fd == -1)
    {
      g_simple_async_result_set_error (params->result, G_IO_ERROR, G_IO_ERROR_FAILED,
                                       "%s", _("Didn't get stream file descriptor"));
      goto out;
    }

  output_stream = g_daemon_file_output_stream_new (fd, params->can_seek, params->initial_offset);
  g_simple_async_result_set_op_res_gpointer (params->result, output_stream, g_object_unref);

out:
  _g_simple_async_result_complete_with_cancellable (params->result, params->cancellable);
  if (params->cancellable)
    g_object_unref (params->cancellable);
  g_object_unref (params->result);
  g_slice_free (StreamOpenParams, params);
}

static void
append_to_async_cb (DBusMessage *reply,
                    DBusConnection *connection,
                    GSimpleAsyncResult *result,
                    GCancellable *cancellable,
                    gpointer callback_data)
{
  guint32 fd_id;
  StreamOpenParams *open_params;

  open_params = g_slice_new0 (StreamOpenParams);

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &open_params->can_seek,
			      DBUS_TYPE_UINT64, &open_params->initial_offset,
			      DBUS_TYPE_INVALID))
    {
      g_simple_async_result_set_error (result, G_IO_ERROR, G_IO_ERROR_FAILED,
                                       _("Invalid return value from %s"), "open");
      goto failure;
    }

  open_params->result = g_object_ref (result);
  if (cancellable)
    open_params->cancellable = g_object_ref (cancellable);
  _g_dbus_connection_get_fd_async (connection, fd_id,
                                   (GetFdAsyncCallback) stream_open_cb, open_params);
  return;

failure:
  g_slice_free (StreamOpenParams, open_params);
  _g_simple_async_result_complete_with_cancellable (result, cancellable);
}

static void
g_daemon_file_append_to_async (GFile                      *file,
                               GFileCreateFlags            flags,
                               int                         io_priority,
                               GCancellable               *cancellable,
                               GAsyncReadyCallback         callback,
                               gpointer                    user_data)
{
  guint16 mode;
  dbus_bool_t make_backup;
  guint32 dbus_flags;
  char *etag;
  guint32 pid;

  pid = get_pid_for_file (file);

  mode = 1;
  etag = "";
  make_backup = FALSE;
  dbus_flags = flags;
  
  do_async_path_call (file, 
                      G_VFS_DBUS_MOUNT_OP_OPEN_FOR_WRITE,
                      cancellable,
                      callback, user_data,
                      append_to_async_cb, NULL, NULL,
                      DBUS_TYPE_UINT16, &mode,
                      DBUS_TYPE_STRING, &etag,
                      DBUS_TYPE_BOOLEAN, &make_backup,
                      DBUS_TYPE_UINT32, &dbus_flags,
                      DBUS_TYPE_UINT32, &pid,
                      0);
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
create_async_cb (DBusMessage *reply,
                 DBusConnection *connection,
                 GSimpleAsyncResult *result,
                 GCancellable *cancellable,
                 gpointer callback_data)
{
  guint32 fd_id;
  StreamOpenParams *open_params;

  open_params = g_slice_new0 (StreamOpenParams);

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &open_params->can_seek,
			      DBUS_TYPE_UINT64, &open_params->initial_offset,
			      DBUS_TYPE_INVALID))
    {
      g_simple_async_result_set_error (result, G_IO_ERROR, G_IO_ERROR_FAILED,
                                       _("Invalid return value from %s"), "open");
      goto failure;
    }

  open_params->result = g_object_ref (result);
  _g_dbus_connection_get_fd_async (connection, fd_id,
                                   (GetFdAsyncCallback) stream_open_cb, open_params);
  return;

failure:
  g_slice_free (StreamOpenParams, open_params);
  _g_simple_async_result_complete_with_cancellable (result, cancellable);
}

static void
g_daemon_file_create_async (GFile                      *file,
                            GFileCreateFlags            flags,
                            int                         io_priority,
                            GCancellable               *cancellable,
                            GAsyncReadyCallback         callback,
                            gpointer                    user_data)
{
  guint16 mode;
  dbus_bool_t make_backup;
  char *etag;
  guint32 dbus_flags;
  guint32 pid;

  pid = get_pid_for_file (file);

  mode = 0;
  etag = "";
  make_backup = FALSE;
  dbus_flags = flags;
  
  do_async_path_call (file, 
                      G_VFS_DBUS_MOUNT_OP_OPEN_FOR_WRITE,
                      cancellable,
                      callback, user_data,
                      create_async_cb, NULL, NULL,
                      DBUS_TYPE_UINT16, &mode,
                      DBUS_TYPE_STRING, &etag,
                      DBUS_TYPE_BOOLEAN, &make_backup,
                      DBUS_TYPE_UINT32, &dbus_flags,
                      DBUS_TYPE_UINT32, &pid,
                      0);
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

static void
enumerate_children_async_cb (DBusMessage *reply,
                             DBusConnection *connection,
                             GSimpleAsyncResult *result,
                             GCancellable *cancellable,
                             gpointer callback_data)
{
  GDaemonFileEnumerator *enumerator = callback_data;

  if (reply == NULL || connection == NULL)
  {
    g_simple_async_result_set_error (result, G_IO_ERROR, G_IO_ERROR_FAILED,
                                     _("Invalid return value from %s"), "enumerate_children");
    goto out;
  }

  g_object_ref (enumerator);

  g_simple_async_result_set_op_res_gpointer (result, enumerator, g_object_unref);

out:
  _g_simple_async_result_complete_with_cancellable (result, cancellable);
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
  dbus_uint32_t flags_dbus;
  char *obj_path;
  GDaemonFileEnumerator *enumerator;
  char *uri;

  enumerator = g_daemon_file_enumerator_new (file, attributes);
  obj_path = g_daemon_file_enumerator_get_object_path (enumerator);

  uri = g_file_get_uri (file);

  if (attributes == NULL)
    attributes = "";
  flags_dbus = flags;
  do_async_path_call (file, 
                      G_VFS_DBUS_MOUNT_OP_ENUMERATE,
                      cancellable,
                      callback, user_data,
                      enumerate_children_async_cb, enumerator, g_object_unref,
                      DBUS_TYPE_STRING, &obj_path,
                      DBUS_TYPE_STRING, &attributes,
                      DBUS_TYPE_UINT32, &flags_dbus,
                      DBUS_TYPE_STRING, &uri,
                      0);
  g_free (uri);
  g_free (obj_path);
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
      g_simple_async_result_set_from_error (data->result, my_error);
      goto out;
    }

  if (error)
    {
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

  if (my_error)
    g_error_free (my_error);
  if (data->cancellable)
    g_object_unref (data->cancellable);
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
replace_async_cb (DBusMessage *reply,
                  DBusConnection *connection,
                  GSimpleAsyncResult *result,
                  GCancellable *cancellable,
                  gpointer callback_data)
{
  guint32 fd_id;
  StreamOpenParams *open_params;

  open_params = g_slice_new0 (StreamOpenParams);

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &open_params->can_seek,
			      DBUS_TYPE_UINT64, &open_params->initial_offset,
			      DBUS_TYPE_INVALID))
    {
      g_simple_async_result_set_error (result, G_IO_ERROR, G_IO_ERROR_FAILED,
                                       _("Invalid return value from %s"), "open");
      goto failure;
    }

  open_params->result = g_object_ref (result);
  _g_dbus_connection_get_fd_async (connection, fd_id,
                                   (GetFdAsyncCallback) stream_open_cb, open_params);
  return;

failure:
  g_slice_free (StreamOpenParams, open_params);
  _g_simple_async_result_complete_with_cancellable (result, cancellable);
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
  dbus_bool_t dbus_make_backup = make_backup;
  guint32 dbus_flags = flags;
  guint16 mode = 2;
  guint32 pid;

  pid = get_pid_for_file (file);
  
  if (etag == NULL)
    etag = "";

  do_async_path_call (file,
                      G_VFS_DBUS_MOUNT_OP_OPEN_FOR_WRITE,
                      cancellable,
                      callback, user_data,
                      replace_async_cb, NULL, NULL,
                      DBUS_TYPE_UINT16, &mode,
                      DBUS_TYPE_STRING, &etag,
                      DBUS_TYPE_BOOLEAN, &dbus_make_backup,
                      DBUS_TYPE_UINT32, &dbus_flags,
                      DBUS_TYPE_UINT32, &pid,
                      0);
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

static void
set_display_name_async_cb (DBusMessage *reply,
                           DBusConnection *connection,
                           GSimpleAsyncResult *result,
                           GCancellable *cancellable,
                           gpointer callback_data)
{
  GDaemonFile *daemon_file = callback_data;
  GFile *file;
  DBusMessageIter iter;
  gchar *new_path;

  if (!dbus_message_iter_init (reply, &iter) ||
      !_g_dbus_message_iter_get_args (&iter, NULL,
				      G_DBUS_TYPE_CSTRING, &new_path,
				      0))
    {
      g_simple_async_result_set_error (result, G_IO_ERROR, G_IO_ERROR_FAILED,
                                       _("Invalid return value from %s"), "set_display_name");
      goto out;
    }

  file = new_file_for_new_path (daemon_file, new_path);
  g_free (new_path);

  g_simple_async_result_set_op_res_gpointer (result, file, g_object_unref);

out:
  _g_simple_async_result_complete_with_cancellable (result, cancellable);
}

static void
g_daemon_file_set_display_name_async (GFile                      *file,
                                      const char                 *display_name,
                                      int                         io_priority,
                                      GCancellable               *cancellable,
                                      GAsyncReadyCallback         callback,
                                      gpointer                    user_data)
{
  g_object_ref (file);

  do_async_path_call (file,
                      G_VFS_DBUS_MOUNT_OP_SET_DISPLAY_NAME,
                      cancellable,
                      callback, user_data,
                      set_display_name_async_cb, file, g_object_unref,
                      DBUS_TYPE_STRING, &display_name,
                      0);
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
