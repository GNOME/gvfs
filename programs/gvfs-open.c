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

#include <stdio.h>
#include <unistd.h>
#include <locale.h>
#include <errno.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

static gboolean show_version = FALSE;
static gchar **locations = NULL;

static GOptionEntry entries[] = {
  { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, N_("Show program version"), NULL },
  {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &locations, NULL, NULL },
  {NULL}
};

int
main (int argc, char *argv[])
{
  GError *error = NULL;
  GOptionContext *context = NULL;
  gchar *param;
  gchar *summary;
  int i;
  gboolean success;
  gboolean res;
  GFile *file;

  setlocale (LC_ALL, "");

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  param = g_strdup_printf ("%s...", _("FILE"));
  /* Translators: this message will appear after the usage string */
  /* and before the list of options.                              */
  summary = _("Open files with the default application that\n"
              "is registered to handle files of this type.");

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

  if (!locations)
    {
      /* Translators: the %s is the program name. This error message */
      /* means the user is calling gvfs-cat without any argument.    */
      g_printerr (_("%s: missing locations"), g_get_prgname ());
      g_printerr ("\n");
      g_printerr (_("Try \"%s --help\" for more information."),
		  g_get_prgname ());
      g_printerr ("\n");
      return 1;
    }

  i = 0;
  success = TRUE;

  do
    {
      res = g_app_info_launch_default_for_uri (locations[i],
					       NULL,
					       &error);

      if (!res && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
	{
	  /* g_app_info_launch_default_for_uri() can't properly handle non-URI (local) locations */
	  g_clear_error (&error);
	  file = g_file_new_for_commandline_arg (locations[i]);
	  res = g_app_info_launch_default_for_uri (g_file_get_uri (file),
						   NULL,
						   &error);
	  g_object_unref (file);
	}

      if (!res)
	{
	  /* Translators: the first %s is the program name, the second one  */
	  /* is the URI of the file, the third is the error message.        */
	  g_printerr (_("%s: %s: error opening location: %s\n"),
		      g_get_prgname (), locations[i], error->message);
	  g_clear_error (&error);
	  success = FALSE;
	}
    }
  while (locations[++i] != NULL);

  return success ? 0 : 2;
}
