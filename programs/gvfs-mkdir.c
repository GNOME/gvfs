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
#include <locale.h>
#include <gio/gio.h>

static gboolean parent = FALSE;
static gboolean show_version = FALSE;

static GOptionEntry entries[] =
{
  { "parent", 'p', 0, G_OPTION_ARG_NONE, &parent, N_("Create parent directories"), NULL },
  { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, N_("Show program version"), NULL },
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
  summary = _("Create directories.");

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

  if (show_version)
    {
      g_print (PACKAGE_STRING "\n");
      return 0;
    }

  if (argc > 1)
    {
      int i;

      for (i = 1; i < argc; i++)
	{
	  file = g_file_new_for_commandline_arg (argv[i]);
	  error = NULL;
	  if (parent)
	    {
	       if (!g_file_make_directory_with_parents (file, NULL, &error))
		{
		  g_printerr (_("Error creating directory: %s\n"), error->message);
		  g_error_free (error);
		  retval = 1;
		}
	    }
	  else
	    {
	      if (!g_file_make_directory (file, NULL, &error))
		{
		  g_printerr (_("Error creating directory: %s\n"), error->message);
		  g_error_free (error);
		  retval = 1;
		}
	      g_object_unref (file);
	    }
	}
    }

  return retval;
}
