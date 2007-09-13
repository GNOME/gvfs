#include <config.h>

#include <stdio.h>
#include <unistd.h>
#include <locale.h>
#include <errno.h>

#include <glib.h>
#include <gio/gfile.h>

static char *etag = NULL;
static gboolean backup = FALSE;
static gboolean create = FALSE;
static gboolean append = FALSE;
static gboolean print_etag = FALSE;

static GOptionEntry entries[] = 
{
	{ "backup", 'b', 0, G_OPTION_ARG_NONE, &backup, "Create backup", NULL },
	{ "create", 'c', 0, G_OPTION_ARG_NONE, &create, "Only create if not existing", NULL },
	{ "append", 'a', 0, G_OPTION_ARG_NONE, &append, "Append to end of file", NULL },
	{ "print_etag", 'p', 0, G_OPTION_ARG_NONE, &print_etag, "Print new etag at end", NULL },
	{ "etag", 'e', 0, G_OPTION_ARG_STRING, &etag, "The etag of the file being overwritten", NULL },
	{ NULL }
};

static gboolean
save (GFile *file)
{
  GOutputStream *out;
  char buffer[1025];
  char *p;
  gssize res;
  gboolean close_res;
  GError *error;
  gboolean save_res;

  error = NULL;
  if (create)
    out = (GOutputStream *)g_file_create (file, NULL, &error);
  else if (append)
    out = (GOutputStream *)g_file_append_to  (file, NULL, &error);
  else
    out = (GOutputStream *)g_file_replace  (file, etag, backup, NULL, &error);
  if (out == NULL)
    {
      g_printerr ("Error opening file: %s\n", error->message);
      g_error_free (error);
      return FALSE;
    }
  
  save_res = TRUE;
  
  while (1)
    {
      res = read (STDIN_FILENO, buffer, 1024);
      if (res > 0)
	{
	  ssize_t written;
	  
	  p = buffer;
	  while (res > 0)
	    {
	      error = NULL;
	      written = g_output_stream_write (out, p, res, NULL, &error);
	      if (written == -1)
		{
		  save_res = FALSE;
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
	  save_res = FALSE;
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
      save_res = FALSE;
      g_printerr ("Error closing: %s\n", error->message);
      g_error_free (error);
    }

  if (close_res && print_etag)
    {
      char *etag;
      etag = g_file_output_stream_get_etag (G_FILE_OUTPUT_STREAM (out), NULL, &error);

      if (etag)
	g_print ("Etag: %s\n", etag);
      else
	{
	  g_printerr ("Error getting etag: %s\n", error->message);
	  g_error_free (error);
	}
	
      g_free (etag);
    }
  
  g_object_unref (out);
  
  return save_res;
}

int
main (int argc, char *argv[])
{
  GError *error;
  GOptionContext *context;
  GFile *file;
  gboolean res;
  
  setlocale (LC_ALL, "");

  g_type_init ();
  
  error = NULL;
  context = g_option_context_new ("- output files at <location>");
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, &argc, &argv, &error);

  res = FALSE;
  
  if (argc > 1)
    {
      file = g_file_get_for_commandline_arg (argv[1]);
      res = save (file);
      g_object_unref (file);
    }

  if (res)
    return 0;
  return 1;
}
