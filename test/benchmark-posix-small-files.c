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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <glib.h>
#include <gio/gio.h>

#define BENCHMARK_UNIT_NAME "posix-big-file"

#include "benchmark-common.c"

#define FILE_SIZE      4096
#define BUFFER_SIZE    4096
#define ITERATIONS_NUM 65536

static gboolean
is_dir (const gchar *dir)
{
  struct stat sbuf;

  if (stat (dir, &sbuf) < 0)
    return FALSE;

  if (S_ISDIR (sbuf.st_mode))
    return TRUE;

  return FALSE;
}

static gchar *
create_file (const gchar *base_dir)
{
  gchar         *scratch_file;
  gint           output_fd;
  gint           pid;
  gchar          buffer [BUFFER_SIZE];
  gint           i;

  pid = getpid ();
  scratch_file = g_strdup_printf ("%s/posix-benchmark-scratch-%d", base_dir, pid);

  output_fd = open (scratch_file, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if (output_fd < 0)
    {
      g_printerr ("Failed to create scratch file: %s\n", g_strerror (errno));
      g_free (scratch_file);
      return NULL;
    }

  memset (buffer, 0xaa, BUFFER_SIZE);

  for (i = 0; i < FILE_SIZE; i += BUFFER_SIZE)
    {
      gint bytes_written;

      bytes_written = write (output_fd, buffer, BUFFER_SIZE);
      if (bytes_written < BUFFER_SIZE)
        {
          if (errno == EINTR)
            {
              i -= BUFFER_SIZE - bytes_written;
              continue;
            }

          g_printerr ("Failed to populate scratch file: %s\n", g_strerror (errno));
          close (output_fd);
          g_free (scratch_file);
          return NULL;
        }
    }

  close (output_fd);
  return scratch_file;
}

static void
read_file (const gchar *scratch_file)
{
  gint          input_fd;
  gint          i;

  input_fd = open (scratch_file, O_RDONLY);
  if (input_fd < 0)
    {
      g_printerr ("Failed to read back scratch file: %s\n", g_strerror (errno));
      return;
    }

  for (i = 0; i < FILE_SIZE; i += BUFFER_SIZE)
    {
      gchar buffer [BUFFER_SIZE];
      gsize bytes_read;

      bytes_read = read (input_fd, buffer, BUFFER_SIZE);
      if (bytes_read < BUFFER_SIZE)
        {
          if (errno == EINTR)
            {
              i -= BUFFER_SIZE - bytes_read;
              continue;
            }

          g_printerr ("Failed to read back scratch file: %s\n", g_strerror (errno));
          close (input_fd);
          return;
        }
    }

  close (input_fd);
}

static void
delete_file (const gchar *scratch_file)
{
  if (unlink (scratch_file) < 0)
    {
      g_printerr ("Failed to delete scratch file: %s\n", g_strerror (errno));
    }
}

static gint
benchmark_run (gint argc, gchar *argv [])
{
  gchar *base_dir;
  gchar *scratch_file;
  gint   i;
  
  setlocale (LC_ALL, "");

  if (argc < 2)
    {
      g_printerr ("Usage: %s <scratch path>\n", argv [0]);
      return 1;
    }

  base_dir = argv [1];

  if (!is_dir (base_dir))
    {
      g_printerr ("Scratch path %s is not a directory\n", argv [1]);
      return 1;
    }

  for (i = 0; i < ITERATIONS_NUM; i++)
    {
      scratch_file = create_file (base_dir);
      if (!scratch_file)
        {
          g_free (base_dir);
          return 1;
        }

      read_file (scratch_file);
      delete_file (scratch_file);

      g_free (scratch_file);
    }

  return 0;
}
