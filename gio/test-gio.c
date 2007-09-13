#include <string.h>
#include <unistd.h>
#include <locale.h>
#include <stdlib.h>
#include <stdio.h>

#include <glib.h>
#include "gfile.h"
#include "gseekable.h"
#include "glocalfileinputstream.h"
#include "glocalfileoutputstream.h"
#include "gsocketinputstream.h"
#include "gappinfo.h"
#include "gcontenttype.h"

static gpointer
cancel_thread (gpointer data)
{
#ifdef G_OS_WIN32
  _sleep (1);
#else
  sleep (1);
#endif
  g_print ("cancel_thread GO!\n");
  g_cancellable_cancel (G_CANCELLABLE (data));
  return NULL;
}

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
  GCancellable *c;
  GError *error;

  g_print ("> test_sync %s\n", uri);

  c = g_cancellable_new ();
  
  file = g_file_get_for_uri (uri);
  if (0) g_thread_create (cancel_thread, c, FALSE, NULL);
  error = NULL;
  in = (GInputStream *)g_file_read (file, c, &error);
  g_print ("input stream: %p\n", in);
  if (in == NULL)
    {
      g_print ("open error %d: %s\n", error->code, error->message);
      goto out;
    }

  while (1)
    {
      res = g_input_stream_read (in, buffer, 1024, c, NULL);
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

  close_res = g_input_stream_close (in, c, NULL);

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
close_done (GInputStream *stream,
	    gboolean      result,
	    gpointer      _data,
	    GError       *error)
{
  AsyncData *data = _data;
  
  g_print ("close result: %d\n", result);
  if (!result)
    g_print ("Close error %d: %s\n", error->code, error->message);

  g_object_unref (data->c);
  g_free (data);
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
      g_input_stream_read_async (stream, data->buffer, 1024, 0, read_done, data, data->c);
      //g_cancellable_cancel (data->c);
    }
  else
    g_input_stream_close_async (stream, 0, close_done, data, data->c);
}

static void
test_async_open_callback (GFile *file,
			  GFileInputStream *stream,
			  gpointer user_data,
			  GError *error)
{
  AsyncData *data = user_data;
  
  g_print ("test_async_open_callback: %p\n", stream);
  if (stream)
    g_input_stream_read_async (G_INPUT_STREAM (stream), data->buffer, 1024, 0, read_done, data, data->c);
  else
    g_print ("%s\n", error->message);
}


static void
test_async (char *uri, gboolean dump)
{
  GFile *file;
  AsyncData *data = g_new0 (AsyncData, 1);

  data->buffer = g_malloc (1025);
  data->c = g_cancellable_new ();

  file = g_file_get_for_uri (uri);
  g_file_read_async (file, 0, test_async_open_callback, data, data->c);
  if (0) g_thread_create (cancel_thread, data->c, FALSE, NULL);
}

static gboolean
cancel_cancellable_cb (gpointer data)
{
  GCancellable *cancellable = G_CANCELLABLE (data);

  g_cancellable_cancel (cancellable);
  g_object_unref (cancellable);
  
  return FALSE;
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

  file = g_file_get_for_uri ("test:///etc/passwd");

  error = NULL;
  in = (GInputStream *)g_file_read (file, NULL, &error);

  if (in == NULL)
    {
      g_print ("Can't find test:///etc/passwd: %s\n", error->message);
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

static void
test_content_types (void)
{
  GList *types, *l;
  char *type;
  char *desc;
  char *mime;
  
#ifdef G_OS_WIN32
  g_print (".aiff is_a .aiff: %d\n", g_content_type_is_a (".aiff", ".aiff"));
  g_print (".aiff is_a .gif: %d\n", g_content_type_is_a (".aiff", ".gif"));
  g_print (".aiff is_a text: %d\n", g_content_type_is_a (".aiff", "text"));
  g_print (".aiff is_a audio: %d\n", g_content_type_is_a (".aiff", "audio"));
  g_print (".jpg is_a .jpeg: %d\n", g_content_type_is_a (".jpg", ".jpeg"));

  g_print (".aiff descr: %s\n", g_content_type_get_description (".aiff"));
  g_print (".gif descr: %s\n", g_content_type_get_description (".gif"));
  g_print (".jpeg descr: %s\n", g_content_type_get_description (".jpeg"));
  
  g_print (".aiff mimetype: %s\n", g_content_type_get_mime_type (".aiff"));
  g_print (".gif mimetype: %s\n", g_content_type_get_mime_type (".gif"));
  g_print (".jpeg mimetype: %s\n", g_content_type_get_mime_type (".jpeg"));
  g_print ("* mimetype: %s\n", g_content_type_get_mime_type ("*"));
  g_print ("image mimetype: %s\n", g_content_type_get_mime_type ("image"));
#endif

  types = g_get_registered_content_types ();
  
  for (l = types; l != NULL; l = l->next)
    {
      type = l->data;
      desc = g_content_type_get_description (type);
      mime = g_content_type_get_mime_type (type);
      g_print ("type %s - %s (%s)\n", type, desc, mime);
      g_free (mime);
      g_free (type);
      g_free (desc);
    }
  
  g_list_free (types);
}

static gint
compare_apps (gconstpointer  _a,
	      gconstpointer  _b)
{
  GAppInfo *a = (GAppInfo *)_a;
  GAppInfo *b = (GAppInfo *)_b;
  char *name_a;
  char *name_b;
  int res;

  name_a = g_app_info_get_name (a);
  name_b = g_app_info_get_name (b);
  res = g_utf8_collate (name_a, name_b);
  g_free (name_a);
  g_free (name_b);
  return res;
}

static void
test_appinfo (void)
{
  GAppInfo *info;
  GList *infos, *l;
  const char *test_type;


#ifdef G_OS_WIN32
  test_type = ".jpg";
#else
  test_type = "text/html";
#endif
  info = g_get_default_app_info_for_type (test_type);
  g_print ("default app for %s: %s\n", test_type,
	   info? g_app_info_get_name (info): "None");
  
#if 0
  GError *error = NULL;

  if (0)
    {
      info = g_app_info_create_from_commandline ("/usr/bin/ls -l",
						 NULL, &error);
      if (info == NULL)
	g_print ("error: %s\n", error->message);
      else
	g_print ("new info - %p: %s\n", info, g_app_info_get_name (info));
    }
  

  g_print ("setting as default for x-test/gio\n");
  if (!g_app_info_set_as_default_for_type (info, "x-test/gio", NULL))
    g_print ("Failed!");

  info = g_get_default_app_info_for_type ("x-test/gio");
  g_print ("default x-test/gio - %p: %s\n", info, g_app_info_get_name (info));
#endif

  infos = g_get_all_app_info_for_type (test_type);
  g_print ("all %s app info: \n", test_type);
  for (l = infos; l != NULL; l = l->next)
    {
      info = l->data;
      g_print ("%p: %s\n", info, g_app_info_get_name (info));
    }

  infos = g_get_all_app_info ();
  g_print ("all app info: \n");
  infos = g_list_sort (infos, compare_apps);

  for (l = infos; l != NULL; l = l->next)
    {
      info = l->data;
      g_print ("%s%s\n", g_app_info_get_name (info),
	       g_app_info_should_show (info, "GNOME")?"":" (hidden)");
    }
}

int
main (int argc, char *argv[])
{
  GFile *file;
  GMainLoop *loop;

  setlocale (LC_ALL, "");
  
  g_type_init ();
  g_thread_init (NULL);

  if (1)
    {
      test_content_types ();
      test_appinfo ();
      return 0;
    }
  
  if (0)
    test_seek ();
  
  loop = g_main_loop_new (NULL, FALSE);

  if (0) {
    GInputStream *s;
    char *buffer;
    gssize res;
    GCancellable *c;

    buffer = g_malloc (1025);
    
    s = g_socket_input_stream_new (0, FALSE);

    if (1)
      {
	res = g_input_stream_read (s, buffer, 128, NULL, NULL);
	g_print ("res1: %d\n", res);
	res = g_input_stream_read (s, buffer, 128, NULL, NULL);
	g_print ("res2: %d\n", res);
      }

    c = g_cancellable_new ();
    g_input_stream_read_async (s, buffer, 128, 0, read_done, buffer, c);
    if (1) g_timeout_add (1000, cancel_cancellable_cb, g_object_ref (c));
    g_print ("main loop run\n");
    g_main_loop_run (loop);
    g_object_unref (c);
    g_print ("main loop quit\n");
  }

  
  file = g_file_get_for_path ("/tmp");

  if (0) test_sync ("test:///etc/passwd", FALSE);
  if (1) test_async ("test:///etc/passwd", TRUE);

  if (0) test_out ();

  g_print ("Starting mainloop\n");
  g_main_loop_run (loop);
  
  return 0;
}
