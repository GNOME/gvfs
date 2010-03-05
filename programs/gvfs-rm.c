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
#include <locale.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

static gboolean force = FALSE;

static GOptionEntry entries[] =
{
  {"force", 'f', 0, G_OPTION_ARG_NONE, &force,
   N_("ignore nonexistent files, never prompt"), NULL},
  { NULL }
};


int
main (int argc, char *argv[])
{
  GError *error;
  GOptionContext *context;
  GFile *file;
  int retval = 0;

  setlocale (LC_ALL, "");

  g_type_init ();

  error = NULL;
  context = g_option_context_new (_("- delete files"));
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);
  g_option_context_free (context);

  if (argc > 1)
    {
      int i;

      for (i = 1; i < argc; i++) {
	file = g_file_new_for_commandline_arg (argv[i]);
	error = NULL;
	if (!g_file_delete (file, NULL, &error))
	  {
	    if (!force ||
		!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
	      {
	        g_printerr ("Error deleting file: %s\n", error->message);
	        retval = 1;
	      }
	    g_error_free (error);
	  }
	g_object_unref (file);
      }
    }

  return retval;
}
