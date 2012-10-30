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
#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#define BENCHMARK_UNIT_NAME "gvfs-big-file"

#include "benchmark-common.c"

#define FILE_SIZE      4096
#define BUFFER_SIZE    4096
#define ITERATIONS_NUM 65536

static gboolean
is_dir (GFile *file)
{
  GFileInfo *info;
  gboolean res;
  
  info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_TYPE, 0, NULL, NULL);
  res = info && g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
  if (info)
    g_object_unref (info);
  return res;
}

static GFile *
create_file (GFile *base_dir)
{
  GFile         *scratch_file;
  gchar         *scratch_name;
  GOutputStream *output_stream;
  gint           pid;
  GError        *error = NULL;
  gchar          buffer [BUFFER_SIZE];
  gint           i;

  pid = getpid ();
  scratch_name = g_strdup_printf ("gvfs-benchmark-scratch-%d", pid);
  scratch_file = g_file_resolve_relative_path (base_dir, scratch_name);
  g_free (scratch_name);

  if (!scratch_file)
    return NULL;

  output_stream = G_OUTPUT_STREAM (g_file_replace (scratch_file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &error));
  if (!output_stream)
    {
      g_printerr ("Failed to create scratch file: %s\n", error->message);
      g_object_unref (scratch_file);
      return NULL;
    }

  memset (buffer, 0xaa, BUFFER_SIZE);

  for (i = 0; i < FILE_SIZE; i += BUFFER_SIZE)
    {
      if (g_output_stream_write (output_stream, buffer, BUFFER_SIZE, NULL, &error) < BUFFER_SIZE)
        {
          g_printerr ("Failed to populate scratch file: %s\n", error->message);
          g_output_stream_close (output_stream, NULL, NULL);
          g_object_unref (output_stream);
          g_object_unref (scratch_file);
          return NULL;
        }
    }

  g_output_stream_close (output_stream, NULL, NULL);
  g_object_unref (output_stream);
  return scratch_file;
}

static void
read_file (GFile *scratch_file)
{
  GInputStream *input_stream;
  GError       *error = NULL;
  gint          i;

  input_stream = (GInputStream *) g_file_read (scratch_file, NULL, &error);
  if (!input_stream)
    {
      g_printerr ("Failed to open scratch file: %s\n", error->message);
      return;
    }

  for (i = 0; i < FILE_SIZE; i += BUFFER_SIZE)
    {
      gchar buffer [BUFFER_SIZE];
      gsize bytes_read;

      if (!g_input_stream_read_all (input_stream, buffer, BUFFER_SIZE, &bytes_read, NULL, &error) ||
          bytes_read < BUFFER_SIZE)
        {
          g_printerr ("Failed to read back scratch file: %s\n", error->message);
          g_input_stream_close (input_stream, NULL, NULL);
          g_object_unref (input_stream);
        }
    }

  g_object_unref (input_stream);
}

static void
delete_file (GFile *scratch_file)
{
#if 0
  GError *error = NULL;

  /* Enable when GDaemonFile supports delete */

  if (!g_file_delete (scratch_file, NULL, &error))
    {
      g_printerr ("Failed to delete scratch file: %s\n", error->message);
    }
#endif
}

static gint
benchmark_run (gint argc, gchar *argv [])
{
  GFile *base_dir;
  GFile *scratch_file;
  gint   i;
  
  setlocale (LC_ALL, "");

  if (argc < 2)
    {
      g_printerr ("Usage: %s <scratch URI>\n", argv [0]);
      return 1;
    }

  base_dir = g_file_new_for_commandline_arg (argv [1]);

  if (!is_dir (base_dir))
    {
      g_printerr ("Scratch URI %s is not a directory\n", argv [1]);
      g_object_unref (base_dir);
      return 1;
    }

  for (i = 0; i < ITERATIONS_NUM; i++)
    {
      scratch_file = create_file (base_dir);
      if (!scratch_file)
        {
          g_object_unref (base_dir);
          return 1;
        }

      read_file (scratch_file);
      delete_file (scratch_file);

      g_object_unref (scratch_file);
    }

  g_object_unref (base_dir);
  return 0;
}
