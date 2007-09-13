#include <string.h>
#include <unistd.h>

#include <glib.h>
#include "gfile.h"
#include "gseekable.h"
#include "gfileinputstreamlocal.h"
#include "gfileoutputstreamlocal.h"
#include "ginputstreamsocket.h"

static void
test_out ()
{
  GOutputStream *out;
  GFile *file;
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

  g_print ("test_out\n");
  
  unlink ("/tmp/test");

  file = g_file_get_for_path ("/tmp/test");
  out = (GOutputStream *)g_file_create (file, NULL, NULL);

  left = sizeof(buffer);
  ptr = buffer;
  
  while (left > 0)
    {
      error = NULL;
      res = g_output_stream_write (out, ptr, MIN (left, 128), NULL, &error);
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

  close_res = g_output_stream_close (out, NULL, NULL);
  g_print ("close res: %d\n", close_res);
}

static void
test_sync (char *uri, gboolean dump)
{
  GInputStream *in;
  GFile *file;
  char buffer[1025];
  gssize res;
  gboolean close_res;

  g_print ("> test_sync %s\n", uri);
  
  file = g_file_get_for_uri (uri);
  in = (GInputStream *)g_file_read (file, NULL, NULL);
  if (in == NULL)
    goto out;

  while (1)
    {
      res = g_input_stream_read (in, buffer, 1024, NULL, NULL);
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

  close_res = g_input_stream_close (in, NULL, NULL);

  if (!dump)
    g_print ("close res: %d\n", close_res);

 out:
  g_print ("< test_sync\n");
}

typedef struct {
  char *buffer;
  GCancellable *c;
} AsyncData;

static void
async_data_free (gpointer _data)
{
  AsyncData *data = _data;
  g_object_unref (data->c);
  g_free (data);
}

static void
close_done (GInputStream *stream,
	    gboolean      result,
	    gpointer      data,
	    GError       *error)
{
  g_print ("close result: %d\n", result);
  if (!result)
    g_print ("Close error %d: %s\n", error->code, error->message);
}

static void
read_done (GInputStream *stream,
	   void         *buffer,
	   gsize         count_requested,
	   gssize        count_read,
	   gpointer      _data,
	   GError       *error)
{
  AsyncData *data = _data;
  g_print ("count_read: %d\n", count_read);
  if (count_read == -1)
    g_print ("Error %d: %s\n", error->code, error->message);

  if (count_read > 0)
    {
      g_input_stream_read_async (stream, data->buffer, 1024, 0, read_done, data, NULL, data->c);
      //g_cancellable_cancel (data->c);
    }
  else
    g_input_stream_close_async (stream, 0, close_done, data, async_data_free, data->c);
}

static void
test_async (char *uri, gboolean dump)
{
  GInputStream *in;
  GFile *file;
  AsyncData *data = g_new0 (AsyncData, 1);

  data->buffer = g_malloc (1025);
  data->c = g_cancellable_new ();

  file = g_file_get_for_uri (uri);
  in = (GInputStream *)g_file_read (file, NULL, NULL);
  if (in == NULL)
    return;
  
  g_input_stream_read_async (in, data->buffer, 1024, 0, read_done, data, NULL, data->c);
}

static gboolean
cancel_cancellable_cb (gpointer data)
{
  GCancellable *cancellable = G_CANCELLABLE (data);

  g_cancellable_cancel (cancellable);
  g_object_unref (cancellable);
  
  return FALSE;
}

static gpointer
cancel_thread (gpointer data)
{
  sleep (1);
  g_print ("cancel_thread GO!\n");
  g_cancellable_cancel (G_CANCELLABLE (data));
  return NULL;
}

static void
test_seek (void)
{
  GInputStream *in;
  char buffer1[1025];
  char buffer2[1025];
  gssize res;
  gboolean close_res;
  GFile *file;
  GSeekable *seekable;
  GError *error;
  GCancellable *c;

  file = g_file_get_for_uri ("foo:///etc/passwd");

  error = NULL;
  in = (GInputStream *)g_file_read (file, NULL, &error);

  if (in == NULL)
    {
      g_print ("Can't find foo:///etc/passwd: %s\n", error->message);
      g_error_free (error);
      return;
    }
  
  seekable = G_SEEKABLE (in);

  g_print ("offset: %d\n", (int)g_seekable_tell (seekable));
  
  res = g_input_stream_read (in, buffer1, 1024, NULL, NULL);
  g_print ("read 1 res = %d\n", res);

  g_print ("offset: %d\n", (int)g_seekable_tell (seekable));
  
  res = g_seekable_seek (seekable, 0, G_SEEK_SET, NULL, NULL);
  g_print ("seek res = %d\n", res);

  c = g_cancellable_new ();
  if (0) g_thread_create (cancel_thread, c, FALSE, NULL);
  res = g_input_stream_read (in, buffer2, 1024, c, &error);
  g_print ("read 2 res = %d\n", res);
  if (res == -1)
    g_print ("error: %s\n", error->message);

  g_object_unref (c);
  
  if (memcmp (buffer1, buffer2, 1024) != 0)
    g_print ("Buffers differ\n");
  
  close_res = g_input_stream_close (in, NULL, NULL);
  g_print ("close res: %d\n", close_res);
}

int
main (int argc, char *argv[])
{
  GFile *file;
  GMainLoop *loop;

  g_type_init ();
  g_thread_init (NULL);

  if (0)
    test_seek ();
  
  loop = g_main_loop_new (NULL, FALSE);

  if (0) {
    GInputStream *s;
    char *buffer;
    gssize res;
    GCancellable *c;

    buffer = g_malloc (1025);
    
    s = g_input_stream_socket_new (0, FALSE);

    if (1)
      {
	res = g_input_stream_read (s, buffer, 128, NULL, NULL);
	g_print ("res1: %d\n", res);
	res = g_input_stream_read (s, buffer, 128, NULL, NULL);
	g_print ("res2: %d\n", res);
      }

    c = g_cancellable_new ();
    g_input_stream_read_async (s, buffer, 128, 0, read_done, buffer, NULL, c);
    if (1) g_timeout_add (1000, cancel_cancellable_cb, g_object_ref (c));
    g_print ("main loop run\n");
    g_main_loop_run (loop);
    g_object_unref (c);
    g_print ("main loop quit\n");
  }

  
  file = g_file_get_for_path ("/tmp");

  if (0) test_sync ("foo:///etc/passwd", FALSE);
  if (1) test_async ("foo:///etc/passwd", TRUE);

  if (0) test_out ();

  g_main_loop_run (loop);
  
  return 0;
}
