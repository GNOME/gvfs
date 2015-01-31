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

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <stdlib.h>
#include "metatree.h"
#include "gvfsdaemonprotocol.h"
#include "metadata-dbus.h"

#define WRITEOUT_TIMEOUT_SECS 60
#define WRITEOUT_TIMEOUT_SECS_NFS 15

typedef struct {
  char *filename;
  MetaTree *tree;
  guint writeout_timeout;
} TreeInfo;

static GHashTable *tree_infos = NULL;
static GVfsMetadata *skeleton = NULL;

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
  gboolean on_nfs;

  if (info->writeout_timeout == 0)
    {
      on_nfs = meta_tree_is_on_nfs (info->tree);
      info->writeout_timeout =
        g_timeout_add_seconds (on_nfs ? WRITEOUT_TIMEOUT_SECS_NFS : WRITEOUT_TIMEOUT_SECS,
			       writeout_timeout, info);
    }
}

static void
flush_single (const gchar *filename,
              TreeInfo *info,
              gpointer user_data)
{
  if (info->writeout_timeout != 0)
    {
      g_source_remove (info->writeout_timeout);
      writeout_timeout (info);
    }
}

static void
flush_all ()
{
  g_hash_table_foreach (tree_infos, (GHFunc) flush_single, NULL);
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
handle_set (GVfsMetadata *object,
            GDBusMethodInvocation *invocation,
            const gchar *arg_treefile,
            const gchar *arg_path,
            GVariant *arg_data,
            GVfsMetadata *daemon)
{
  TreeInfo *info;
  const gchar *str;
  const gchar **strv;
  const gchar *key;
  GError *error;
  GVariantIter iter;
  GVariant *value;

  info = tree_info_lookup (arg_treefile);
  if (info == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_NOT_FOUND,
                                             _("Can't find metadata file %s"),
                                             arg_treefile);
      return TRUE;
    }

  error = NULL;

  g_variant_iter_init (&iter, arg_data);
  while (g_variant_iter_next (&iter, "{&sv}", &key, &value))
    {
      if (g_variant_is_of_type (value, G_VARIANT_TYPE_STRING_ARRAY))
	{
	  /* stringv */
          strv = g_variant_get_strv (value, NULL);
	  if (!meta_tree_set_stringv (info->tree, arg_path, key, (gchar **) strv))
	    {
	      g_set_error_literal (&error, G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                  _("Unable to set metadata key"));
	    }
	  g_free (strv);
	}
      else if (g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
	{
	  /* string */
          str = g_variant_get_string (value, NULL);
	  if (!meta_tree_set_string (info->tree, arg_path, key, str))
	    {
              g_set_error_literal (&error, G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   _("Unable to set metadata key"));
	    }
	}
      else if (g_variant_is_of_type (value, G_VARIANT_TYPE_BYTE))
	{
	  /* Unset */
	  if (!meta_tree_unset (info->tree, arg_path, key))
	    {
              g_set_error_literal (&error, G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   _("Unable to unset metadata key"));
	    }
	}
      g_variant_unref (value);
    }

  tree_info_schedule_writeout (info);

  if (error)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
    }
  else
    {
      gvfs_metadata_complete_set (object, invocation);
    }
  
  return TRUE;
}

static void
append_key (GVariantBuilder *builder,
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
      g_variant_builder_add (builder, "{sv}", key, g_variant_new_string (str));
      g_free (str);
    }
  else if (keytype == META_KEY_TYPE_STRINGV)
    {
      strv = meta_tree_lookup_stringv (tree, path, key);
      g_variant_builder_add (builder, "{sv}", key, g_variant_new_strv ((const gchar * const  *) strv, -1));
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

static gboolean
handle_get (GVfsMetadata *object,
            GDBusMethodInvocation *invocation,
            const gchar *arg_treefile,
            const gchar *arg_path,
            const gchar *const *arg_keys,
            GVfsMetadata *daemon)
{
  TreeInfo *info;
  GPtrArray *meta_keys;
  gboolean free_keys;
  gchar **iter_keys;
  gchar **i;
  GVariantBuilder *builder;

  info = tree_info_lookup (arg_treefile);
  if (info == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_NOT_FOUND,
                                             _("Can't find metadata file %s"),
                                             arg_treefile);
      return TRUE;
    }

  if (arg_keys == NULL)
    {
      /* Get all keys */
      free_keys = TRUE;
      meta_keys = g_ptr_array_new ();
      meta_tree_enumerate_keys (info->tree, arg_path, enum_keys, meta_keys);
      iter_keys = (gchar **) g_ptr_array_free (meta_keys, FALSE);
    }
  else
    {
      free_keys = FALSE;
      iter_keys = (gchar **) arg_keys;
    }

  builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);

  for (i = iter_keys; *i; i++)
    append_key (builder, info->tree, arg_path, *i);
  if (free_keys)
    g_strfreev (iter_keys);
  
  gvfs_metadata_complete_get (object, invocation,
                              g_variant_builder_end (builder));
  g_variant_builder_unref (builder);

  return TRUE;
}

static gboolean
handle_unset (GVfsMetadata *object,
              GDBusMethodInvocation *invocation,
              const gchar *arg_treefile,
              const gchar *arg_path,
              const gchar *arg_key,
              GVfsMetadata *daemon)
{
  TreeInfo *info;

  info = tree_info_lookup (arg_treefile);
  if (info == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_NOT_FOUND,
                                             _("Can't find metadata file %s"),
                                             arg_treefile);
      return TRUE;
    }

  if (!meta_tree_unset (info->tree, arg_path, arg_key))
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_IO_ERROR,
                                                     G_IO_ERROR_FAILED,
                                                     _("Unable to unset metadata key"));
      return TRUE;
    }

  tree_info_schedule_writeout (info);
  gvfs_metadata_complete_unset (object, invocation);

  return TRUE;
}

static gboolean
handle_remove (GVfsMetadata *object,
               GDBusMethodInvocation *invocation,
               const gchar *arg_treefile,
               const gchar *arg_path,
               GVfsMetadata *daemon)
{
  TreeInfo *info;

  info = tree_info_lookup (arg_treefile);
  if (info == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_NOT_FOUND,
                                             _("Can't find metadata file %s"),
                                             arg_treefile);
      return TRUE;
    }

  if (!meta_tree_remove (info->tree, arg_path))
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_IO_ERROR,
                                                     G_IO_ERROR_FAILED,
                                                     _("Unable to remove metadata keys"));
      return TRUE;
    }

  tree_info_schedule_writeout (info);
  gvfs_metadata_complete_remove (object, invocation);
  
  return TRUE;
}

static gboolean
handle_move (GVfsMetadata *object,
             GDBusMethodInvocation *invocation,
             const gchar *arg_treefile,
             const gchar *arg_path,
             const gchar *arg_dest_path,
             GVfsMetadata *daemon)
{
  TreeInfo *info;

  info = tree_info_lookup (arg_treefile);
  if (info == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_IO_ERROR,
                                             G_IO_ERROR_NOT_FOUND,
                                             _("Can't find metadata file %s"),
                                             arg_treefile);
      return TRUE;
    }

  /* Overwrites any dest */
  if (!meta_tree_copy (info->tree, arg_path, arg_dest_path))
    {
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_IO_ERROR,
                                                     G_IO_ERROR_FAILED,
                                                     _("Unable to move metadata keys"));
      return TRUE;
    }

  /* Remove source if copy succeeded (ignoring errors) */
  meta_tree_remove (info->tree, arg_path);

  tree_info_schedule_writeout (info);
  gvfs_metadata_complete_move (object, invocation);
  
  return TRUE;
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  GMainLoop *loop = user_data;

  /* means that someone has claimed our name (we allow replacement) */
  flush_all ();
  g_main_loop_quit (loop);
}

static void
on_connection_closed (GDBusConnection *connection,
                      gboolean         remote_peer_vanished,
                      GError          *error,
                      gpointer         user_data)
{
  GMainLoop *loop = user_data;

  /* session bus died */
  flush_all ();
  g_main_loop_quit (loop);
}

int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  GDBusConnection *conn;
  gboolean replace;
  gboolean show_version;
  GError *error;
  guint name_owner_id;
  GBusNameOwnerFlags flags;
  GOptionContext *context;
  const GOptionEntry options[] = {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace,  N_("Replace old daemon."), NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, N_("Show program version."), NULL},
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
  show_version = FALSE;
  name_owner_id = 0;

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

  if (show_version)
    {
      g_print(PACKAGE_STRING "\n");
      return 0;
    }

  error = NULL;
  conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!conn)
    {
      g_printerr ("Failed to connect to the D-BUS daemon: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      return 1;
    }

  tree_infos = g_hash_table_new_full (g_str_hash,
				      g_str_equal,
				      NULL,
				      (GDestroyNotify)tree_info_free);

  loop = g_main_loop_new (NULL, FALSE);
  g_dbus_connection_set_exit_on_close (conn, FALSE);
  g_signal_connect (conn, "closed", G_CALLBACK (on_connection_closed), loop);

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  if (replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  skeleton = gvfs_metadata_skeleton_new ();

  g_signal_connect (skeleton, "handle-set", G_CALLBACK (handle_set), skeleton);
  g_signal_connect (skeleton, "handle-unset", G_CALLBACK (handle_unset), skeleton);
  g_signal_connect (skeleton, "handle-get", G_CALLBACK (handle_get), skeleton);
  g_signal_connect (skeleton, "handle-remove", G_CALLBACK (handle_remove), skeleton);
  g_signal_connect (skeleton, "handle-move", G_CALLBACK (handle_move), skeleton);

  error = NULL;
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (skeleton), conn,
                                         G_VFS_DBUS_METADATA_PATH, &error))
    {
      g_printerr ("Error exporting metadata daemon: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      g_object_unref (conn);
      g_main_loop_unref (loop);
      return 1;
    }

  name_owner_id = g_bus_own_name_on_connection (conn,
                                                G_VFS_DBUS_METADATA_NAME,
                                                flags,
                                                NULL,
                                                on_name_lost,
                                                loop,
                                                NULL);
  
  g_main_loop_run (loop);
  
  if (skeleton)
    g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (skeleton));
  if (name_owner_id != 0)
    g_bus_unown_name (name_owner_id);
  if (conn)
    g_object_unref (conn);
  if (loop != NULL)
    g_main_loop_unref (loop);

  return 0;
}
