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
#include <locale.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

static void
delete_trash_file (GFile *file, gboolean del_file, gboolean del_children)
{
  GFileInfo *info;
  GFile *child;
  GFileEnumerator *enumerator;

  if (del_children)
    {
      enumerator = g_file_enumerate_children (file,
                                              G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                              G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                              NULL,
                                              NULL);
      if (enumerator)
        {
          while ((info = g_file_enumerator_next_file (enumerator, NULL, NULL)) != NULL)
            {
              child = g_file_get_child (file, g_file_info_get_name (info));
              delete_trash_file (child, TRUE, g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY);
              g_object_unref (child);
              g_object_unref (info);
            }
          g_file_enumerator_close (enumerator, NULL, NULL);
          g_object_unref (enumerator);
        }
    }

  if (del_file)
    g_file_delete (file, NULL, NULL);
}

static gboolean force = FALSE;
static gboolean empty = FALSE;

static GOptionEntry entries[] =
{
  { "force", 'f', 0, G_OPTION_ARG_NONE, &force, N_("Ignore nonexistent files, never prompt"), NULL },
  { "empty", 0, 0, G_OPTION_ARG_NONE, &empty, N_("Empty the trash"), NULL },
  { NULL }
};

int
main (int argc, char *argv[])
{
  GError *error;
  GOptionContext *context;
  GFile *file;
  int retval = 0;
  gchar *param;
  gchar *summary;

  setlocale (LC_ALL, "");

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  error = NULL;
  param = g_strdup_printf ("[%s...]", _("LOCATION"));
  summary = _("Move files or directories to the trash.");

  context = g_option_context_new (param);
  g_option_context_set_summary (context, summary);
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);
  g_option_context_free (context);
  g_free (param);

  if (error != NULL)
    {
      g_printerr (_("Error parsing commandline options: %s\n"), error->message);
      g_printerr ("\n");
      g_printerr (_("Try \"%s --help\" for more information."), g_get_prgname ());
      g_printerr ("\n");
      g_error_free (error);
      return 1;
    }

  if (argc > 1)
    {
      int i;

      for (i = 1; i < argc; i++) {
	file = g_file_new_for_commandline_arg (argv[i]);
	error = NULL;
	if (!g_file_trash (file, NULL, &error))
	  {
	    if (!force ||
		!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
	      {
	        g_printerr (_("Error trashing file: %s\n"), error->message);
	        retval = 1;
	      }
	    g_error_free (error);
	  }
	g_object_unref (file);
      }
    }

  if (empty)
    {
      GFile *file;
      file = g_file_new_for_uri ("trash:");
      delete_trash_file (file, FALSE, TRUE);
      g_object_unref (file);
    }

  return retval;
}
