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

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <stdlib.h>
#include <dbus/dbus.h>
#include "gvfsdbusutils.h"
#include "metatree.h"
#include "gvfsdaemonprotocol.h"

#define WRITEOUT_TIMEOUT_SECS 60

typedef struct {
  char *filename;
  MetaTree *tree;
  guint writeout_timeout;
} TreeInfo;

static GHashTable *tree_infos = NULL;

static void
tree_info_free (TreeInfo *info)
{
  g_free (info->filename);
  meta_tree_unref (info->tree);
  if (info->writeout_timeout)
    g_source_remove (info->writeout_timeout);

  g_free (info);
}

static gboolean
writeout_timeout (gpointer data)
{
  TreeInfo *info = data;

  meta_tree_flush (info->tree);
  info->writeout_timeout = 0;

  return FALSE;
}

static void
tree_info_schedule_writeout (TreeInfo *info)
{
  if (info->writeout_timeout == 0)
    info->writeout_timeout =
      g_timeout_add_seconds (WRITEOUT_TIMEOUT_SECS,
			     writeout_timeout, info);
}

static TreeInfo *
tree_info_new (const char *filename)
{
  TreeInfo *info;
  MetaTree *tree;

  tree = meta_tree_open (filename, TRUE);
  if (tree == NULL)
    return NULL;

  info = g_new0 (TreeInfo, 1);
  info->filename = g_strdup (filename);
  info->tree = tree;
  info->writeout_timeout = 0;

  return info;
}

static TreeInfo *
tree_info_lookup (const char *filename)
{
  TreeInfo *info;

  info = g_hash_table_lookup (tree_infos, filename);
  if (info)
    return info;

  info = tree_info_new (filename);
  if (info)
    g_hash_table_insert (tree_infos,
			 info->filename,
			 info);
  return info;
}

static gboolean
metadata_set (const char *treefile,
	      const char *path,
	      DBusMessageIter *iter,
	      DBusError *derror)
{
  TreeInfo *info;
  const char *str;
  char **strv;
  gboolean res;
  const char *key;
  int n_elements;
  char c;

  info = tree_info_lookup (treefile);
  if (info == NULL)
    {
      dbus_set_error (derror,
		      DBUS_ERROR_FILE_NOT_FOUND,
		      _("Can't find metadata file %s"),
		      treefile);
      return FALSE;
    }

  res = TRUE;
  while (dbus_message_iter_get_arg_type (iter) != 0)
    {
      if (!_g_dbus_message_iter_get_args (iter, derror,
					  DBUS_TYPE_STRING, &key,
					  0))
	{
	  res = FALSE;
	  break;
	}

      if (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_ARRAY)
	{
	  /* stringv */
	  if (!_g_dbus_message_iter_get_args (iter, derror,
					      DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &strv, &n_elements,
					      0))
	    {
	      res = FALSE;
	      break;
	    }
	  if (!meta_tree_set_stringv (info->tree, path, key, strv))
	    {
	      dbus_set_error (derror,
			      DBUS_ERROR_FAILED,
			      _("Unable to set metadata key"));
	      res = FALSE;
	    }
	  g_strfreev (strv);
	}
      else if (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_STRING)
	{
	  /* string */
	  if (!_g_dbus_message_iter_get_args (iter, derror,
					      DBUS_TYPE_STRING, &str,
					      0))
	    {
	      res = FALSE;
	      break;
	    }
	  if (!meta_tree_set_string (info->tree, path, key, str))
	    {
	      dbus_set_error (derror,
			      DBUS_ERROR_FAILED,
			      _("Unable to set metadata key"));
	      res = FALSE;
	    }
	}
      else if (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_BYTE)
	{
	  /* Unset */
	  if (!_g_dbus_message_iter_get_args (iter, derror,
					      DBUS_TYPE_BYTE, &c,
					      0))
	    {
	      res = FALSE;
	      break;
	    }
	  if (!meta_tree_unset (info->tree, path, key))
	    {
	      dbus_set_error (derror,
			      DBUS_ERROR_FAILED,
			      _("Unable to unset metadata key"));
	      res = FALSE;
	    }
	}
    }

  tree_info_schedule_writeout (info);

  return res;
}

static void
append_string (DBusMessageIter *iter,
	       const char *key,
	       const char *string)
{
  DBusMessageIter variant_iter, struct_iter;

  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_STRUCT,
					 NULL,
					 &struct_iter))
    _g_dbus_oom ();

  if (!dbus_message_iter_append_basic (&struct_iter,
				       DBUS_TYPE_STRING,
				       &key))
    _g_dbus_oom ();

  if (!dbus_message_iter_open_container (&struct_iter,
					 DBUS_TYPE_VARIANT,
					 DBUS_TYPE_STRING_AS_STRING,
					 &variant_iter))
    _g_dbus_oom ();

  if (!dbus_message_iter_append_basic (&variant_iter,
				       DBUS_TYPE_STRING, &string))
    _g_dbus_oom ();

  if (!dbus_message_iter_close_container (&struct_iter, &variant_iter))
    _g_dbus_oom ();

  if (!dbus_message_iter_close_container (iter, &struct_iter))
    _g_dbus_oom ();
}

static void
append_stringv (DBusMessageIter *iter,
		const char *key,
		char **stringv)
{
  DBusMessageIter variant_iter, struct_iter;

  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_STRUCT,
					 NULL,
					 &struct_iter))
    _g_dbus_oom ();

  if (!dbus_message_iter_append_basic (&struct_iter,
				       DBUS_TYPE_STRING,
				       &key))
    _g_dbus_oom ();

  if (!dbus_message_iter_open_container (&struct_iter,
					 DBUS_TYPE_VARIANT,
					 DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_STRING_AS_STRING,
					 &variant_iter))
    _g_dbus_oom ();

  _g_dbus_message_iter_append_args (&variant_iter,
				    DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &stringv, g_strv_length (stringv),
				    0);

  if (!dbus_message_iter_close_container (&struct_iter, &variant_iter))
    _g_dbus_oom ();

  if (!dbus_message_iter_close_container (iter, &struct_iter))
    _g_dbus_oom ();
}

static void
append_key (DBusMessageIter *iter,
	    MetaTree *tree,
	    const char *path,
	    const char *key)
{
  MetaKeyType keytype;
  char *str;
  char **strv;

  keytype = meta_tree_lookup_key_type (tree, path, key);

  if (keytype == META_KEY_TYPE_STRING)
    {
      str = meta_tree_lookup_string (tree, path, key);
      append_string (iter, key, str);
      g_free (str);
    }
  else if (keytype == META_KEY_TYPE_STRINGV)
    {
      strv = meta_tree_lookup_stringv (tree, path, key);
      append_stringv (iter, key, strv);
      g_strfreev (strv);
    }
}

static gboolean
enum_keys (const char *key,
	   MetaKeyType type,
	   gpointer value,
	   gpointer user_data)
{
  GPtrArray *keys = user_data;

  g_ptr_array_add (keys, g_strdup (key));
  return TRUE;
}

static DBusMessage *
metadata_get (const char *treefile,
	      const char *path,
	      DBusMessage *message,
	      DBusMessageIter *iter,
	      DBusError *derror)
{
  TreeInfo *info;
  char *key;
  DBusMessage *reply;
  DBusMessageIter reply_iter;
  GPtrArray *keys;
  int i;
  gboolean free_keys;

  info = tree_info_lookup (treefile);
  if (info == NULL)
    {
      dbus_set_error (derror,
		      DBUS_ERROR_FILE_NOT_FOUND,
		      _("Can't find metadata file %s"),
		      treefile);
      return NULL;
    }

  reply = dbus_message_new_method_return (message);
  dbus_message_iter_init_append (reply, &reply_iter);

  keys = g_ptr_array_new ();
  if (dbus_message_iter_get_arg_type (iter) == 0)
    {
      /* Get all keys */
      free_keys = TRUE;
      meta_tree_enumerate_keys (info->tree, path, enum_keys, keys);
    }
  else
    {
      free_keys = FALSE;
      while (dbus_message_iter_get_arg_type (iter) != 0)
	{
	  if (!_g_dbus_message_iter_get_args (iter, derror,
					      DBUS_TYPE_STRING, &key,
					      0))
	    break;

	  g_ptr_array_add (keys, key);
	}
    }

  for (i = 0; i < keys->len; i++)
    {
      key = g_ptr_array_index (keys, i);
      append_key (&reply_iter, info->tree, path, key);
      if (free_keys)
	g_free (key);
    }
  g_ptr_array_free (keys, TRUE);

  return reply;
}

static gboolean
metadata_unset (const char *treefile,
		const char *path,
		const char *key,
		DBusError *derror)
{
  TreeInfo *info;

  info = tree_info_lookup (treefile);
  if (info == NULL)
    {
      dbus_set_error (derror,
		      DBUS_ERROR_FILE_NOT_FOUND,
		      _("Can't find metadata file %s"),
		      treefile);
      return FALSE;
    }

  if (!meta_tree_unset (info->tree, path, key))
    {
      dbus_set_error (derror,
		      DBUS_ERROR_FAILED,
		      _("Unable to unset metadata key"));
      return FALSE;
    }

  tree_info_schedule_writeout (info);
  return TRUE;
}

static gboolean
metadata_remove (const char *treefile,
		 const char *path,
		 DBusError *derror)
{
  TreeInfo *info;

  info = tree_info_lookup (treefile);
  if (info == NULL)
    {
      dbus_set_error (derror,
		      DBUS_ERROR_FILE_NOT_FOUND,
		      _("Can't find metadata file %s"),
		      treefile);
      return FALSE;
    }

  if (!meta_tree_remove (info->tree, path))
    {
      dbus_set_error (derror,
		      DBUS_ERROR_FAILED,
		      _("Unable to remove metadata keys"));
      return FALSE;
    }

  tree_info_schedule_writeout (info);
  return TRUE;
}

static gboolean
metadata_move (const char *treefile,
	       const char *src_path,
	       const char *dest_path,
	       DBusError *derror)
{
  TreeInfo *info;

  info = tree_info_lookup (treefile);
  if (info == NULL)
    {
      dbus_set_error (derror,
		      DBUS_ERROR_FILE_NOT_FOUND,
		      _("Can't find metadata file %s"),
		      treefile);
      return FALSE;
    }

  /* Overwrites any dest */
  if (!meta_tree_copy (info->tree, src_path, dest_path))
    {
      dbus_set_error (derror,
		      DBUS_ERROR_FAILED,
		      _("Unable to move metadata keys"));
      return FALSE;
    }

  /* Remove source if copy succeeded (ignoring errors) */
  meta_tree_remove (info->tree, src_path);

  tree_info_schedule_writeout (info);
  return TRUE;
}

static gboolean
register_name (DBusConnection *conn,
	       gboolean replace)
{
  DBusError error;
  unsigned int flags;
  int ret;

  flags = DBUS_NAME_FLAG_ALLOW_REPLACEMENT | DBUS_NAME_FLAG_DO_NOT_QUEUE;
  if (replace)
    flags |= DBUS_NAME_FLAG_REPLACE_EXISTING;

  dbus_error_init (&error);
  ret = dbus_bus_request_name (conn, G_VFS_DBUS_METADATA_NAME, flags, &error);
  if (ret == -1)
    {
      g_printerr ("Failed to acquire daemon name: %s", error.message);
      dbus_error_free (&error);
      return FALSE;
    }
  else if (ret == DBUS_REQUEST_NAME_REPLY_EXISTS)
    {
      g_printerr ("Metadata daemon already running, exiting.\n");
      return FALSE;
    }
  else if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
    {
      g_printerr ("Not primary owner of the service, exiting.\n");
      return FALSE;
    }
  return TRUE;
}

static DBusHandlerResult
dbus_message_filter_func (DBusConnection *conn,
			  DBusMessage    *message,
			  gpointer        data)
{
  char *name;

  if (dbus_message_is_signal (message, DBUS_INTERFACE_DBUS, "NameLost"))
    {
      if (dbus_message_get_args (message, NULL,
				 DBUS_TYPE_STRING, &name,
				 DBUS_TYPE_INVALID) &&
	  strcmp (name, G_VFS_DBUS_METADATA_NAME) == 0)
	{
	  /* Someone else got the name (i.e. someone used --replace), exit */
	  exit (1);
	}
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void
metadata_unregistered (DBusConnection  *connection,
		       void            *user_data)
{
}

static DBusHandlerResult
metadata_message (DBusConnection  *connection,
		  DBusMessage     *message,
		  void            *user_data)
{
  DBusMessageIter iter;
  DBusError derror;
  DBusMessage *reply;
  char *treefile;
  char *path, *dest_path;
  const char *key;

  reply = NULL;
  dbus_message_iter_init (message, &iter);
  dbus_error_init (&derror);

  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_METADATA_INTERFACE,
				   G_VFS_DBUS_METADATA_OP_SET))
    {
      treefile = NULL;
      path = NULL;
      if (!_g_dbus_message_iter_get_args (&iter, &derror,
					  G_DBUS_TYPE_CSTRING, &treefile,
					  G_DBUS_TYPE_CSTRING, &path,
					  0) ||
	  !metadata_set (treefile, path, &iter, &derror))
	{
	  reply = dbus_message_new_error (message,
					  derror.name,
					  derror.message);
	  dbus_error_free (&derror);
	}
      else
	reply = dbus_message_new_method_return (message);

      g_free (treefile);
      g_free (path);
    }

  else if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_METADATA_INTERFACE,
				   G_VFS_DBUS_METADATA_OP_UNSET))
    {
      treefile = NULL;
      path = NULL;
      if (!_g_dbus_message_iter_get_args (&iter, &derror,
					  G_DBUS_TYPE_CSTRING, &treefile,
					  G_DBUS_TYPE_CSTRING, &path,
					  DBUS_TYPE_STRING, &key,
					  0) ||
	  !metadata_unset (treefile, path, key, &derror))
	{
	  reply = dbus_message_new_error (message,
					  derror.name,
					  derror.message);
	  dbus_error_free (&derror);
	}
      else
	reply = dbus_message_new_method_return (message);

      g_free (treefile);
      g_free (path);
    }

  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_METADATA_INTERFACE,
				   G_VFS_DBUS_METADATA_OP_GET))
    {
      treefile = NULL;
      path = NULL;
      if (!_g_dbus_message_iter_get_args (&iter, &derror,
					  G_DBUS_TYPE_CSTRING, &treefile,
					  G_DBUS_TYPE_CSTRING, &path,
					  0) ||
	  (reply = metadata_get (treefile, path, message, &iter, &derror)) == NULL)
	{
	  reply = dbus_message_new_error (message,
					  derror.name,
					  derror.message);
	  dbus_error_free (&derror);
	}

      g_free (treefile);
      g_free (path);
    }

  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_METADATA_INTERFACE,
					G_VFS_DBUS_METADATA_OP_REMOVE))
    {
      treefile = NULL;
      path = NULL;
      if (!_g_dbus_message_iter_get_args (&iter, &derror,
					  G_DBUS_TYPE_CSTRING, &treefile,
					  G_DBUS_TYPE_CSTRING, &path,
					  0) ||
	  !metadata_remove (treefile, path, &derror))
	{
	  reply = dbus_message_new_error (message,
					  derror.name,
					  derror.message);
	  dbus_error_free (&derror);
	}
      else
	reply = dbus_message_new_method_return (message);

      g_free (treefile);
      g_free (path);
    }

  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_METADATA_INTERFACE,
					G_VFS_DBUS_METADATA_OP_MOVE))
    {
      treefile = NULL;
      path = NULL;
      dest_path = NULL;
      if (!_g_dbus_message_iter_get_args (&iter, &derror,
					  G_DBUS_TYPE_CSTRING, &treefile,
					  G_DBUS_TYPE_CSTRING, &path,
					  G_DBUS_TYPE_CSTRING, &dest_path,
					  0) ||
	  !metadata_move (treefile, path, dest_path, &derror))
	{
	  reply = dbus_message_new_error (message,
					  derror.name,
					  derror.message);
	  dbus_error_free (&derror);
	}
      else
	reply = dbus_message_new_method_return (message);

      g_free (treefile);
      g_free (path);
      g_free (dest_path);
    }

  if (reply)
    {
      dbus_connection_send (connection, reply, NULL);
      dbus_message_unref (reply);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
  else
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static struct DBusObjectPathVTable metadata_dbus_vtable = {
  metadata_unregistered,
  metadata_message
};

int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  DBusConnection *conn;
  gboolean replace;
  GError *error;
  DBusError derror;
  GOptionContext *context;
  const GOptionEntry options[] = {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace,  N_("Replace old daemon."), NULL },
    { NULL }
  };

  setlocale (LC_ALL, "");

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  g_set_application_name (_("GVFS Metadata Daemon"));
  context = g_option_context_new ("");

  g_option_context_set_summary (context, _("Metadata daemon for GVFS"));

  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);

  replace = FALSE;

  error = NULL;
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      /* Translators: the first %s is the application name, */
      /* the second %s is the error message                 */
      g_printerr (_("%s: %s"), g_get_application_name(), error->message);
      g_printerr ("\n");
      g_printerr (_("Try \"%s --help\" for more information."),
		  g_get_prgname ());
      g_printerr ("\n");
      g_error_free (error);
      g_option_context_free (context);
      return 1;
    }

  g_option_context_free (context);

  g_type_init ();

  loop = g_main_loop_new (NULL, FALSE);

  dbus_error_init (&derror);
  conn = dbus_bus_get (DBUS_BUS_SESSION, &derror);
  if (!conn)
    {
      g_printerr ("Failed to connect to the D-BUS daemon: %s\n",
		  derror.message);

      dbus_error_free (&derror);
      return 1;
    }

  dbus_bus_add_match (conn,
		      "type='signal',"
		      "interface='org.freedesktop.DBus',"
		      "member='NameOwnerChanged',"
		      "arg0='"G_VFS_DBUS_METADATA_NAME"'",
		      &derror);
  if (dbus_error_is_set (&derror))
    {
      g_printerr ("Failed to add dbus match: %s\n", derror.message);
      dbus_error_free (&derror);
      return 1;
    }

  if (!dbus_connection_add_filter (conn,
				   dbus_message_filter_func, NULL, NULL))
    {
      g_printerr ("Failed to add dbus filter\n");
      return 1;
    }

  if (!dbus_connection_register_object_path (conn,
					     G_VFS_DBUS_METADATA_PATH,
					     &metadata_dbus_vtable, NULL))
    {
      g_printerr ("Failed to register object path\n");
      return 1;
    }

  if (!register_name (conn, replace))
    return 1;

  _g_dbus_connection_integrate_with_main (conn);

  tree_infos = g_hash_table_new_full (g_str_hash,
				      g_str_equal,
				      NULL,
				      (GDestroyNotify)tree_info_free);

  g_main_loop_run (loop);

  return 0;
}
