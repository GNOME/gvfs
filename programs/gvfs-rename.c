/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2008 Christian Kellner <gicmo@gnome.org>
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
 * Author: Christian Kellner <gicmo@gnome.org>
 */

#include <config.h>

#include <glib.h>
#include <locale.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

static void
show_help (GOptionContext *context, const char *error)
{
  char *help;

  if (error)
    g_printerr (_("Error: %s"), error);

  help = g_option_context_get_help (context, TRUE, NULL);
  g_printerr ("%s", help);
  g_free (help);
  g_option_context_free (context);
}

int
main (int argc, char *argv[])
{
  GOptionContext *context;
  GError         *error;
  GFile          *file;
  GFile          *new_file;
  int             retval = 0;
  gchar          *param;
  gchar          *summary;

  setlocale (LC_ALL, "");

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  error = NULL;
  param = g_strdup_printf ("%s %s", _("LOCATION"), _("NEW-NAME"));
  summary = _("Rename a file.");

  context = g_option_context_new (param);
  g_option_context_set_summary (context, summary);
  g_option_context_parse (context, &argc, &argv, &error);

  if (error != NULL)
    {
      g_printerr (_("Error parsing commandline options: %s\n"), error->message);
      g_printerr ("\n");
      g_printerr (_("Try \"%s --help\" for more information."), g_get_prgname ());
      g_printerr ("\n");
      g_error_free (error);
      return 1;
    }

  if (argc < 3)
    {
      show_help (context, _("Missing operand\n"));
      return 1;
    }

  g_option_context_free (context);
  g_free (param);

  file = g_file_new_for_commandline_arg (argv[1]);

  new_file = g_file_set_display_name (file, argv[2],
				      NULL, &error);

  if (new_file == NULL)
    {
      g_printerr (_("Error: %s\n"), error->message);
      g_error_free (error);
      retval = 1;
    }
  else
    {
      char *uri = g_file_get_uri (new_file);
      g_print (_("Rename successful. New uri: %s\n"), uri);
      g_object_unref (new_file);
      g_free (uri);
    }

  g_object_unref (file);
  return retval;
}
