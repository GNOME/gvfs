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

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

static gboolean show_version = FALSE;
static gchar **locations = NULL;

static GOptionEntry entries[] = {
  { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, N_("Show program version"), NULL },
  { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &locations, NULL, NULL },
  { NULL }
};

static gboolean
cat (GFile * file)
{
  GInputStream *in;
  char buffer[1024 * 8 + 1];
  char *p;
  gssize res;
  gboolean close_res;
  GError *error;
  gboolean success;

  error = NULL;
  in = (GInputStream *) g_file_read (file, NULL, &error);
  if (in == NULL)
    {
      /* Translators: the first %s is the program name, the second one  */
      /* is the URI of the file, the third is the error message.        */
      g_printerr (_("%s: %s: error opening file: %s\n"),
                  g_get_prgname (), g_file_get_uri (file), error->message);
      g_error_free (error);
      return FALSE;
    }

  success = TRUE;
  while (1)
    {
      res =
        g_input_stream_read (in, buffer, sizeof (buffer) - 1, NULL, &error);
      if (res > 0)
        {
          ssize_t written;

          p = buffer;
          while (res > 0)
            {
              written = write (STDOUT_FILENO, p, res);

              if (written == -1 && errno != EINTR)
                {
                  /* Translators: the first %s is the program name, the */
                  /* second one is the URI of the file.                 */
                  g_printerr (_("%s: %s, error writing to stdout"),
                              g_get_prgname (), g_file_get_uri (file));
                  success = FALSE;
                  goto out;
                }
              res -= written;
              p += written;
            }
        }
      else if (res < 0)
        {
          /* Translators: the first %s is the program name, the second one  */
          /* is the URI of the file, the third is the error message.        */
          g_printerr (_("%s: %s: error reading: %s\n"),
                      g_get_prgname (), g_file_get_uri (file),
                      error->message);
          g_error_free (error);
          error = NULL;
          success = FALSE;
          break;
        }
      else if (res == 0)
        break;
    }

 out:

  close_res = g_input_stream_close (in, NULL, &error);
  if (!close_res)
    {
      /* Translators: the first %s is the program name, the second one  */
      /* is the URI of the file, the third is the error message.        */
      g_printerr (_("%s: %s:error closing: %s\n"),
                  g_get_prgname (), g_file_get_uri (file), error->message);
      g_error_free (error);
      success = FALSE;
    }

  return success;
}

int
main (int argc, char *argv[])
{
  GError *error = NULL;
  GOptionContext *context = NULL;
  GFile *file;
  gchar *summary;
  gchar *description;
  int i;
  gchar *param;
  gboolean res;

  setlocale (LC_ALL, "");

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  param = g_strdup_printf (("%s..."), _("FILE"));
  /* Translators: this message will appear after the usage string */
  /* and before the list of options.                              */
  summary = _("Concatenate files and print to the standard output.");
  description = g_strconcat (_("gvfs-cat works just like the traditional cat utility, but using gvfs\n"
                               "locations instead of local files: for example you can use something\n"
                               "like smb://server/resource/file.txt as location."),
                               "\n\n",
                             _("Note: just pipe through cat if you need its formatting options\n"
                               "like -n, -T or other."),
                             NULL);

  context = g_option_context_new (param);
  g_option_context_set_summary (context, summary);
  g_option_context_set_description (context, description);

  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);

  g_option_context_free (context);
  g_free (param);
  g_free (description);

  if (error != NULL)
    {
      g_printerr (_("Error parsing commandline options: %s\n"), error->message);
      g_printerr ("\n");
      g_printerr (_("Try \"%s --help\" for more information."), g_get_prgname ());
      g_printerr ("\n");
      g_error_free (error);
      return 1;
    }

  if (show_version)
    {
      g_print (PACKAGE_STRING "\n");
      return 0;
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

  res = TRUE;
  i = 0;

  do
    {
      file = g_file_new_for_commandline_arg (locations[i]);
      res = cat (file) && res;
      g_object_unref (file);
    }
  while (locations[++i] != NULL);

  return res ? 0 : 2;
}
