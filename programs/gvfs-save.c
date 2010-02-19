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

static char *etag = NULL;
static gboolean backup = FALSE;
static gboolean create = FALSE;
static gboolean append = FALSE;
static gboolean priv = FALSE;
static gboolean print_etag = FALSE;

static GOptionEntry entries[] =
{
  { "backup", 'b', 0, G_OPTION_ARG_NONE, &backup, N_("Create backup"), NULL },
  { "create", 'c', 0, G_OPTION_ARG_NONE, &create, N_("Only create if not existing"), NULL },
  { "append", 'a', 0, G_OPTION_ARG_NONE, &append, N_("Append to end of file"), NULL },
  { "private", 'p', 0, G_OPTION_ARG_NONE, &priv, N_("When creating a file, restrict access to the current user only"), NULL },
  { "print_etag", 'v', 0, G_OPTION_ARG_NONE, &print_etag, N_("Print new etag at end"), NULL },
  { "etag", 'e', 0, G_OPTION_ARG_STRING, &etag, N_("The etag of the file being overwritten"), NULL },
  { NULL }
};

static gboolean
save (GFile *file)
{
  GOutputStream *out;
  GFileCreateFlags flags;
  char buffer[1025];
  char *p;
  gssize res;
  gboolean close_res;
  GError *error;
  gboolean save_res;

  error = NULL;

  flags = priv ? G_FILE_CREATE_PRIVATE : G_FILE_CREATE_NONE;

  if (create)
    out = (GOutputStream *)g_file_create (file, flags, NULL, &error);
  else if (append)
    out = (GOutputStream *)g_file_append_to  (file, flags, NULL, &error);
  else
    out = (GOutputStream *)g_file_replace  (file, etag, backup, flags, NULL, &error);
  if (out == NULL)
    {
      g_printerr (_("Error opening file: %s\n"), error->message);
      g_error_free (error);
      return FALSE;
    }

  save_res = TRUE;

  while (1)
    {
      res = read (STDIN_FILENO, buffer, 1024);
      if (res > 0)
	{
	  ssize_t written;

	  p = buffer;
	  while (res > 0)
	    {
	      error = NULL;
	      written = g_output_stream_write (out, p, res, NULL, &error);
	      if (written == -1)
		{
		  save_res = FALSE;
		  g_printerr ("Error writing to stream: %s", error->message);
		  g_error_free (error);
		  goto out;
		}
	      res -= written;
	      p += written;
	    }
	}
      else if (res < 0)
	{
	  save_res = FALSE;
	  perror (_("Error reading stdin"));
	  break;
	}
      else if (res == 0)
	break;
    }

 out:

  close_res = g_output_stream_close (out, NULL, &error);
  if (!close_res)
    {
      save_res = FALSE;
      g_printerr (_("Error closing: %s\n"), error->message);
      g_error_free (error);
    }

  if (close_res && print_etag)
    {
      char *etag;
      etag = g_file_output_stream_get_etag (G_FILE_OUTPUT_STREAM (out));

      if (etag)
	g_print ("Etag: %s\n", etag);
      else
	g_print (_("Etag not available\n"));
      g_free (etag);
    }

  g_object_unref (out);

  return save_res;
}

int
main (int argc, char *argv[])
{
  GError *error;
  GOptionContext *context;
  GFile *file;
  gboolean res;

  setlocale (LC_ALL, "");

  g_type_init ();

  error = NULL;
  context = g_option_context_new (_("DEST - read from standard input and save to DEST"));
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

  res = FALSE;

  if (argc > 1)
    {
      file = g_file_new_for_commandline_arg (argv[1]);
      res = save (file);
      g_object_unref (file);
    }

  if (res)
    return 0;
  return 1;
}
