#include <config.h>

#include <stdio.h>
#include <unistd.h>
#include <locale.h>
#include <errno.h>

#include <glib.h>
#include <gvfs/gfile.h>

static GOptionEntry entries[] = 
{
	{ NULL }
};

static void
cat (GFile *file)
{
  GInputStream *in;
  char buffer[1025];
  char *p;
  gssize res;
  gboolean close_res;
  GError *error;
  
  error = NULL;
  in = (GInputStream *)g_file_read (file, NULL, &error);
  if (in == NULL)
    {
      g_printerr ("Error opening file: %s\n", error->message);
      g_error_free (error);
      return;
    }

  while (1)
    {
      res = g_input_stream_read (in, buffer, 1024, NULL, &error);
      if (res > 0)
	{
	  ssize_t written;

	  p = buffer;
	  while (res > 0)
	    {
	      written = write (STDOUT_FILENO, p, res);
	      
	      if (written == -1 && errno != EINTR)
		{
		  perror ("Error writing to stdout");
		  goto out;
		}
	      res -= written;
	      p += written;
	    }
	}
      else if (res < 0)
	{
	  g_printerr ("Error reading: %s\n", error->message);
	  g_error_free (error);
	  error = NULL;
	  break;
	}
      else if (res == 0)
	break;
    }

 out:
  
  close_res = g_input_stream_close (in, NULL, &error);
  if (!close_res)
    {
      g_printerr ("Error closing: %s\n", error->message);
      g_error_free (error);
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
  context = g_option_context_new ("- output files at <location>");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);
  
  if (argc > 1)
    {
      int i;
      
      for (i = 1; i < argc; i++) {
	file = g_file_get_for_commandline_arg (argv[i]);
	cat (file);
	g_object_unref (file);
      }
    }

  return 0;
}
