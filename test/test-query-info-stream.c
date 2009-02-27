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
#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <gio/gio.h>

/* Fill test data with 0..200, repeadedly.
 * This is not a power of two to avoid possible
 * effects with base-2 i/o buffer sizes that could
 * hide bugs */
#define DATA_MODULO 200

static GMainLoop *main_loop;

static gboolean
verify_block (guchar *data, guchar *start, gsize size)
{
  guchar d;
  gsize i;

  d = 0;
  if (start)
    d = *start;
  for (i = 0; i < size; i++)
    {
      if (data[i] != d)
	return FALSE;
      
      d++;
      if (d >= DATA_MODULO)
	d = 0;
    }

  if (start)
    *start = d;
  
  return TRUE;
}

static guchar *
allocate_block (gsize size)
{
  guchar *data;
  gsize i;
  guchar d;

  data = g_malloc (size);
  d = 0;
  for (i = 0; i < size; i++)
    {
      data[i] = d;
      d++;
      if (d >= DATA_MODULO)
	d = 0;
    }
  return data;
}

static void
check_query_info_res (GFileInfo *info,
		      GError *error,
		      gsize expected_size)
{
  goffset file_size;

  if (info == NULL)
    {
      g_print ("error querying info: %s\n", error->message);
      exit (1);
    }

  if (!g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
    {
      g_print ("couldn't read size attribute\n");
      exit (1);
    }
  
  file_size = g_file_info_get_size (info);
  if (file_size != expected_size)
    {
      g_print ("wrong file size\n");
      exit (1);
    }
}

static void
check_query_info_out (GFileOutputStream *out, gsize expected_size)
{
  GFileInfo *info;
  GError *error;

  error = NULL;
  info = g_file_output_stream_query_info (out, "*", NULL, &error);

  check_query_info_res (info, error, expected_size);
}

static void
create_file (GFile *file, gsize size)
{
  GFileOutputStream *out;
  guchar *data;
  gsize written;
  GError *error;

  data = allocate_block (size);

  error = NULL;
  out = g_file_replace (file, NULL, FALSE, 0, NULL, &error);
  if (out == NULL)
    {
      g_print ("error creating file: %s\n", error->message);
      exit (1);
    }
  
  check_query_info_out (out, 0);
  
  if (!g_output_stream_write_all (G_OUTPUT_STREAM (out),
				  data, size, 
				  &written,
				  NULL, &error))
    {
      g_print ("error writing to file: %s\n", error->message);
      exit (1);
    }

  check_query_info_out (out, written);
  
  if (written != size)
    {
      g_print ("not all data written to file\n");
      exit (1);
    }

  g_output_stream_close (G_OUTPUT_STREAM (out), NULL, NULL);
  
  g_free (data);
}

static void
check_query_info (GFileInputStream *in, gsize expected_size)
{
  GFileInfo *info;
  GError *error;

  error = NULL;
  info = g_file_input_stream_query_info (in, "*", NULL, &error);

  check_query_info_res (info, error, expected_size);
}

static void
async_cb (GObject *source_object,
	  GAsyncResult *res,
	  gpointer user_data)
{
  GFileInfo *info;
  GError *error;

  error = NULL;
  info =
    g_file_input_stream_query_info_finish (G_FILE_INPUT_STREAM (source_object),
					   res, &error);

  check_query_info_res (info, error, 100*1000);
  
  g_main_loop_quit (main_loop);
}

int
main (int argc, char *argv[])
{
  GFile *file;
  GFileInputStream *in;
  GError *error;
  gssize res;
  gsize read_size;
  guchar *buffer;
  guchar start;
  gboolean do_create_file;
  
  g_type_init ();

  do_create_file = FALSE;
  
  if (argc > 1)
    {
      if (strcmp(argv[1], "-c") == 0)
	{
	  do_create_file = TRUE;
	  argc--;
	  argv++;
	}
    }
      
  if (argc != 2)
    {
      g_print ("need file arg");
      return 1;
    }

  file = g_file_new_for_commandline_arg (argv[1]);

  if (do_create_file)
    create_file (file, 100*1000);

  error = NULL;
  
  in = g_file_read (file, NULL, &error);
  if (in == NULL)
    {
      g_print ("error reading file: %s\n", error->message);
      exit (1);
    }

  check_query_info (in, 100*1000);

  buffer = malloc (100*1000);

  start = 0;
  read_size = 0;
  do
    {
      res = g_input_stream_read  (G_INPUT_STREAM (in),
				  buffer,
				  150,
				  NULL, &error);
      if (res == 0)
	break;
      
      if (res < 0)
	{
	  g_print ("error reading: %s\n", error->message);
	  exit (1);
	}

      if (!verify_block (buffer, &start, res))
	{
	  g_print ("error in block starting at %d\n", (int)read_size);
	  exit (1);
	}

      read_size += res;

      check_query_info (in, 100*1000);
    }
  while (1);

  if (read_size != 100*1000)
    {
      g_print ("Didn't read entire file\n");
      exit (1);
    }

  main_loop = g_main_loop_new (NULL, FALSE);
  
  g_file_input_stream_query_info_async  (in, "*",
					 0, NULL,
					 async_cb, NULL);
  
  g_main_loop_run (main_loop);

  g_print ("ALL OK\n");
  return 0;
}
