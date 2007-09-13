#include <config.h>

#include <glib.h>
#include <locale.h>
#include <gvfs/gfile.h>

static char *attributes = NULL;

static GOptionEntry entries[] = 
{
	{ "attributes", 'a', 0, G_OPTION_ARG_STRING, &attributes, "The attributes to get", NULL },
	{ NULL }
};

static void
show_info (GFileInfo *info)
{
  const char *name;
  
  name = g_file_info_get_name (info);
  g_print ("filename: %s\n", name);
}

static void
list (GFile *file)
{
  GFileInfoRequestFlags request;
  GFileEnumerator *enumerator;
  GFileInfo *info;
  GError *error;

  if (file == NULL)
    return;
  
  request = G_FILE_INFO_FILE_TYPE | G_FILE_INFO_NAME;

  enumerator = g_file_enumerate_children (file, request, attributes, TRUE);

  error = NULL;
  while ((info = g_file_enumerator_next_file (enumerator, &error)) != NULL)
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
	 
  if (!g_file_enumerator_stop (enumerator, &error))
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
  
  if (argc > 1)
    {
      int i;
      
      for (i = 1; i < argc; i++) {
	file = g_file_get_for_commandline_arg (argv[i]);
	list (file);
	g_object_unref (file);
      }
    }
  else
    {
      char *cwd;
      
      cwd = g_get_current_dir ();
      file = g_file_get_for_path (cwd);
      g_free (cwd);
      list (file);
      g_object_unref (file);
    }

  return 0;
}
