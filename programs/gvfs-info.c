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

static char *attributes = NULL;
static gboolean nofollow_symlinks = FALSE;
static gboolean filesystem = FALSE;
static gboolean writable = FALSE;

static GOptionEntry entries[] =
{
  { "query-writable", 'w', 0, G_OPTION_ARG_NONE, &writable, N_("List writable attributes"), NULL },
  { "filesystem", 'f', 0, G_OPTION_ARG_NONE, &filesystem, N_("Get filesystem info"), NULL },
  { "attributes", 'a', 0, G_OPTION_ARG_STRING, &attributes, N_("The attributes to get"), NULL },
  { "nofollow-symlinks", 'n', 0, G_OPTION_ARG_NONE, &nofollow_symlinks, N_("Don't follow symlinks"), NULL },
  { NULL }
};

static const char *
type_to_string (GFileType type)
{
  switch (type)
    {
    default:
      return _("invalid type");

    case G_FILE_TYPE_UNKNOWN:
      return _("unknown");

    case G_FILE_TYPE_REGULAR:
      return _("regular");

    case G_FILE_TYPE_DIRECTORY:
      return _("directory");

    case G_FILE_TYPE_SYMBOLIC_LINK:
      return _("symlink");

    case G_FILE_TYPE_SPECIAL:
      return _("special");

    case G_FILE_TYPE_SHORTCUT:
      return _("shortcut");

    case G_FILE_TYPE_MOUNTABLE:
      return _("mountable");
    }
}

static char *
escape_string (const char *in)
{
  GString *str;
  static char *hex_digits = "0123456789abcdef";
  char c;


  str = g_string_new ("");

  while ((c = *in++) != 0)
    {
      if (c >= 32 && c <= 126 && c != '\\')
	g_string_append_c (str, c);
      else
	{
	  g_string_append (str, "\\x");
	  g_string_append_c (str, hex_digits[(c >> 8) & 0xf]);
	  g_string_append_c (str, hex_digits[c & 0xf]);
	}
    }

  return g_string_free (str, FALSE);
}

static void
show_attributes (GFileInfo *info)
{
  char **attributes;
  char *s;
  int i;

  attributes = g_file_info_list_attributes (info, NULL);

  g_print (_("attributes:\n"));
  for (i = 0; attributes[i] != NULL; i++)
    {
      /* list the icons in order rather than displaying "GThemedIcon:0x8df7200" */
      if (strcmp (attributes[i], "standard::icon") == 0)
	{
	  GIcon *icon;
	  int j;
	  const char * const *names = NULL;
	  icon = g_file_info_get_icon (info);

	  /* only look up names if GThemedIcon */
	  if (G_IS_THEMED_ICON(icon))
	    {
	      names = g_themed_icon_get_names (G_THEMED_ICON (icon));
	      g_print ("  %s: ", attributes[i]);
	      for (j = 0; names[j] != NULL; j++)
		g_print ("%s%s", names[j], (names[j+1] == NULL)?"":", ");
	      g_print ("\n");
	    }
	  else
	    {
	      s = g_file_info_get_attribute_as_string (info, attributes[i]);
	      g_print ("  %s: %s\n", attributes[i], s);
	      g_free (s);
	    }
	}
      else
	{
	  s = g_file_info_get_attribute_as_string (info, attributes[i]);
	  g_print ("  %s: %s\n", attributes[i], s);
	  g_free (s);
	}
    }
  g_strfreev (attributes);
}

static void
show_info (GFileInfo *info)
{
  const char *name, *type;
  char *escaped;
  goffset size;

  name = g_file_info_get_display_name (info);
  if (name)
    g_print (_("display name: %s\n"), name);

  name = g_file_info_get_edit_name (info);
  if (name)
    g_print (_("edit name: %s\n"), name);

  name = g_file_info_get_name (info);
  if (name)
    {
      escaped = escape_string (name);
      g_print (_("name: %s\n"), escaped);
      g_free (escaped);
    }

  if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_TYPE))
    {
      type = type_to_string (g_file_info_get_file_type (info));
      g_print (_("type: %s\n"), type);
    }

  if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
    {
      size = g_file_info_get_size (info);
      g_print (_("size: "));
      g_print (" %"G_GUINT64_FORMAT"\n", (guint64)size);
    }

  if (g_file_info_get_is_hidden (info))
    g_print (_("hidden\n"));

  show_attributes (info);
}

static void
query_info (GFile *file)
{
  GFileQueryInfoFlags flags;
  GFileInfo *info;
  GError *error;

  if (file == NULL)
    return;

  if (attributes == NULL)
    attributes = "*";

  flags = 0;
  if (nofollow_symlinks)
    flags |= G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS;

  error = NULL;
  if (filesystem)
    info = g_file_query_filesystem_info (file, attributes, NULL, &error);
  else
    info = g_file_query_info (file, attributes, flags, NULL, &error);

  if (info == NULL)
    {
      g_printerr ("Error getting info: %s\n", error->message);
      g_error_free (error);
      return;
    }

  if (filesystem)
    show_attributes (info);
  else
    show_info (info);

  g_object_unref (info);
}

static char *
attribute_type_to_string (GFileAttributeType type)
{
  switch (type)
    {
    case G_FILE_ATTRIBUTE_TYPE_INVALID:
      return "invalid";
    case G_FILE_ATTRIBUTE_TYPE_STRING:
      return "string";
    case G_FILE_ATTRIBUTE_TYPE_BYTE_STRING:
      return "bytestring";
    case G_FILE_ATTRIBUTE_TYPE_BOOLEAN:
      return "boolean";
    case G_FILE_ATTRIBUTE_TYPE_UINT32:
      return "uint32";
    case G_FILE_ATTRIBUTE_TYPE_INT32:
      return "int32";
    case G_FILE_ATTRIBUTE_TYPE_UINT64:
      return "uint64";
    case G_FILE_ATTRIBUTE_TYPE_INT64:
      return "int64";
    case G_FILE_ATTRIBUTE_TYPE_OBJECT:
      return "object";
    default:
      return "uknown type";
    }
}

static char *
attribute_flags_to_string (GFileAttributeInfoFlags flags)
{
  GString *s;
  int i;
  gboolean first;
  struct {
    guint32 mask;
    char *descr;
  } flag_descr[] = {
    {
      G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE,
      N_("Copy with file")
    },
    {
      G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED,
      N_("Keep with file when moved")
    }
  };

  first = TRUE;

  s = g_string_new ("");
  for (i = 0; i < G_N_ELEMENTS (flag_descr); i++)
    {
      if (flags & flag_descr[i].mask)
	{
	  if (!first)
	    g_string_append (s, ", ");
	  g_string_append (s, gettext (flag_descr[i].descr));
	  first = FALSE;
	}
    }

  return g_string_free (s, FALSE);
}

static void
get_writable_info (GFile *file)
{
  GFileAttributeInfoList *list;
  GError *error;
  int i;
  char *flags;

  if (file == NULL)
    return;

  error = NULL;

  list = g_file_query_settable_attributes (file, NULL, &error);
  if (list == NULL)
    {
      g_printerr (_("Error getting writable attributes: %s\n"), error->message);
      g_error_free (error);
      return;
    }

  g_print (_("Settable attributes:\n"));
  for (i = 0; i < list->n_infos; i++)
    {
      flags = attribute_flags_to_string (list->infos[i].flags);
      g_print (" %s (%s%s%s)\n",
	       list->infos[i].name,
	       attribute_type_to_string (list->infos[i].type),
	       (*flags != 0)?", ":"", flags);
      g_free (flags);
    }

  g_file_attribute_info_list_unref (list);

  list = g_file_query_writable_namespaces (file, NULL, &error);
  if (list == NULL)
    {
      g_printerr ("Error getting writable namespaces: %s\n", error->message);
      g_error_free (error);
      return;
    }

  if (list->n_infos > 0)
    {
      g_print (_("Writable attribute namespaces:\n"));
      for (i = 0; i < list->n_infos; i++)
	{
	  flags = attribute_flags_to_string (list->infos[i].flags);
	  g_print (" %s (%s%s%s)\n",
		   list->infos[i].name,
		   attribute_type_to_string (list->infos[i].type),
		   (*flags != 0)?", ":"", flags);
	}
    }

  g_file_attribute_info_list_unref (list);
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
  context = g_option_context_new (_("- show info for <location>"));
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

  if (argc > 1)
    {
      int i;

      for (i = 1; i < argc; i++) {
	file = g_file_new_for_commandline_arg (argv[i]);
	if (writable)
	  get_writable_info (file);
	else
	  query_info (file);
	g_object_unref (file);
      }
    }

  return 0;
}
