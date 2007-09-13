#include <config.h>

#include <stdio.h>
#include <unistd.h>
#include <locale.h>
#include <errno.h>

#include <glib.h>
#include <gio/gfile.h>

static GOptionEntry entries[] = 
{
	{ NULL }
};

static void
create (GFile *file)
{
  GOutputStream *out;
  char buffer[1025];
  char *p;
  gssize res;
  gboolean close_res;
  GError *error;
  
  error = NULL;
  out = (GOutputStream *)g_file_create (file, NULL, &error);
  if (out == NULL)
    {
      g_printerr ("Error opening file: %s\n", error->message);
      g_error_free (error);
      return;
    }

  while (1)
    {
      res = read (STDIN_FILENO, buffer, 1024);
      g_print ("read: %d\n", res);
      if (res > 0)
	{
	  ssize_t written;
	  
	  p = buffer;
	  while (res > 0)
	    {
	      error = NULL;
	      written = g_output_stream_write (out, p, res, NULL, &error);
	      g_print ("written: %d\n", written);
	      if (written == -1)
		{
		  g_printerr ("Error writing to stream: %s", error->message);
		  g_error_free (error);
		  goto out;
		}
	      res -= written;
	      p += written;
	    }
	}
      else if (res < 0)
	{
	  perror ("Error reading stdin");
	  break;
	}
      else if (res == 0)
	break;
    }

 out:
  
  close_res = g_output_stream_close (out, NULL, &error);
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
      file = g_file_get_for_commandline_arg (argv[1]);
      create (file);
      g_object_unref (file);
    }

  return 0;
}
