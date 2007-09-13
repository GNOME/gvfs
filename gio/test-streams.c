#include <glib.h>
#include <string.h>

#include "gmemoryinputstream.h"
#include "gmemoryoutputstream.h"
#include "gseekable.h"

static const char *gmis_data = "Hab nun ach! Philosophie, Juristerei und Medizin";

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

  g_assert (stream != NULL);

  memset (buf, 0, sizeof (buf));

  n = g_input_stream_read (stream, buf, 3, NULL, NULL);

  g_assert (n == 3);
  g_assert (strcmp (buf, "Hab") == 0);

  n = g_input_stream_skip (stream, 4, NULL, NULL);
  g_assert (n == 4);

  nread = 0;
  res = g_input_stream_read_all (stream, buf, sizeof (buf), &nread, NULL, NULL);

  g_assert (res && nread == strlen (gmis_data) - 7);
  g_assert (strcmp (buf, gmis_data + 7) == 0);

  res = g_seekable_can_seek (G_SEEKABLE (stream));
  g_assert (res == TRUE);

  n = g_seekable_tell (G_SEEKABLE (stream));
  g_assert (n == strlen (gmis_data));
  
  res = g_seekable_seek (G_SEEKABLE (stream), -n, G_SEEK_CUR, NULL, NULL);
  g_assert (res == TRUE);

  n = g_seekable_tell (G_SEEKABLE (stream));
  g_assert (n == 0);

  res = g_seekable_seek (G_SEEKABLE (stream), 4, G_SEEK_SET, NULL, NULL);
  g_assert (res == TRUE);

  memset (buf, 0, sizeof (buf));
  n = g_input_stream_read (stream, buf, 3, NULL, NULL);
  g_assert (n == 3);
  g_assert (strcmp (buf, "nun") == 0);

  res = g_seekable_seek (G_SEEKABLE (stream), -1, G_SEEK_SET, NULL, NULL);
  g_assert (res == FALSE);

  res = g_seekable_seek (G_SEEKABLE (stream), 1, G_SEEK_END, NULL, NULL);
  g_assert (res == FALSE);

  res = g_seekable_seek (G_SEEKABLE (stream), 99, G_SEEK_CUR, NULL, NULL);
  g_assert (res == FALSE);

  res = g_seekable_seek (G_SEEKABLE (stream), -1, G_SEEK_END, NULL, NULL);
  g_assert (res == TRUE);
  
  memset (buf, 0, sizeof (buf));
  n = g_input_stream_read (stream, buf, 10, NULL, NULL);
  g_assert (n == 1);
  g_assert (strcmp (buf, "n") == 0);

  n = g_input_stream_read (stream, buf, 10, NULL, NULL);
  g_assert (n == 0);

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

  g_assert (stream != NULL);
  g_object_get (stream, "data", &data, NULL);

  if (use_own_array) {
    g_assert (data == array);
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

  g_assert (res == TRUE);
  g_assert (len == n);
  g_assert (memcmp (array->data, data->data, len) == 0);
  g_assert (memcmp (array->data, gmis_data, len) == 0);

  len = gmis_len - n;

  res = g_output_stream_write_all (stream,
           (void *) (gmis_data  + n),
           len,
           &n,
           NULL,
           NULL);
  
  g_assert (res == TRUE);
  g_assert (len == n);
  g_assert (memcmp (array->data, data->data, gmis_len) == 0);
  g_assert (memcmp (array->data, gmis_data, gmis_len) == 0);

  //Test limits
  g_object_set (stream, "size-limit", gmis_len, NULL);

  n = g_output_stream_write (stream,
                             (void *) gmis_data,
                             10,
                             NULL,
                             NULL);

  g_assert (n == -1);
  g_object_set (stream, "size-limit", 0, NULL);


  //Test seeking
  pos = g_seekable_tell (G_SEEKABLE (stream));
  g_assert (gmis_len == pos);

  len = strlen ("Medizin");
  res = g_seekable_seek (G_SEEKABLE (stream), -8, G_SEEK_CUR, NULL, &error);

  g_assert (res == TRUE);
  pos = g_seekable_tell (G_SEEKABLE (stream));
  g_assert (pos == gmis_len - (len + 1));



  sn = g_output_stream_write (stream,
                              "Medizin", 
                              len,
                              NULL,
                              NULL);

  g_assert (len == sn);
  g_assert (g_str_equal (array->data, data->data));
  g_assert (g_str_equal (array->data, gmis_data));
  

  if (use_own_array) {
    g_byte_array_free (array, TRUE);
  }

  g_print ("DONE [OK]\n");
  return TRUE;
}

int
main (int argc, char **argv)
{
  int res;
 
  g_type_init ();
  g_thread_init (NULL);

  res = TRUE;
  res &= test_memory_input_stream ();
  res &= test_memory_output_stream (TRUE);
  res &= test_memory_output_stream (FALSE);

  return res ? 0 : -1;
}

