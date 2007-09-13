#include <config.h>

#include <glib.h>
#include <locale.h>
#include <gio/gfile.h>

static char *attributes = NULL;
static gboolean show_hidden = FALSE;

static GOptionEntry entries[] = 
{
	{ "attributes", 'a', 0, G_OPTION_ARG_STRING, &attributes, "The attributes to get", NULL },
	{ "hidden", 'h', 0, G_OPTION_ARG_NONE, &show_hidden, "Show hidden files", NULL },
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
  g_print ("%s\t%"G_GUINT64_FORMAT"\t(%s)", name, (guint64)size, type);

  first_attr = TRUE;
  attributes = g_file_info_list_attributes (info, NULL);
  for (i = 0 ; attributes[i] != NULL; i++)
    {
      GFileAttributeValue *val;
      char *val_as_string;

      if (strcmp (attributes[i], G_FILE_ATTRIBUTE_STD_NAME) == 0 ||
	  strcmp (attributes[i], G_FILE_ATTRIBUTE_STD_SIZE) == 0 ||
	  strcmp (attributes[i], G_FILE_ATTRIBUTE_STD_TYPE) == 0)
	continue;

      if (first_attr)
	{
	  g_print ("\t");
	  first_attr = FALSE;
	}
      else
	g_print (" ");
      val = g_file_info_get_attribute (info, attributes[i]);
      val_as_string = g_file_attribute_value_as_string (val);
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
  enumerator = g_file_enumerate_children (file, attributes, 0, NULL, &error);
  if (enumerator == NULL)
    {
      g_print ("Error: %s\n", error->message);
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
      g_print ("Error: %s\n", error->message);
      g_error_free (error);
      error = NULL;
    }
	 
  if (!g_file_enumerator_stop (enumerator, NULL, &error))
    {
      g_print ("Error stopping enumerator: %s\n", error->message);
      g_error_free (error);
      error = NULL;
    }
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
  context = g_option_context_new ("- list files at <location>");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);

  attributes = g_strconcat (G_FILE_ATTRIBUTE_STD_NAME "," G_FILE_ATTRIBUTE_STD_TYPE "," G_FILE_ATTRIBUTE_STD_SIZE,
			    attributes != NULL ? "," : "",
			    attributes,
			    NULL);
  
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
