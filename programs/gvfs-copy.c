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

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

static gboolean progress = FALSE;
static gboolean interactive = FALSE;
static gboolean no_dereference = FALSE;
static gboolean backup = FALSE;
static gboolean preserve = FALSE;
static gboolean no_target_directory = FALSE;

static GOptionEntry entries[] =
{
  { "no-target-directory", 'T', 0, G_OPTION_ARG_NONE, &no_target_directory, N_("no target directory"), NULL },
  { "progress", 'p', 0, G_OPTION_ARG_NONE, &progress, N_("show progress"), NULL },
  { "interactive", 'i', 0, G_OPTION_ARG_NONE, &interactive, N_("prompt before overwrite"), NULL },
  { "preserve", 'p', 0, G_OPTION_ARG_NONE, &preserve, N_("preserve all attributes"), NULL },
  { "backup", 'b', 0, G_OPTION_ARG_NONE, &backup, N_("backup existing destination files"), NULL },
  { "no-dereference", 'P', 0, G_OPTION_ARG_NONE, &no_dereference, N_("never follow symbolic links"), NULL },
  { NULL }
};

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

static GTimeVal start_time;
static void
show_progress (goffset current_num_bytes,
	       goffset total_num_bytes,
	       gpointer user_data)
{
  GTimeVal tv;
  char *size;

  g_get_current_time (&tv);

  size = g_format_size_for_display (current_num_bytes / MAX (tv.tv_sec - start_time.tv_sec, 1));
  g_print (_("progress"));
  g_print (" %"G_GINT64_FORMAT"/%"G_GINT64_FORMAT" (%s/s)\n",
	   current_num_bytes, total_num_bytes, size);
  g_free (size);
}

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
  GError *error;
  GOptionContext *context;
  GFile *source, *dest, *target;
  gboolean dest_is_dir;
  char *basename;
  int i;
  GFileCopyFlags flags;
  int retval = 0;

  setlocale (LC_ALL, "");

  g_type_init ();

  error = NULL;
  context = g_option_context_new (_("SOURCE... DEST - copy file(s) from SOURCE to DEST"));
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);

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

  if (argc <= 2)
    {
      show_help (context, _("Missing operand\n"));
      return 1;
    }

  dest = g_file_new_for_commandline_arg (argv[argc-1]);

  if (no_target_directory && argc > 3)
    {
      show_help (context, _("Too many arguments\n"));
      g_object_unref (dest);
      return 1;
    }

  dest_is_dir = is_dir (dest);

  if (!dest_is_dir && argc > 3)
    {
      g_printerr (_("Target %s is not a directory\n"), argv[argc-1]);
      show_help (context, NULL);
      g_object_unref (dest);
      return 1;
    }

  g_option_context_free (context);

  for (i = 1; i < argc - 1; i++)
    {
      source = g_file_new_for_commandline_arg (argv[i]);

      if (dest_is_dir && !no_target_directory)
	{
	  basename = g_file_get_basename (source);
	  target = g_file_get_child (dest, basename);
	  g_free (basename);
	}
      else
	target = g_object_ref (dest);

      flags = 0;
      if (backup)
	flags |= G_FILE_COPY_BACKUP;
      if (!interactive)
	flags |= G_FILE_COPY_OVERWRITE;
      if (no_dereference)
	flags |= G_FILE_COPY_NOFOLLOW_SYMLINKS;
      if (preserve)
	flags |= G_FILE_COPY_ALL_METADATA;


      error = NULL;
      g_get_current_time (&start_time);
      if (!g_file_copy (source, target, flags, NULL, progress?show_progress:NULL, NULL, &error))
	{
	  if (interactive && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
	    {
	      char line[16];

	      g_error_free (error);
	      error = NULL;

	      basename = g_file_get_basename (target);
	      g_print (_("overwrite %s?"), basename);
	      g_free (basename);

	      if (fgets(line, sizeof (line), stdin) &&
		  line[0] == 'y')
		{
		  flags |= G_FILE_COPY_OVERWRITE;
		  if (!g_file_copy (source, target, flags, NULL, NULL, NULL, &error))
		    goto copy_failed;
		}
	    }
	  else
	    {
	    copy_failed:
	      g_printerr (_("Error copying file %s: %s\n"), argv[i], error->message);
	      g_error_free (error);
	      retval = 1;
	    }
	}

      g_object_unref (source);
      g_object_unref (target);
    }

  g_object_unref (dest);

  return retval;
}
