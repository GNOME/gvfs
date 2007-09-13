#include <config.h>

#include <glib.h>
#include <locale.h>
#include <gio/gfile.h>


static GOptionEntry entries[] = 
{
	{ NULL }
};


int
main (int argc, char *argv[])
{
  GError *error;
  GOptionContext *context;
  GFile *file;
  
  setlocale (LC_ALL, "");

  g_type_init ();
  
  error = NULL;
  context = g_option_context_new ("- delete files");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);
  
  if (argc > 1)
    {
      int i;
      
      for (i = 1; i < argc; i++) {
	file = g_file_new_for_commandline_arg (argv[i]);
	error = NULL;
	if (!g_file_delete (file, NULL, &error))
	  {
	    g_print ("Error deleting file: %s\n", error->message);
	    g_error_free (error);
	  }
	g_object_unref (file);
      }
    }

  return 0;
}
