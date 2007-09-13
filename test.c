#include <string.h>

#include <glib.h>
#include "gfile.h"
#include "ginputstreamfile.h"
#include "goutputstreamfile.h"

static void
test_out ()
{
  GOutputStream *out;
  char buffer[2345];
  char *ptr;
  char *str = "Test_String ";
  int str_len;
  int left;
  int i;
  gssize res;
  gboolean close_res;
  GError *error;

  str_len = strlen (str);
  for (i = 0; i < sizeof(buffer); i++) {
    buffer[i] = str[i%str_len];
  }
  
  out = g_output_stream_file_new ("/tmp/test",
				  G_OUTPUT_STREAM_FILE_OPEN_CREATE);

  left = sizeof(buffer);
  ptr = buffer;
  
  while (left > 0)
    {
      error = NULL;
      res = g_output_stream_write (out, ptr, MIN (left, 128), &error);
      g_print ("res = %d\n", res);

      if (res == -1)
	{
	  g_print ("error %d: %s\n", error->code, error->message);
	  g_error_free (error);
	}
      
      if (res > 0)
	{
	  left -= res;
	  ptr += res;
	}

      if (res < 0)
	break;
    }

  close_res = g_output_stream_close (out, NULL);
  g_print ("close res: %d\n", close_res);
}

static void
test_sync (char *filename, gboolean dump)
{
  GInputStream *in;
  char buffer[1025];
  gssize res;
  gboolean close_res;
  
  in = g_input_stream_file_new (filename);

  while (1)
    {
      res = g_input_stream_read (in, buffer, 1024, NULL);
      if (dump)
	{
	  if (res > 0)
	    {
	      buffer[res] = 0;
	      g_print ("%s", buffer);
	    }
	}
      else
	g_print ("res = %d\n", res);

      if (res <= 0)
	break;
    }

  close_res = g_input_stream_close (in, NULL);

  if (!dump)
    g_print ("close res: %d\n", close_res);
}

static void
close_done (GInputStream *stream,
	    gboolean      result,
	    gpointer      data,
	    GError       *error)
{
  g_print ("close result: %d\n", result);
}

static void
read_done (GInputStream *stream,
	   void         *buffer,
	   gsize         count_requested,
	   gssize        count_read,
	   gpointer      data,
	   GError       *error)
{
  g_print ("count_read: %d\n", count_read);
  if (count_read == -1)
    g_print ("Error %d: %s\n", error->code, error->message);

  if (count_read > 0)
    {
      g_input_stream_read_async (stream, buffer, 1024, 0, read_done, buffer, NULL);
      g_input_stream_cancel (stream);
    }
  else
    g_input_stream_close_async (stream, 0, close_done, buffer, g_free);
}


static void
test_async (char *filename, gboolean dump)
{
  GInputStream *in;
  char *buffer;

  buffer = g_malloc (1025);
  
  in = g_input_stream_file_new (filename);
  
  g_input_stream_read_async (in, buffer, 1024, 0, read_done, buffer, NULL);
}


int
main (int argc, char *argv[])
{
  GFile *file;
  GMainLoop *loop;

  g_type_init ();
  g_thread_init (NULL);
  
  file = g_file_get_for_path ("/tmp");

  if (0) test_sync ("/etc/passwd", FALSE);
  if (0) test_async ("/etc/passwd", TRUE);

  test_out ();

  loop = g_main_loop_new (NULL, FALSE);

  g_main_loop_run (loop);
  
  return 0;
}
