#include <glib.h>
#include <string.h>

#include "gmemoryinputstream.h"
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

  g_print ("Testing GMemoryInputStream...\n");

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


int
main (int argc, char **argv)
{
  int res;
 
  g_type_init ();
  g_thread_init (NULL);

  res = TRUE;
  res &= test_memory_input_stream ();

  return res;
}

