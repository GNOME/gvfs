#include <config.h>

#include <glib.h>
#include <locale.h>
#include <gio/gfile.h>

static char *attributes = NULL;
static gboolean nofollow_symlinks = FALSE;
static gboolean filesystem = FALSE;

static GOptionEntry entries[] = 
{
	{ "filesystem", 'f', 0, G_OPTION_ARG_NONE, &filesystem, "Get filesystem info", NULL },
	{ "attributes", 'a', 0, G_OPTION_ARG_STRING, &attributes, "The attributes to get", NULL },
	{ "nofollow-symlinks", 'n', 0, G_OPTION_ARG_NONE, &nofollow_symlinks, "Don't follow symlinks", NULL },
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
  int i;
  
  attributes = g_file_info_list_attributes (info, NULL);
  
  g_print ("attributes:\n");
  for (i = 0; attributes[i] != NULL; i++)
    {
      char *value;
      value = g_file_info_get_attribute_as_string (info, attributes[i]);
      g_print ("  %s: %s\n", attributes[i], value);
      g_free (value);
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
    g_print ("display name: %s\n", name);
  
  name = g_file_info_get_edit_name (info);
  if (name)
    g_print ("edit name: %s\n", name);
  
  name = g_file_info_get_name (info);
  if (name)
    {
      escaped = escape_string (name);
      g_print ("name: %s\n", escaped);
      g_free (escaped);
    }
  
  type = type_to_string (g_file_info_get_file_type (info));
  g_print ("type: %s\n", type);
  
  size = g_file_info_get_size (info);
  g_print ("size: %"G_GUINT64_FORMAT"\n", (guint64)size);
  
  if (g_file_info_get_flags (info) & G_FILE_FLAG_HIDDEN)
    g_print ("hidden\n");
  
  show_attributes (info);
}

static void
get_info (GFile *file)
{
  GFileGetInfoFlags flags;
  GFileInfo *info;
  GError *error;

  if (file == NULL)
    return;

  if (attributes == NULL)
    attributes = "*";

  flags = 0;
  if (nofollow_symlinks)
    flags |= G_FILE_GET_INFO_NOFOLLOW_SYMLINKS;
  
  error = NULL;
  if (filesystem)
    info = g_file_get_filesystem_info (file, attributes, NULL, &error);
  else
    info = g_file_get_info (file, attributes, flags, NULL, &error);

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

int
main (int argc, char *argv[])
{
  GError *error;
  GOptionContext *context;
  GFile *file;
  
  setlocale (LC_ALL, "");

  g_type_init ();
  
  error = NULL;
  context = g_option_context_new ("- show info for <location>");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);
  
  if (argc > 1)
    {
      int i;
      
      for (i = 1; i < argc; i++) {
	file = g_file_get_for_commandline_arg (argv[i]);
	get_info (file);
	g_object_unref (file);
      }
    }

  return 0;
}
