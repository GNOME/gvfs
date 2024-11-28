/* outputstream.c
 *
 * Copyright (C) 2024 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with the Gnome Library; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Authors: Ondrej Holy <oholy@redhat.com>
 */

#include "config.h"
#include <gio/gio.h>

#define TEST_BUFFER "abcdefghijklmnopqrstuvwxyz"
#define TEST_BUFFER2 "0123456789"
#define TEST_BUFFER3 "helloworld"

#define _g_file_edit(file, flags, cancellable, error) \
  g_file_append_to(file, flags | (1 << 15), cancellable, error)

static GFile *
get_test_file (GFile *test_dir,
               const gchar *name,
               const gchar *output_buffer,
               gboolean create_dir)
{
  GFile *test_file;
  g_autoptr(GError) error = NULL;

  test_file = g_file_get_child_for_display_name (test_dir, name, &error);
  g_assert_no_error (error);
  g_assert_nonnull (test_file);

  g_file_delete (test_file, NULL, NULL);

  if (output_buffer != NULL)
    {
      GFileOutputStream *output_stream;
      gsize bytes_written = 0;

      output_stream = g_file_create (test_file,
                                     G_FILE_CREATE_NONE,
                                     NULL,
                                     &error);
      g_assert_no_error (error);
      g_assert_nonnull (output_stream);

      g_output_stream_write_all (G_OUTPUT_STREAM (output_stream),
                                 output_buffer,
                                 strlen (output_buffer),
                                 &bytes_written,
                                 NULL,
                                 &error);
      g_assert_no_error (error);
      g_assert_cmpuint (bytes_written, ==, strlen (output_buffer));

      g_output_stream_close (G_OUTPUT_STREAM (output_stream), NULL, &error);
      g_assert_no_error (error);
    }
  else if (create_dir)
    {
      g_file_make_directory (test_file, NULL, &error);
      g_assert_no_error (error);
    }

  return test_file;
}

static void
test_write_helper (GFile *test_file,
                   GFileOutputStream *output_stream,
                   const gchar *expected_input_buffer)
{
  g_autoptr(GError) error = NULL;
  gsize bytes_written = 0;
  g_autoptr(GFileInputStream) input_stream = NULL;
  gchar input_buffer[64] = { 0 };
  gsize bytes_read = 0;

  /* Test */
  g_output_stream_write_all (G_OUTPUT_STREAM (output_stream),
                             TEST_BUFFER,
                             strlen (TEST_BUFFER),
                             &bytes_written,
                             NULL,
                             &error);
  g_assert_no_error (error);
  g_assert_cmpuint (bytes_written, ==, strlen (TEST_BUFFER));

  g_output_stream_close (G_OUTPUT_STREAM (output_stream), NULL, &error);
  g_assert_no_error (error);

  /* Verify */
  input_stream = g_file_read (test_file, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (input_stream);

  g_input_stream_read_all (G_INPUT_STREAM (input_stream),
                           &input_buffer,
                           sizeof (input_buffer),
                           &bytes_read,
                           NULL,
                           &error);
  g_assert_no_error (error);
  g_assert_cmpuint (bytes_read, ==, strlen (expected_input_buffer));
  g_assert_cmpstr (input_buffer, ==, expected_input_buffer);

  g_input_stream_close (G_INPUT_STREAM (input_stream), NULL, &error);
  g_assert_no_error (error);

  /* Clean */
  g_file_delete (test_file, NULL, NULL);
}

static void
test_seek_helper (GFile *test_file,
                  GFileOutputStream *output_stream,
                  const gchar *expected_input_buffer)
{
  g_autoptr(GError) error = NULL;
  gsize bytes_written = 0;
  g_autoptr(GFileInputStream) input_stream = NULL;
  gchar input_buffer[32] = { 0 };
  gsize bytes_read = 0;

  /* Test */
  if (!g_seekable_can_seek (G_SEEKABLE (output_stream)))
    {
      g_test_skip ("Seek is not supported.");
      return;
    }

  g_output_stream_write_all (G_OUTPUT_STREAM (output_stream),
                             TEST_BUFFER,
                             strlen (TEST_BUFFER),
                             &bytes_written,
                             NULL,
                             &error);
  g_assert_no_error (error);
  g_assert_cmpuint (bytes_written, ==, strlen (TEST_BUFFER));

  g_seekable_seek (G_SEEKABLE (output_stream), 5, G_SEEK_SET, NULL, &error);
  g_assert_no_error (error);

  bytes_written = g_output_stream_write (G_OUTPUT_STREAM (output_stream),
                                         "1",
                                         1,
                                         NULL,
                                         &error);
  g_assert_no_error (error);
  g_assert_cmpuint (bytes_written, ==, 1);

  g_seekable_seek (G_SEEKABLE (output_stream), 5, G_SEEK_CUR, NULL, &error);
  g_assert_no_error (error);

  bytes_written = g_output_stream_write (G_OUTPUT_STREAM (output_stream),
                                         "2",
                                         1,
                                         NULL,
                                         &error);
  g_assert_no_error (error);
  g_assert_cmpuint (bytes_written, ==, 1);

  g_seekable_seek (G_SEEKABLE (output_stream), -5, G_SEEK_END, NULL, &error);
  g_assert_no_error (error);

  bytes_written = g_output_stream_write (G_OUTPUT_STREAM (output_stream),
                                         "3",
                                         1,
                                         NULL,
                                         &error);
  g_assert_no_error (error);
  g_assert_cmpuint (bytes_written, ==, 1);

  g_output_stream_close (G_OUTPUT_STREAM (output_stream), NULL, &error);
  g_assert_no_error (error);

  /* Verify */
  input_stream = g_file_read (test_file, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (input_stream);

  g_input_stream_read_all (G_INPUT_STREAM (input_stream),
                           &input_buffer,
                           sizeof (input_buffer),
                           &bytes_read,
                           NULL,
                           &error);
  g_assert_no_error (error);
  g_assert_cmpuint (bytes_read, ==, strlen (expected_input_buffer));
  g_assert_cmpstr (input_buffer, ==, expected_input_buffer);

  g_input_stream_close (G_INPUT_STREAM (input_stream), NULL, &error);
  g_assert_no_error (error);

  /* Clean */
  g_file_delete (test_file, NULL, NULL);
}

static void
test_truncate_helper (GFile *test_file,
                      GFileOutputStream *output_stream,
                      const gchar *expected_input_buffer,
                      gsize expected_input_buffer_len)
{
  g_autoptr(GError) error = NULL;
  gsize bytes_written = 0;
  g_autoptr(GFileInputStream) input_stream = NULL;
  gchar input_buffer[32] = { 0 };
  gsize bytes_read = 0;

  /* Test */
  if (!g_seekable_can_truncate (G_SEEKABLE (output_stream)))
    {
      g_test_skip ("Truncate is not supported.");
      return;
    }

  g_output_stream_write_all (G_OUTPUT_STREAM (output_stream),
                             TEST_BUFFER,
                             strlen (TEST_BUFFER),
                             &bytes_written,
                             NULL,
                             &error);
  g_assert_no_error (error);
  g_assert_cmpuint (bytes_written, ==, strlen (TEST_BUFFER));

  g_seekable_truncate (G_SEEKABLE (output_stream), 5, NULL, &error);
  g_assert_no_error (error);

  g_output_stream_write_all (G_OUTPUT_STREAM (output_stream),
                             TEST_BUFFER2,
                             strlen (TEST_BUFFER2),
                             &bytes_written,
                             NULL,
                             &error);
  g_assert_no_error (error);
  g_assert_cmpuint (bytes_written, ==, strlen (TEST_BUFFER2));

  g_output_stream_close (G_OUTPUT_STREAM (output_stream), NULL, &error);
  g_assert_no_error (error);

  /* Verify */
  input_stream = g_file_read (test_file, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (input_stream);

  g_input_stream_read_all (G_INPUT_STREAM (input_stream),
                           &input_buffer,
                           sizeof (input_buffer),
                           &bytes_read,
                           NULL,
                           &error);
  g_assert_no_error (error);
  g_assert_cmpmem (input_buffer,
                   bytes_read,
                   expected_input_buffer,
                   expected_input_buffer_len);

  g_input_stream_close (G_INPUT_STREAM (input_stream), NULL, &error);
  g_assert_no_error (error);

  /* Clean */
  g_file_delete (test_file, NULL, NULL);
}

static void
test_create_nonexistent (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that g_file_create() creates a file when it "
                  "doesn't exist yet.");

  /* Prepare */
  test_file = get_test_file (test_dir,
                             "test_create_nonexistent",
                             NULL,
                             FALSE);

  /* Test */
  output_stream = g_file_create (test_file, G_FILE_CREATE_NONE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_write_helper (test_file, output_stream, TEST_BUFFER);
}

static void
test_create_existent_file (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that g_file_create() fails with "
                  "G_IO_ERROR_EXISTS when the file already exists.");

  /* Prepare */
  test_file = get_test_file (test_dir,
                             "test_create_existent_file",
                             TEST_BUFFER,
                             FALSE);

  /* Test */
  output_stream = g_file_create (test_file, G_FILE_CREATE_NONE, NULL, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_assert_null (output_stream);
  g_clear_error (&error);

  /* Clean */
  g_file_delete (test_file, NULL, NULL);
}

static void
test_create_existent_dir (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that g_file_create() fails with "
                  "G_IO_ERROR_EXISTS when there is a dir.");

  /* Prepare */
  test_file = get_test_file (test_dir,
                             "test_create_existent_dir",
                             NULL,
                             TRUE);

  /* Test */
  output_stream = g_file_create (test_file, G_FILE_CREATE_NONE, NULL, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_assert_null (output_stream);

  /* Clean */
  g_file_delete (test_file, NULL, NULL);
}

static void
test_create_seek (gconstpointer user_data)
{
  g_autoptr(GError) error = NULL;
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that seek works on a stream created by "
                  "g_file_create().");

  /* Prepare */
  test_file = get_test_file (test_dir, "test_create_seek", NULL, FALSE);

  /* Test */
  output_stream = g_file_create (test_file, G_FILE_CREATE_NONE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_seek_helper (test_file, output_stream, "abcde1ghijk2mnopqrstu3wxyz");
}

static void
test_create_truncate (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that truncate works on a stream created by "
                  "g_file_create().");

  /* Prepare */
  test_file = get_test_file (test_dir, "test_create_truncate", NULL, FALSE);

  /* Test */
  output_stream = g_file_create (test_file, G_FILE_CREATE_NONE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_truncate_helper (test_file,
                        output_stream,
                        "abcde\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                        TEST_BUFFER2,
                        32);
}

static void
test_create_tell (gconstpointer user_data)
{
  g_autoptr(GError) error = NULL;
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that offset is correctly reported on a stream "
                   "created by g_file_create().");

  /* Prepare */
  test_file = get_test_file (test_dir, "test_create_tell", NULL, FALSE);

  /* Test */
  output_stream = g_file_create (test_file, G_FILE_CREATE_NONE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  if (!g_seekable_can_seek (G_SEEKABLE (output_stream)))
  {
    g_test_skip ("Seek is not supported.");
    return;
  }

  g_assert_cmpint (g_seekable_tell (G_SEEKABLE (output_stream)),
                   ==,
                   0);
}

static void
test_append_nonexistent (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that g_file_append_to() creates a file when it "
                  "doesn't exist yet.");

  /* Prepare */
  test_file = get_test_file (test_dir,
                             "test_append_nonexistent",
                             NULL,
                             FALSE);

  /* Test */
  output_stream = g_file_append_to (test_file,
                                    G_FILE_CREATE_NONE,
                                    NULL,
                                    &error);
  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_write_helper (test_file, output_stream, TEST_BUFFER);
}

static void
test_append_existent_file (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that g_file_append_to() appends data when the "
                  "file already exists.");

  /* Prepare */
  test_file = get_test_file (test_dir,
                             "test_append_existent_file",
                             TEST_BUFFER2,
                             FALSE);

  /* Test */
  output_stream = g_file_append_to (test_file,
                                    G_FILE_CREATE_NONE,
                                    NULL,
                                    &error);
  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_write_helper (test_file,
                     output_stream,
                     TEST_BUFFER2 TEST_BUFFER);
}

static void
test_append_existent_dir (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that g_file_append_to() fails with "
                  "G_IO_ERROR_IS_DIRECTORY when there is a dir.");

  /* Prepare */
  test_file = get_test_file (test_dir,
                             "test_append_existent_dir",
                             NULL,
                             TRUE);

  /* Test */
  output_stream = g_file_append_to (test_file, G_FILE_CREATE_NONE, NULL, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY);
  g_assert_null (output_stream);

  /* Clean */
  g_file_delete (test_file, NULL, NULL);
}

static void
test_append_seek (gconstpointer user_data)
{
  g_autoptr(GError) error = NULL;
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that seek works on a stream created by "
                  "g_file_append_to().");

  /* Prepare */
  test_file = get_test_file (test_dir, "test_append_seek", NULL, FALSE);

  /* Test */
  output_stream = g_file_append_to (test_file,
                                    G_FILE_CREATE_NONE,
                                    NULL,
                                    &error);
  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_seek_helper (test_file, output_stream, "abcdefghijklmnopqrstuvwxyz123");
}

static void
test_append_truncate (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that truncate works on a stream created by "
                  "g_file_append_to().");

  /* Prepare */
  test_file = get_test_file (test_dir,
                             "test_append_truncate",
                             TEST_BUFFER3,
                             FALSE);

  /* Test */
  output_stream = g_file_append_to (test_file,
                                    G_FILE_CREATE_NONE,
                                    NULL,
                                    &error);
  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_truncate_helper (test_file,
                        output_stream,
                        "hello" TEST_BUFFER2,
                        15);
}

static void
test_append_tell (gconstpointer user_data)
{
  g_autoptr(GError) error = NULL;
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that offset is correctly reported on a stream "
                   "created by g_file_append_to().");

  /* Prepare */
  test_file = get_test_file (test_dir, "test_append_tell", TEST_BUFFER, FALSE);

  /* Test */
  output_stream = g_file_append_to (test_file,
                                    G_FILE_CREATE_NONE,
                                    NULL,
                                    &error);
  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  if (!g_seekable_can_seek (G_SEEKABLE (output_stream)))
    {
      g_test_skip ("Seek is not supported.");
      return;
    }

  g_assert_cmpint (g_seekable_tell (G_SEEKABLE (output_stream)),
                   ==,
                   strlen (TEST_BUFFER));
}

static void
test_edit_nonexistent (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that _g_file_edit() creates a file when it "
                  "doesn't exist yet.");

  /* Prepare */
  test_file = get_test_file (test_dir,
                             "test_edit_nonexistent",
                             NULL,
                             FALSE);

  /* Test */
  output_stream = _g_file_edit (test_file, G_FILE_CREATE_NONE, NULL, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    {
      g_test_skip ("Edit is not supported.");
      return;
    }

  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_write_helper (test_file, output_stream, TEST_BUFFER);
}

static void
test_edit_existent_file (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that _g_file_edit() edits data when the "
                  "file already exists.");

  /* Prepare */
  test_file = get_test_file (test_dir,
                             "test_edit_existent_file",
                             TEST_BUFFER2 TEST_BUFFER2 TEST_BUFFER2,
                             FALSE);

  /* Test */
  output_stream = _g_file_edit (test_file, G_FILE_CREATE_NONE, NULL, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    {
      g_test_skip ("Edit is not supported.");
      return;
    }

  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_write_helper (test_file,
                     output_stream,
                     TEST_BUFFER "6789");
}

static void
test_edit_existent_dir (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that _g_file_edit() fails with "
                  "G_IO_ERROR_IS_DIRECTORY when there is a dir.");

  /* Prepare */
  test_file = get_test_file (test_dir, "test_edit_existent_dir", NULL, TRUE);

  /* Test */
  output_stream = _g_file_edit (test_file, G_FILE_CREATE_NONE, NULL, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    {
      g_test_skip ("Edit is not supported.");
      return;
    }

  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY);
  g_assert_null (output_stream);

  /* Clean */
  g_file_delete (test_file, NULL, NULL);
}

static void
test_edit_seek (gconstpointer user_data)
{
  g_autoptr(GError) error = NULL;
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that seek works on a stream created by "
                  "_g_file_edit().");

  /* Prepare */
  test_file = get_test_file (test_dir, "test_edit_seek", NULL, FALSE);

  /* Test */
  output_stream = _g_file_edit (test_file, G_FILE_CREATE_NONE, NULL, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    {
      g_test_skip ("Edit is not supported.");
      return;
    }

  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_seek_helper (test_file, output_stream, "abcde1ghijk2mnopqrstu3wxyz");
}

static void
test_edit_truncate (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that truncate works on a stream created by "
                  "_g_file_edit().");

  /* Prepare */
  test_file = get_test_file (test_dir,
                             "test_edit_truncate",
                             TEST_BUFFER3,
                             FALSE);

  /* Test */
  output_stream = _g_file_edit (test_file, G_FILE_CREATE_NONE, NULL, &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    {
      g_test_skip ("Edit is not supported.");
      return;
    }

  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_truncate_helper (test_file,
                        output_stream,
                        "abcde\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                        TEST_BUFFER2,
                        32);
}

static void
test_edit_tell (gconstpointer user_data)
{
  g_autoptr(GError) error = NULL;
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that offset is correctly reported on a stream "
                   "created by _g_file_edit().");

  /* Prepare */
  test_file = get_test_file (test_dir, "test_edit_tell", TEST_BUFFER, FALSE);

  /* Test */
  output_stream = _g_file_edit (test_file,
                                G_FILE_CREATE_NONE,
                                NULL,
                                &error);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    {
      g_test_skip ("Edit is not supported.");
      return;
    }

  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  if (!g_seekable_can_seek (G_SEEKABLE (output_stream)))
    {
      g_test_skip ("Seek is not supported.");
      return;
    }

  g_assert_cmpint (g_seekable_tell (G_SEEKABLE (output_stream)),
                   ==,
                   0);
}

static void
test_replace_nonexistent (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that g_file_replace() creates a file when it "
                  "doesn't exist yet.");

  /* Prepare */
  test_file = get_test_file (test_dir,
                             "test_replace_nonexistent",
                             NULL,
                             FALSE);

  /* Test */
  output_stream = g_file_replace (test_file,
                                  NULL,
                                  FALSE,
                                  G_FILE_CREATE_NONE,
                                  NULL,
                                  &error);
  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_write_helper (test_file, output_stream, TEST_BUFFER);
}

static void
test_replace_existent_file (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that g_file_replace() replaces data when the "
                  "file already exists.");

  /* Prepare */
  test_file = get_test_file (test_dir,
                             "test_replace_existent_file",
                             TEST_BUFFER2,
                             FALSE);

  /* Test */
  output_stream = g_file_replace (test_file,
                                  NULL,
                                  FALSE,
                                  G_FILE_CREATE_NONE,
                                  NULL,
                                  &error);
  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_write_helper (test_file,
                     output_stream,
                     TEST_BUFFER);
}

static void
test_replace_existent_dir (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that g_file_replace() fails with "
                  "G_IO_ERROR_IS_DIRECTORY when there is a dir.");

  /* Prepare */
  test_file = get_test_file (test_dir,
                             "test_replace_existent_dir",
                             NULL,
                             TRUE);

  /* Test */
  output_stream = g_file_replace (test_file,
                                  NULL,
                                  FALSE,
                                  G_FILE_CREATE_NONE,
                                  NULL,
                                  &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY);
  g_assert_null (output_stream);

  /* Clean */
  g_file_delete (test_file, NULL, NULL);
}

static void
test_replace_seek (gconstpointer user_data)
{
  g_autoptr(GError) error = NULL;
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that seek works on a stream created by "
                  "g_file_replace().");

  /* Prepare */
  test_file = get_test_file (test_dir, "test_repalce_seek", NULL, FALSE);

  /* Test */
  output_stream = g_file_replace (test_file,
                                  NULL,
                                  FALSE,
                                  G_FILE_CREATE_NONE,
                                  NULL,
                                  &error);
  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_seek_helper (test_file, output_stream, "abcde1ghijk2mnopqrstu3wxyz");
}

static void
test_replace_truncate (gconstpointer user_data)
{
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that truncate works on a stream created by "
                  "g_file_replace().");

  /* Prepare */
  test_file = get_test_file (test_dir,
                             "test_replace_truncate",
                             TEST_BUFFER3,
                             FALSE);

  /* Test */
  output_stream = g_file_replace (test_file,
                                  NULL,
                                  FALSE,
                                  G_FILE_CREATE_NONE,
                                  NULL,
                                  &error);
  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  test_truncate_helper (test_file,
                        output_stream,
                        "abcde\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"
                        TEST_BUFFER2,
                        32);
}

static void
test_replace_tell (gconstpointer user_data)
{
  g_autoptr(GError) error = NULL;
  GFile *test_dir = G_FILE (user_data);
  g_autoptr(GFile) test_file = NULL;
  g_autoptr(GFileOutputStream) output_stream = NULL;

  g_test_summary ("It verifies that offset is correctly reported on a stream "
                   "created by g_file_replace().");

  /* Prepare */
  test_file = get_test_file (test_dir, "test_replace_tell", TEST_BUFFER, FALSE);

  /* Test */
  output_stream = g_file_replace (test_file,
                                  NULL,
                                  FALSE,
                                  G_FILE_CREATE_NONE,
                                  NULL,
                                  &error);
  g_assert_no_error (error);
  g_assert_nonnull (output_stream);

  if (!g_seekable_can_seek (G_SEEKABLE (output_stream)))
    {
      g_test_skip ("Seek is not supported.");
      return;
    }

  g_assert_cmpint (g_seekable_tell (G_SEEKABLE (output_stream)),
                   ==,
                   0);
}

int
main (int argc, char *argv[])
{
  g_autoptr(GFile) test_dir = NULL;

  g_test_init (&argc, &argv, NULL);

  if (argc < 2)
    {
      g_printerr ("ERROR: Test URI is not specified\n");

      return 99;
    }

  test_dir = g_file_new_for_commandline_arg (argv[1]);

  g_test_add_data_func ("/write/create-nonexistent",
                        test_dir,
                        test_create_nonexistent);
  g_test_add_data_func ("/write/create-existent-file",
                        test_dir,
                        test_create_existent_file);
  g_test_add_data_func ("/write/create-existent-dir",
                        test_dir,
                        test_create_existent_dir);
  g_test_add_data_func ("/write/create-seek",
                        test_dir,
                        test_create_seek);
  g_test_add_data_func ("/write/create-truncate",
                        test_dir,
                        test_create_truncate);
  g_test_add_data_func ("/write/create-tell",
                        test_dir,
                        test_create_tell);

  g_test_add_data_func ("/write/append-nonexistent",
                        test_dir,
                        test_append_nonexistent);
  g_test_add_data_func ("/write/append-existent-file",
                        test_dir,
                        test_append_existent_file);
  g_test_add_data_func ("/write/append-existent-dir",
                        test_dir,
                        test_append_existent_dir);
  g_test_add_data_func ("/write/append-seek",
                        test_dir,
                        test_append_seek);
  g_test_add_data_func ("/write/append-truncate",
                        test_dir,
                        test_append_truncate);
  g_test_add_data_func ("/write/append-tell",
                        test_dir,
                        test_append_tell);

  if (!g_file_has_uri_scheme (test_dir, "file"))
    {
      g_test_add_data_func ("/write/edit-nonexistent",
                            test_dir,
                            test_edit_nonexistent);
      g_test_add_data_func ("/write/edit-existent-file",
                            test_dir,
                            test_edit_existent_file);
      g_test_add_data_func ("/write/edit-existent-dir",
                            test_dir,
                            test_edit_existent_dir);
      g_test_add_data_func ("/write/edit-seek",
                            test_dir,
                            test_edit_seek);
      g_test_add_data_func ("/write/edit-truncate",
                            test_dir,
                            test_edit_truncate);
      g_test_add_data_func ("/write/edit-tell",
                            test_dir,
                            test_edit_tell);
    }

  g_test_add_data_func ("/write/replace-nonexistent",
                        test_dir,
                        test_replace_nonexistent);
  g_test_add_data_func ("/write/replace-existent-file",
                        test_dir,
                        test_replace_existent_file);
  g_test_add_data_func ("/write/replace-existent-dir",
                        test_dir,
                        test_replace_existent_dir);
  g_test_add_data_func ("/write/replace-seek",
                        test_dir,
                        test_replace_seek);
  g_test_add_data_func ("/write/replace-truncate",
                        test_dir,
                        test_replace_truncate);
  g_test_add_data_func ("/write/replace-tell",
                        test_dir,
                        test_replace_tell);

  return g_test_run ();
}
