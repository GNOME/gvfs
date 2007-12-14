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
#include <gio/gio.h>

static GOptionEntry entries[] = 
{
	{ NULL }
};

static void
cat (GFile *file)
{
  GInputStream *in;
  char buffer[1024*8 + 1];
  char *p;
  gssize res;
  gboolean close_res;
  GError *error;
  
  error = NULL;
  in = (GInputStream *)g_file_read (file, NULL, &error);
  if (in == NULL)
    {
      g_printerr ("Error opening file: %s\n", error->message);
      g_error_free (error);
      return;
    }

  while (1)
    {
      res = g_input_stream_read (in, buffer, sizeof (buffer) - 1, NULL, &error);
      if (res > 0)
	{
	  ssize_t written;

	  p = buffer;
	  while (res > 0)
	    {
	      written = write (STDOUT_FILENO, p, res);
	      
	      if (written == -1 && errno != EINTR)
		{
		  perror ("Error writing to stdout");
		  goto out;
		}
	      res -= written;
	      p += written;
	    }
	}
      else if (res < 0)
	{
	  g_printerr ("Error reading: %s\n", error->message);
	  g_error_free (error);
	  error = NULL;
	  break;
	}
      else if (res == 0)
	break;
    }

 out:
  
  close_res = g_input_stream_close (in, NULL, &error);
  if (!close_res)
    {
      g_printerr ("Error closing: %s\n", error->message);
      g_error_free (error);
    }
}

int
main (int argc, char *argv[])
{
  GError *error;
  GOptionContext *context;
  GFile *file;
  
  setlocale (LC_ALL, "");

  g_type_init ();
  
  error = NULL;
  context = g_option_context_new ("- output files at <location>");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);
  g_option_context_free (context);
  
  if (argc > 1)
    {
      int i;
      
      for (i = 1; i < argc; i++) {
	file = g_file_new_for_commandline_arg (argv[i]);
	cat (file);
	g_object_unref (file);
      }
    }

  return 0;
}
