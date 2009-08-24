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
#include <string.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

static char *attributes = NULL;
static gboolean show_hidden = FALSE;
static gboolean show_long = FALSE;
static gboolean nofollow_symlinks = FALSE;
static char *show_completions = NULL;

static GOptionEntry entries[] =
{
  { "attributes", 'a', 0, G_OPTION_ARG_STRING, &attributes, N_("The attributes to get"), NULL },
  { "hidden", 'h', 0, G_OPTION_ARG_NONE, &show_hidden, N_("Show hidden files"), NULL },
  { "long", 'l', 0, G_OPTION_ARG_NONE, &show_long, N_("Use a long listing format"), NULL },
  { "show-completions", 'c', 0, G_OPTION_ARG_STRING, &show_completions, N_("Show completions"), NULL},
  { "nofollow-symlinks", 'n', 0, G_OPTION_ARG_NONE, &nofollow_symlinks, N_("Don't follow symlinks"), NULL},
  { NULL }
};

static const char *
type_to_string (GFileType type)
{
  switch (type)
    {
    default:
      return "invalid type";

    case G_FILE_TYPE_UNKNOWN:
      return "unknown";

    case G_FILE_TYPE_REGULAR:
      return "regular";

    case G_FILE_TYPE_DIRECTORY:
      return "directory";

    case G_FILE_TYPE_SYMBOLIC_LINK:
      return "symlink";

    case G_FILE_TYPE_SPECIAL:
      return "special";

    case G_FILE_TYPE_SHORTCUT:
      return "shortcut";

    case G_FILE_TYPE_MOUNTABLE:
      return "mountable";
    }
}

static void
show_info (GFileInfo *info)
{
  const char *name, *type;
  goffset size;
  char **attributes;
  int i;
  gboolean first_attr;

  if ((g_file_info_get_is_hidden (info)) && !show_hidden)
    return;

  name = g_file_info_get_name (info);
  if (name == NULL)
    name = "";

  size = g_file_info_get_size (info);
  type = type_to_string (g_file_info_get_file_type (info));
  if (show_long)
    g_print ("%s\t%"G_GUINT64_FORMAT"\t(%s)", name, (guint64)size, type);
  else
    g_print ("%s", name);

  first_attr = TRUE;
  attributes = g_file_info_list_attributes (info, NULL);
  for (i = 0 ; attributes[i] != NULL; i++)
    {
      char *val_as_string;

      if (!show_long ||
	  strcmp (attributes[i], G_FILE_ATTRIBUTE_STANDARD_NAME) == 0 ||
	  strcmp (attributes[i], G_FILE_ATTRIBUTE_STANDARD_SIZE) == 0 ||
	  strcmp (attributes[i], G_FILE_ATTRIBUTE_STANDARD_TYPE) == 0 ||
	  strcmp (attributes[i], G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN) == 0)
	continue;

      if (first_attr)
	{
	  g_print ("\t");
	  first_attr = FALSE;
	}
      else
	g_print (" ");
      val_as_string = g_file_info_get_attribute_as_string (info, attributes[i]);
      g_print ("%s=%s", attributes[i], val_as_string);
      g_free (val_as_string);
    }

  g_strfreev (attributes);

  g_print ("\n");
}

static void
list (GFile *file)
{
  GFileEnumerator *enumerator;
  GFileInfo *info;
  GError *error;

  if (file == NULL)
    return;

  error = NULL;
  enumerator = g_file_enumerate_children (file,
					  attributes,
					  nofollow_symlinks ? G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS : 0,
					  NULL,
					  &error);
  if (enumerator == NULL)
    {
      g_printerr ("Error: %s\n", error->message);
      g_error_free (error);
      error = NULL;
      return;
    }

  while ((info = g_file_enumerator_next_file (enumerator, NULL, &error)) != NULL)
    {
      show_info (info);

      g_object_unref (info);
    }

  if (error)
    {
      g_printerr (_("Error: %s\n"), error->message);
      g_error_free (error);
      error = NULL;
    }

  if (!g_file_enumerator_close (enumerator, NULL, &error))
    {
      g_printerr (_("Error: %s\n"), error->message);
      g_error_free (error);
      error = NULL;
    }
}

static void
print_mounts (const char *prefix)
{
  GList *l;
  GList *mounts;
  GVolumeMonitor *volume_monitor;

  volume_monitor = g_volume_monitor_get ();

  mounts = g_volume_monitor_get_mounts (volume_monitor);
  if (mounts != NULL)
    {
      for (l = mounts; l != NULL; l = l->next)
	{
	  GMount *mount = l->data;
	  GFile *mount_root;
	  char *uri;

	  mount_root = g_mount_get_root (mount);
	  uri = g_file_get_uri (mount_root);
	  if (prefix == NULL ||
	      g_str_has_prefix (uri, prefix))
	    g_print ("%s\n", uri);
	  g_free (uri);
	  g_object_unref (mount_root);
	  g_object_unref (mount);
	}
      g_list_free (mounts);
    }
  g_object_unref (volume_monitor);

  if (prefix == NULL || g_str_has_prefix ("file:///", prefix))
    g_print ("file:///\n");
}

static char*
shell_quote (const gchar *unquoted_string)
{
  const gchar *p;
  GString *dest;

  dest = g_string_new ("");

  p = unquoted_string;

  while (*p)
    {
      if (*p == ' ')
	g_string_append (dest, "\\ ");
      else if (*p == '\n')
	g_string_append (dest, "^J");
      else if (*p == '\\')
	g_string_append (dest, "\\\\");
      else if (*p == '\'')
	g_string_append (dest, "\\'");
      else if (*p == '"')
	g_string_append (dest, "\\\"");
      else
	g_string_append_c (dest, *p);

      ++p;
    }

  return g_string_free (dest, FALSE);
}

static void
show_completed_file (GFile *hit,
		     gboolean is_dir,
		     const char *arg)
{
  char *path, *cwd, *display, *t;
  GFile *cwd_f;
  GFile *home;

  if (g_file_is_native (hit))
    {
      cwd = g_get_current_dir ();
      cwd_f = g_file_new_for_path (cwd);
      g_free (cwd);

      home = g_file_new_for_path (g_get_home_dir ());

      if ((g_file_has_prefix (hit, home) ||
	   g_file_equal (hit, home)) &&
	  arg[0] == '~')
	{
	  t = g_file_get_relative_path (home, hit);
	  path = g_strconcat ("~", (t != NULL) ? "/": "", t, NULL);
	  g_free (t);
	}
      else if (g_file_has_prefix (hit, cwd_f) &&
	       !g_path_is_absolute (arg))
	path = g_file_get_relative_path (cwd_f, hit);
      else
	path = g_file_get_path (hit);

      g_object_unref (cwd_f);
      g_object_unref (home);

      display = shell_quote (path);
      g_free (path);
    }
  else
    display = g_file_get_uri (hit);

  g_print ("%s%s\n", display, (is_dir)?"/":"");
  g_free (display);
}

static void
print_completions (const char *arg)
{
  GFile *f;
  GFile *parent;
  char *basename;
  char *unescaped, *t;

  unescaped = g_shell_unquote (arg, NULL);
  if (unescaped == NULL)
    unescaped = g_strdup (arg);

  if (*unescaped == '~')
    {
      t = unescaped;
      unescaped = g_strconcat (g_get_home_dir(), t+1, NULL);
      g_free (t);
    }

  f = g_file_new_for_commandline_arg (unescaped);

  if (g_str_has_suffix (arg, "/") || *arg == 0)
    {
      parent = g_object_ref (f);
      basename = g_strdup ("");
    }
  else
    {
      parent = g_file_get_parent (f);
      basename = g_file_get_basename (f);
    }

  if (parent == NULL ||
      strchr (arg, '/') == NULL ||
      !g_file_query_exists (parent, NULL))
    {
      GMount *mount;
      mount = g_file_find_enclosing_mount (f, NULL, NULL);
      if (mount == NULL)
	print_mounts (unescaped);
      else
	g_object_unref (mount);
    }

  if (parent != NULL)
    {
      GFileEnumerator *enumerator;
      enumerator = g_file_enumerate_children (parent,
					      G_FILE_ATTRIBUTE_STANDARD_NAME ","
					      G_FILE_ATTRIBUTE_STANDARD_TYPE,
					      nofollow_symlinks ? G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS : 0,
					      NULL,
					      NULL);
      if (enumerator != NULL)
	{
	  GFileInfo *info;

	  while ((info = g_file_enumerator_next_file (enumerator, NULL, NULL)) != NULL)
	    {
	      const char *name;
	      GFileType type;

	      name = g_file_info_get_name (info);
	      type = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_STANDARD_TYPE);
	      if (name != NULL && g_str_has_prefix (name, basename))
		{
		  GFile *entry;

		  entry = g_file_get_child (parent, name);
		  show_completed_file (entry, type == G_FILE_TYPE_DIRECTORY, arg);
		  g_object_unref (entry);
		}
	      g_object_unref (info);
	    }
	  g_file_enumerator_close (enumerator, NULL, NULL);
	}
      g_object_unref (parent);
    }

  g_object_unref (f);
  g_free (basename);
  g_free (unescaped);
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
  context = g_option_context_new (_("- list files at <location>"));
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

  if (attributes != NULL)
    {
      /* asking for attributes implies -l; otherwise it won't get shown */
      show_long = TRUE;
    }

  attributes = g_strconcat (G_FILE_ATTRIBUTE_STANDARD_NAME ","
			    G_FILE_ATTRIBUTE_STANDARD_TYPE ","
			    G_FILE_ATTRIBUTE_STANDARD_SIZE ","
			    G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
			    attributes != NULL ? "," : "",
			    attributes,
			    NULL);

  if (show_completions != NULL)
    {
      print_completions (show_completions);
      return 0;
    }

  if (argc > 1)
    {
      int i;

      for (i = 1; i < argc; i++) {
	file = g_file_new_for_commandline_arg (argv[i]);
	list (file);
	g_object_unref (file);
      }
    }
  else
    {
      char *cwd;

      cwd = g_get_current_dir ();
      file = g_file_new_for_path (cwd);
      g_free (cwd);
      list (file);
      g_object_unref (file);
    }

  g_free (attributes);

  return 0;
}
