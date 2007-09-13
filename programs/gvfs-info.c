#include <config.h>

#include <glib.h>
#include <locale.h>
#include <gio/gfile.h>

static char *attributes = NULL;
static gboolean follow_symlinks = FALSE;

static GOptionEntry entries[] = 
{
	{ "attributes", 'a', 0, G_OPTION_ARG_STRING, &attributes, "The attributes to get", NULL },
	{ "follow-symlinks", 'f', 0, G_OPTION_ARG_NONE, &follow_symlinks, "Follow symlinks", NULL },
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
show_info (GFileInfo *info)
{
  const char *name, *type;
  char *escaped;
  goffset size;
  GFileAttribute *attributes;
  int n_attributes, i;

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

  if (g_file_info_get_is_hidden (info))
    g_print ("hidden\n");

  attributes = g_file_info_get_all_attributes (info, &n_attributes);

  if (attributes != NULL)
    {
      g_print ("attributes:\n");
      for (i = 0; i < n_attributes; i++)
	g_print ("  %s: %s\n", attributes[i].attribute, attributes[i].value);
      g_free (attributes);
    }
}

static void
get_info (GFile *file)
{
  GFileInfoRequestFlags request;
  GFileInfo *info;
  GError *error;

  if (file == NULL)
    return;

  request =
    G_FILE_INFO_FILE_TYPE | G_FILE_INFO_NAME |
    G_FILE_INFO_SIZE | G_FILE_INFO_IS_HIDDEN |
    G_FILE_INFO_DISPLAY_NAME | G_FILE_INFO_EDIT_NAME;
  
  error = NULL;
  info = g_file_get_info (file, request, attributes, follow_symlinks, NULL, &error);

  if (info == NULL)
    {
      g_printerr ("Error getting info: %s\n", error->message);
      g_error_free (error);
      return;
    }

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
