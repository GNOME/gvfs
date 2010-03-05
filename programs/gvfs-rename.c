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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Christian Kellner <gicmo@gnome.org>
 */

#include <config.h>

#include <glib.h>
#include <locale.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

static GOptionEntry entries[] =
{
  { NULL }
};


int
main (int argc, char *argv[])
{
  GOptionContext *context;
  GError         *error;
  GFile          *file;
  GFile          *new_file;
  int             retval = 0;

  setlocale (LC_ALL, "");

  g_type_init ();

  error = NULL;
  context = g_option_context_new (_("- rename file"));
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);
  g_option_context_free (context);

  if (argc < 3)
    {
      g_printerr ("Usage: %s location new_name\n",
		  g_get_prgname ());
      return 1;
    }

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
