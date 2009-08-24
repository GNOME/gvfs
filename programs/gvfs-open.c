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

#include <stdio.h>
#include <unistd.h>
#include <locale.h>
#include <errno.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

static gchar **locations = NULL;

static GOptionEntry entries[] = {
  {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &locations, N_("files"), NULL},
  {NULL}
};

static gboolean
is_file_uri_with_anchor (char *str)
{
  if (g_ascii_strncasecmp (str, "file:", 5) == 0 &&
      strchr (str, '#') != NULL)
    return TRUE;
  return FALSE;
}

static void
open (GFile *file, char *arg_string)
{
  GAppInfo *app;
  GError *error;
  GList l = {NULL};
  gboolean res;

  error = NULL;
  app = g_file_query_default_handler (file, NULL, &error);

  if (app == NULL)
    {
      /* Translators: the first %s is the program name, the second one  */
      /* is the URI of the file, the third is the error message.        */
      g_printerr (_("%s: %s: error opening location: %s\n"),
		  g_get_prgname (), g_file_get_uri (file), error->message);
      g_error_free (error);
      return;
    }

  if (g_file_is_native (file) && !is_file_uri_with_anchor (arg_string))
    {
      /* For normal files, pass in the canonicalized GFile as path */
      l.data = file;
      res = g_app_info_launch (app, &l,
			       NULL, &error);
    }
  else
    {
      /* However, for uris, use the original string, as it might be
	 modified by passing throught GFile (e.g. mailto: links)
      */
      l.data = arg_string;
      res = g_app_info_launch_uris (app, &l,
				    NULL, &error);
    }

  if (!res)
    {
      /* Translators: the first %s is the program name, the second one  */
      /* is the URI of the file, the third is the error message.        */
      g_printerr (_("%s: %s: error launching application: %s\n"),
		  g_get_prgname (), g_file_get_uri (file), error->message);
	  g_error_free (error);
    }

  g_object_unref (app);

  return;
}

int
main (int argc, char *argv[])
{
  GError *error = NULL;
  GOptionContext *context = NULL;
  GFile *file;
  gchar *summary;
  int i;

  setlocale (LC_ALL, "");

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  g_type_init ();

  /* Translators: this message will appear immediately after the */
  /* usage string - Usage: COMMAND [OPTION]... <THIS_MESSAGE>    */
  context =
    g_option_context_new (_("FILES... - open FILES with registered application."));

  /* Translators: this message will appear after the usage string */
  /* and before the list of options.                              */
  summary = _("Opens the file(s) with the default application "
	      "registered to handle the type of the file.");

  g_option_context_set_summary (context, summary);

  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);

  g_option_context_free (context);

  if (error != NULL)
    {
      g_printerr (_("Error parsing commandline options: %s\n"), error->message);
      g_printerr ("\n");
      g_printerr (_("Try \"%s --help\" for more information."),
		  g_get_prgname ());
      g_printerr ("\n");
      g_error_free(error);
      return 1;
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

  do
    {
      file = g_file_new_for_commandline_arg (locations[i]);
      open (file, locations[i]);
      g_object_unref (file);
    }
  while (locations[++i] != NULL);

  return 0;
}
