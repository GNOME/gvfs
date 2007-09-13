#include <glib.h>
#include <string.h>

#include "gmemoryinputstream.h"
#include "gmemoryoutputstream.h"
#include "gbufferedinputstream.h"
#include "gbufferedoutputstream.h"
#include "gseekable.h"

#include <stdlib.h>

static const char *gmis_data = "Hab nun ach! Philosophie, Juristerei und Medizin";

#define test_assert(expr)			G_STMT_START{	         	\
     if G_LIKELY(expr) { } else 			      	        \
        test_assert_warning (__FILE__,    	          \
                             __LINE__,	              \
                             __PRETTY_FUNCTION__,	    \
	                           #expr);		  }G_STMT_END

static void
test_assert_warning (const char *file,
                     const int   line,
                     const char *pretty_function,
                     const char *expression)
{
   g_log ("test",
          G_LOG_LEVEL_ERROR,
          "line %d (%s): assertion failed: (%s)",
          line, 
          pretty_function,
          expression);
}



static gboolean
test_memory_input_stream ()
{
  GInputStream *stream;
  char          buf[100];
  goffset       n;
  gsize         nread;
  gboolean      res;

  g_print ("Testing GMemoryInputStream...");

  stream = g_memory_input_stream_from_data (gmis_data,
                                            strlen (gmis_data));

  test_assert (stream != NULL);

  memset (buf, 0, sizeof (buf));

  n = g_input_stream_read (stream, buf, 3, NULL, NULL);

  test_assert (n == 3);
  test_assert (strcmp (buf, "Hab") == 0);

  n = g_input_stream_skip (stream, 4, NULL, NULL);
  test_assert (n == 4);

  nread = 0;
  res = g_input_stream_read_all (stream, buf, sizeof (buf), &nread, NULL, NULL);

  test_assert (res && nread == strlen (gmis_data) - 7);
  test_assert (strcmp (buf, gmis_data + 7) == 0);

  res = g_seekable_can_seek (G_SEEKABLE (stream));
  test_assert (res == TRUE);

  n = g_seekable_tell (G_SEEKABLE (stream));
  test_assert (n == strlen (gmis_data));
  
  res = g_seekable_seek (G_SEEKABLE (stream), -n, G_SEEK_CUR, NULL, NULL);
  test_assert (res == TRUE);

  n = g_seekable_tell (G_SEEKABLE (stream));
  test_assert (n == 0);

  res = g_seekable_seek (G_SEEKABLE (stream), 4, G_SEEK_SET, NULL, NULL);
  test_assert (res == TRUE);

  memset (buf, 0, sizeof (buf));
  n = g_input_stream_read (stream, buf, 3, NULL, NULL);
  test_assert (n == 3);
  test_assert (strcmp (buf, "nun") == 0);

  res = g_seekable_seek (G_SEEKABLE (stream), -1, G_SEEK_SET, NULL, NULL);
  test_assert (res == FALSE);

  res = g_seekable_seek (G_SEEKABLE (stream), 1, G_SEEK_END, NULL, NULL);
  test_assert (res == FALSE);

  res = g_seekable_seek (G_SEEKABLE (stream), 99, G_SEEK_CUR, NULL, NULL);
  test_assert (res == FALSE);

  res = g_seekable_seek (G_SEEKABLE (stream), -1, G_SEEK_END, NULL, NULL);
  test_assert (res == TRUE);
  
  memset (buf, 0, sizeof (buf));
  n = g_input_stream_read (stream, buf, 10, NULL, NULL);
  test_assert (n == 1);
  test_assert (strcmp (buf, "n") == 0);

  n = g_input_stream_read (stream, buf, 10, NULL, NULL);
  test_assert (n == 0);

  g_object_unref (stream);

  g_print ("DONE [OK]\n");
  return TRUE;
}



static gboolean
test_memory_output_stream (gboolean use_own_array)
{
  GOutputStream *stream;
  GByteArray    *array = NULL;
  GByteArray    *data;
  gsize          n, len;
  gssize         sn;
  goffset        pos;
  gboolean       res;
  GError        *error = NULL;
  gsize          gmis_len;

  g_print ("Testing GMemoryOutputStream (%s)...",
           use_own_array ? "external array" : "iternal array");
 
  gmis_len = strlen (gmis_data) + 1; //we want the \0

  if (use_own_array) {
    array = g_byte_array_new ();
  }

  stream = g_memory_output_stream_new (array);

  test_assert (stream != NULL);
  g_object_get (stream, "data", &data, NULL);

  if (use_own_array) {
    test_assert (data == array);
  } else {
    array = data;
  }

  len = 10;

  res = g_output_stream_write_all (stream,
           (void *) gmis_data,
           len,
           &n,
           NULL,
           NULL);

  test_assert (res == TRUE);
  test_assert (len == n);
  test_assert (memcmp (array->data, data->data, len) == 0);
  test_assert (memcmp (array->data, gmis_data, len) == 0);

  len = gmis_len - n;

  res = g_output_stream_write_all (stream,
           (void *) (gmis_data  + n),
           len,
           &n,
           NULL,
           NULL);
  
  test_assert (res == TRUE);
  test_assert (len == n);
  test_assert (memcmp (array->data, data->data, gmis_len) == 0);
  test_assert (memcmp (array->data, gmis_data, gmis_len) == 0);

  //Test limits
  g_object_set (stream, "size-limit", gmis_len, NULL);

  n = g_output_stream_write (stream,
                             (void *) gmis_data,
                             10,
                             NULL,
                             NULL);

  test_assert (n == -1);
  g_object_set (stream, "size-limit", 0, NULL);


  //Test seeking
  pos = g_seekable_tell (G_SEEKABLE (stream));
  test_assert (gmis_len == pos);

  len = strlen ("Medizin");
  res = g_seekable_seek (G_SEEKABLE (stream), -8, G_SEEK_CUR, NULL, &error);

  test_assert (res == TRUE);
  pos = g_seekable_tell (G_SEEKABLE (stream));
  test_assert (pos == gmis_len - (len + 1));



  sn = g_output_stream_write (stream,
                              "Medizin", 
                              len,
                              NULL,
                              NULL);

  test_assert (len == sn);
  test_assert (g_str_equal (array->data, data->data));
  test_assert (g_str_equal (array->data, gmis_data));
  

  g_object_unref (stream);

  if (use_own_array) {
    g_byte_array_free (array, TRUE);
  }

  g_print ("DONE [OK]\n");
  return TRUE;
}

static gboolean
test_buffered_input_stream ()
{
  GInputStream *mem_stream;
  GInputStream *stream;
  gboolean      res;
  gssize        n;
  gsize         nread;
  char          buf[100];

  g_print ("Testing GBufferedInputStream ...");

  mem_stream = g_memory_input_stream_from_data (gmis_data,
                                                strlen (gmis_data));

  test_assert (mem_stream != NULL);

  stream = g_buffered_input_stream_new_sized (mem_stream, 5);
  g_object_unref (mem_stream);

  memset (buf, 0, sizeof (buf));
  n = g_input_stream_read (stream, buf, 3, NULL, NULL);

  test_assert (n == 3);
  test_assert (strcmp (buf, "Hab") == 0);

  /* XXX, not sure if the default impl should be doing what it does */
  n = g_input_stream_skip (stream, 4, NULL, NULL);
  test_assert (n == 4);

  nread = 0;
  res = g_input_stream_read_all (stream, buf, sizeof (buf), &nread, NULL, NULL);

  test_assert (res && nread == strlen (gmis_data) - 7);
  test_assert (strcmp (buf, gmis_data + 7) == 0);

  g_object_unref (stream);

  g_print ("DONE [OK]\n");

  return TRUE;
}

static gboolean
test_buffered_output_stream ()
{
  GOutputStream *mem_stream;
  GOutputStream *stream;
  GByteArray    *array = NULL;
  gsize          n, len;
  gboolean       res;

  g_print ("Testing GBufferedOutputStream ...");

  mem_stream = g_memory_output_stream_new (array);

  test_assert (mem_stream != NULL);
  g_object_get (mem_stream, "data", &array, NULL);

  stream = g_buffered_output_stream_new_sized (mem_stream, 10);
  g_object_unref (mem_stream);

  /* if we write just 10 bytes everything should be
   * in the buffer and the underlying mem-stream 
   * should still be empty */
  len = 10;
  res = g_output_stream_write_all (stream,
           (void *) gmis_data,
           len,
           &n,
           NULL,
           NULL);

  test_assert (res == TRUE);
  test_assert (len == n);
  test_assert (array->len == 0);

  /* write 5 more bytes */
  len = 5;
  res = g_output_stream_write_all (stream,
           (void *) (gmis_data  + n),
           len,
           &n,
           NULL,
           NULL);

  test_assert (res == TRUE);
  test_assert (len == n);
  /* we should at least have the first 10 bytes now in the
   * mem-stream */
  test_assert (memcmp (array->data, gmis_data, 10) == 0);

  /* now flush the stream and see if we get all bytes written
   * to the mem-stream */

  res = g_output_stream_flush (stream,
                               NULL,
                               NULL);

  test_assert (res == TRUE);
  test_assert (memcmp (array->data, gmis_data, 15) == 0);

  g_object_unref (stream);

  g_print ("DONE [OK]\n");
  return TRUE;
}

static void
log_and_stop (const char     *domain,
              GLogLevelFlags  level,
              const char     *message,
              gpointer        data)
{	
	g_log_default_handler (domain, level, message, data);
  g_on_error_stack_trace ("test-streams");
  abort ();
}

int
main (int argc, char **argv)
{
  int res;

  g_type_init_with_debug_flags (G_TYPE_DEBUG_OBJECTS);
  g_thread_init (NULL);

  g_log_set_handler ("GLib", 
     (GLogLevelFlags) (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING),
     log_and_stop, NULL);

  g_log_set_handler ("GVFS", 
     (GLogLevelFlags) (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING),
     log_and_stop, NULL);

  res = TRUE;
  res &= test_memory_input_stream ();
  res &= test_memory_output_stream (TRUE);
  res &= test_memory_output_stream (FALSE);
  res &= test_buffered_input_stream ();
  res &= test_buffered_output_stream ();

  return res ? 0 : -1;
}

/* vim: ts=2 sw=2 et */
