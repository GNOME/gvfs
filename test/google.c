/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2019 Mayank Sharma
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Mayank Sharma <mayank8019@gmail.com>
 */

#include <gio/gio.h>
#include <glib.h>
#include <locale.h>

#define GOA_API_IS_SUBJECT_TO_CHANGE
#include <gdata/gdata.h>
#include <goa/goa.h>

#define GOOGLE_TEST_DIRECTORY "test-google"

#define OP_COPY_TEST_DIRECTORY "Test-Copy-Dir"
#define OP_MOVE_TEST_DIRECTORY "Test-Move-Dir"

#define TITLE_DUMMY_FILE "Dummy-File"
#define TITLE_COPIED_FILE "Copied-File"

struct GVfsGoogleTestClass {
  GDataDocumentsService     *service;
  GDataDocumentsEntry       *test_dir_entry;
  GDataAuthorizationDomain  *domain;
  GMount                    *mount;
  GFile                     *root;                /* GFile corresponding to root-level directory */
  GFile                     *test_dir;            /* GFile corresponding to test_dir_entry */
  GFile                     *test_dummy_file;     /* A GFile inside of test_dir which can be used for
                                                 testing copy/move operations*/
};

/* static GDataDocumentsEntry *
 * create_dummy_test_file (GDataDocumentsService *service, GDataDocumentsEntry *parent, GError **error)
 * {
 *   static guint dummy_file_counter = 0;
 *   const gchar *buf = "Foobar";
 *   gssize bytes_written;
 *   GDataDocumentsDocument *new_document = NULL;
 *   GDataUploadStream *ostream = NULL;
 *   g_autofree gchar *counter_buf = NULL;
 *   g_autofree gchar *title = NULL;
 *   g_autoptr(GDataDocumentsDocument) document = NULL;
 *   g_autoptr(GError) child_error = NULL;
 * 
 *   g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (service), NULL);
 *   g_return_val_if_fail (error != NULL, NULL);
 * 
 *   counter_buf = g_malloc (G_ASCII_DTOSTR_BUF_SIZE);
 *   counter_buf = g_ascii_dtostr (counter_buf, G_ASCII_DTOSTR_BUF_SIZE, (gdouble) ++dummy_file_counter);
 *   title = g_strconcat (TITLE_DUMMY_FILE, counter_buf, NULL);
 * 
 *   document = gdata_documents_document_new (NULL);
 *   gdata_entry_set_title (GDATA_ENTRY (document), title);
 * 
 *   ostream = gdata_documents_service_upload_document (service,
 *                                                      document,
 *                                                      title,
 *                                                      "text/plain",
 *                                                      GDATA_DOCUMENTS_FOLDER (parent),
 *                                                      NULL,
 *                                                      &child_error);
 *   if (child_error != NULL)
 *     {
 *       g_propagate_error (error, g_steal_pointer (&child_error));
 *       return NULL;
 *     }
 *   g_assert_nonnull (ostream);
 * 
 *   bytes_written = g_output_stream_write (G_OUTPUT_STREAM (ostream), buf, strlen (buf), NULL, &child_error);
 *   if (child_error != NULL)
 *     {
 *       g_propagate_error (error, g_steal_pointer (&child_error));
 *       return NULL;
 *     }
 * 
 *   g_output_stream_close (G_OUTPUT_STREAM (ostream), NULL, &child_error);
 *   if (child_error != NULL)
 *     {
 *       g_propagate_error (error, g_steal_pointer (&child_error));
 *       return NULL;
 *     }
 * 
 *   new_document = gdata_documents_service_finish_upload (service, ostream, &child_error);
 *   if (child_error != NULL)
 *     {
 *       g_propagate_error (error, g_steal_pointer (&child_error));
 *       return NULL;
 *     }
 * 
 *   return GDATA_DOCUMENTS_ENTRY (new_document);
 * } */

/* ---------------------------------------------------------------------------------------------------- */
/* Helper functions begin */
/* ---------------------------------------------------------------------------------------------------- */

static GDataDocumentsEntry *
create_directory (GDataDocumentsService *service, GDataDocumentsEntry *parent, const gchar *title, GError **error)
{
  g_return_val_if_fail (GDATA_IS_DOCUMENTS_SERVICE (service), NULL);
  g_return_val_if_fail (error != NULL, NULL);

  GDataDocumentsEntry *new_folder;
  g_autoptr(GError) child_error = NULL;
  g_autoptr(GDataDocumentsFolder) dummy_new_folder = NULL;

  /* Create the new folder */
  dummy_new_folder = gdata_documents_folder_new (NULL);
  gdata_entry_set_title (GDATA_ENTRY (dummy_new_folder), title);

  /* Insert the folder */
  new_folder = gdata_documents_service_add_entry_to_folder (service,
                                                            GDATA_DOCUMENTS_ENTRY (dummy_new_folder),
                                                            GDATA_DOCUMENTS_FOLDER (parent),
                                                            NULL,
                                                            &child_error);
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return NULL;
    }

  return new_folder;
}

static gchar *
get_file_attribute (GFile *file, const char *attribute, GError **error)
{
  g_autoptr(GFileInfo) info = NULL;
  g_autoptr(GError) child_error = NULL;

  g_return_val_if_fail (G_IS_FILE (file), NULL);
  g_return_val_if_fail (error != NULL && *error == NULL, NULL);

  info = g_file_query_info (file,
                            attribute,
                            G_FILE_QUERY_INFO_NONE,
                            NULL,
                            &child_error);
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return FALSE;
    }

  return g_file_info_get_attribute_as_string (info, attribute);
}

static gboolean
delete_file_recursively (GFile *file)
{
  gboolean success;
  g_autoptr (GError) error = NULL;

  do
    {
      g_autoptr (GFileEnumerator) enumerator = NULL;

      success = g_file_delete (file, NULL, &error);
      if (success ||
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_EMPTY))
        {
          break;
        }

      g_clear_error (&error);
      enumerator = g_file_enumerate_children (file,
                                              G_FILE_ATTRIBUTE_STANDARD_NAME,
                                              G_FILE_QUERY_INFO_NONE,
                                              NULL, &error);

      if (enumerator)
        {
          GFileInfo *info;

          success = TRUE;
          info = g_file_enumerator_next_file (enumerator,
                                              NULL,
                                              &error);

          while (info != NULL)
            {
              g_autoptr (GFile) child = NULL;

              child = g_file_enumerator_get_child (enumerator, info);
              success = success && delete_file_recursively (child);
              g_object_unref (info);

              info = g_file_enumerator_next_file (enumerator, NULL, &error);
            }
        }

      if (error != NULL)
        success = FALSE;
    }
  while (success);

  return success;
}

static gboolean
delete_and_make_new_directory (GFile *folder, GError **error)
{
  gboolean retval = TRUE;
  g_autoptr(GError) child_error = NULL;

  while (g_file_make_directory (folder, NULL, &child_error) == FALSE)
    {
      gboolean needs_delete = FALSE;

      if (child_error != NULL)
        {
          if (child_error->code != G_IO_ERROR_EXISTS)
            {
              g_propagate_error (error, g_steal_pointer (&child_error));
              return FALSE;
            }
          else
            needs_delete = TRUE;
        }

      child_error = NULL;
      if (needs_delete)
        {
          g_file_delete (folder, NULL, &child_error);
          if (child_error != NULL)
            {
              if (child_error->code != G_IO_ERROR_NOT_EMPTY)
                {
                  retval = FALSE;
                  g_propagate_error (error, g_steal_pointer (&child_error));
                  break;
                }

              retval = delete_file_recursively (folder);
            }
        }
    }

  return retval;
}

/* ---------------------------------------------------------------------------------------------------- */
/* Test init and cleanup functions begin */
/* ---------------------------------------------------------------------------------------------------- */

static inline void
_setup_google_mount_and_libgdata_service (struct GVfsGoogleTestClass *self, GError **error)
{
  GList *l, *mounts;
  const gchar *google_account_email = NULL;
  GoaObject *test_account_goa_object = NULL;
  g_autoptr(GoaClient) client = NULL;
  g_autoptr(GError) child_error = NULL;
  g_autoptr(GDataGoaAuthorizer) authorizer = NULL;
  g_autolist(GoaObject) accounts = NULL;
  g_autoptr(GVolumeMonitor) volume_monitor = NULL;

  /* Basic setup pertaining to GOA, and creation of GDataDocumentsService */
  client = goa_client_new_sync (NULL, &child_error);
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return;
    }

  accounts = goa_client_get_accounts (client);
  for (l = accounts; l != NULL; l = l->next)
    {
      GoaObject *object;
      GoaAccount *account;

      object = GOA_OBJECT (l->data);
      account = goa_object_peek_account (object);

      if (g_strcmp0 (goa_account_get_provider_type (account), "google") == 0)
        {
          test_account_goa_object = GOA_OBJECT (l->data);
          break;
        }
    }

  if (test_account_goa_object == NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return;
    }

  authorizer = gdata_goa_authorizer_new (test_account_goa_object);
  self->service = gdata_documents_service_new (GDATA_AUTHORIZER (authorizer));
  if (self->service == NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return;
    }

  self->domain = gdata_documents_service_get_primary_authorization_domain ();
  volume_monitor = g_volume_monitor_get ();
  mounts = g_volume_monitor_get_mounts (volume_monitor);
  google_account_email = goa_account_get_identity (goa_object_peek_account (test_account_goa_object));

  for (l = mounts; l != NULL; l = l->next)
    {
      g_autofree gchar *mount_name = NULL;

      mount_name = g_mount_get_name (G_MOUNT (l->data));
      if (g_strcmp0 (google_account_email, mount_name) == 0)
        {
          self->mount = g_object_ref (G_MOUNT (l->data));
          g_message ("Mount: %s", mount_name);
          break;
        }
    }

  if (self->mount == NULL)
    {
      g_error ("No GMount found");
      return;
    }
  g_list_free_full (mounts, g_object_unref);
}

static void
gvfs_google_test_init (struct GVfsGoogleTestClass **_self, GError **error)
{
  gboolean op_done;
  g_autofree gchar *root_path = NULL;
  g_autofree gchar *test_dir_path = NULL;
  g_autofree gchar *test_dir_id = NULL;
  g_autofree gchar *test_dummy_file_id = NULL;
  g_autoptr(GError) child_error = NULL;
  g_autoptr(GFile) root = NULL;
  g_autoptr(GFile) test_dir = NULL;
  g_autoptr(GFile) test_dummy_file = NULL;
  g_autoptr(GFile) copy_test_dir = NULL;
  g_autoptr(GFile) move_test_dir = NULL;
  g_autoptr(GFileOutputStream) file_write_stream = NULL;
  g_autoptr(GDataDocumentsEntry) root_entry = NULL;
  struct GVfsGoogleTestClass *self;

  *_self = g_malloc (sizeof (struct GVfsGoogleTestClass));
  self = *_self;
  self->mount = NULL;

  _setup_google_mount_and_libgdata_service (self, &child_error);
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return;
    }

  root_entry = GDATA_DOCUMENTS_ENTRY (gdata_service_query_single_entry (GDATA_SERVICE (self->service),
                                                                        self->domain,
                                                                        "root",
                                                                        NULL,
                                                                        GDATA_TYPE_DOCUMENTS_FOLDER,
                                                                        NULL,
                                                                        &child_error));
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return;
    }

  /* The GFile corresponding to the root_entry we fetched above */
  root = g_mount_get_root (self->mount);

  root_path = g_file_get_path (root);
  test_dir = g_file_new_build_filename (root_path, GOOGLE_TEST_DIRECTORY, NULL);

  /* We create a test directory in the root folder (on Google Drive) where all
   * the other test files/folders shall be created */
  g_file_make_directory (test_dir, NULL, &child_error);
  if (child_error != NULL)
    {
      if (child_error->code != G_IO_ERROR_EXISTS)
        {
          g_propagate_error (error, g_steal_pointer (&child_error));
          return;
        }
      else
        child_error = NULL;
    }

  g_object_unref (test_dir);
  test_dir = g_file_get_child_for_display_name (root, GOOGLE_TEST_DIRECTORY, &child_error);
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return;
    }

  self->test_dir = g_object_ref (test_dir);

  test_dir_id = get_file_attribute (self->test_dir, G_FILE_ATTRIBUTE_ID_FILE, &child_error);
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return;
    }

  self->test_dir_entry = GDATA_DOCUMENTS_ENTRY (gdata_service_query_single_entry (GDATA_SERVICE (self->service),
                                                                                  self->domain,
                                                                                  test_dir_id,
                                                                                  NULL,
                                                                                  GDATA_TYPE_DOCUMENTS_FOLDER,
                                                                                  NULL,
                                                                                  &child_error));
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return;
    }

  g_message ("Test folder GFile ID: %s", test_dir_id);
  g_message ("Test folder Entry ID: %s", gdata_entry_get_id (GDATA_ENTRY (self->test_dir_entry)));

  /* Now we create try to create a dummy file which we'll use for testing
   * copy/move/delete operations.If the file already exists, an error with
   * error code G_IO_ERROR_EXISTS is returned, and we simply use that existing
   * file for testing purposes. */
  test_dir_path = g_file_get_path (self->test_dir);
  test_dummy_file = g_file_new_build_filename (test_dir_path, TITLE_DUMMY_FILE, NULL);

  file_write_stream = g_file_create (test_dummy_file, G_FILE_CREATE_NONE, NULL, &child_error);
  if (child_error != NULL)
    {
      if (child_error->code != G_IO_ERROR_EXISTS)
        {
          g_propagate_error (error, g_steal_pointer (&child_error));
          return;
        }
      else
        {
          /* We simply ignore this because if file already exists, we don't
           * have any issues for initiating the test. */
          child_error = NULL;
        }
    }
  else
    {
      g_output_stream_write (G_OUTPUT_STREAM (file_write_stream), "SomeRandomText", 14, NULL, &child_error);
      if (child_error != NULL)
        {
          g_propagate_error (error, g_steal_pointer (&child_error));
          return;
        }

      g_output_stream_close (G_OUTPUT_STREAM (file_write_stream), NULL, &child_error);
      if (child_error != NULL)
        {
          g_propagate_error (error, g_steal_pointer (&child_error));
          return;
        }
    }

  g_object_unref (test_dummy_file);
  test_dummy_file = g_file_get_child_for_display_name (self->test_dir, TITLE_DUMMY_FILE, &child_error);
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return;
    }

  test_dummy_file_id = get_file_attribute (test_dummy_file, G_FILE_ATTRIBUTE_ID_FILE, &child_error);
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return;
    }

  g_message ("Test dummy GFile ID: %s", test_dummy_file_id);
  self->test_dummy_file = g_object_ref (test_dummy_file);

  copy_test_dir = g_file_new_build_filename (test_dir_path, OP_COPY_TEST_DIRECTORY, NULL);
  op_done = delete_and_make_new_directory (copy_test_dir, &child_error);
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return;
    }
  g_assert_true (op_done);

  move_test_dir = g_file_new_build_filename (test_dir_path, OP_MOVE_TEST_DIRECTORY, NULL);
  op_done = delete_and_make_new_directory (move_test_dir, &child_error);
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return;
    }
  g_assert_true (op_done);
}

static void
gvfs_google_test_exit_cleanup (struct GVfsGoogleTestClass **_self, GError **error)
{
  /* g_autoptr(GError) child_error = NULL; */
  struct GVfsGoogleTestClass *self = *_self;

  g_return_if_fail (GDATA_IS_DOCUMENTS_ENTRY (self->test_dir_entry));
  g_return_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self->service));

  /* gdata_service_delete_entry (GDATA_SERVICE (self->service),
   *                             gdata_documents_service_get_primary_authorization_domain (),
   *                             GDATA_ENTRY (self->test_dir_entry),
   *                             NULL,
   *                             &child_error);
   * if (child_error != NULL)
   *   {
   *     g_propagate_error (error, g_steal_pointer (&child_error));
   *     return;
   *   } */

  g_object_unref (self->service);
  g_object_unref (self->test_dir_entry);
  g_object_unref (self->mount);
  g_object_unref (self->test_dir);
  g_free (self);
}

/* ---------------------------------------------------------------------------------------------------- */
/* Actual test functions begin */
/* ---------------------------------------------------------------------------------------------------- */

static void
gvfs_google_test_make_directory_using_valid_display_name (gconstpointer user_data)
{
  struct GVfsGoogleTestClass *self = (struct GVfsGoogleTestClass *) user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) dummy_new_folder = NULL;
  g_autoptr(GFile) actual_new_folder = NULL;
  g_autofree gchar *parent_path = NULL;
  const gchar *folder_display_name = "Valid-Display-Name-Dir";

  parent_path = g_file_get_path (self->test_dir);
  dummy_new_folder = g_file_new_build_filename (parent_path, folder_display_name, NULL);

  g_assert_true (delete_and_make_new_directory (dummy_new_folder, &error));
  g_assert_no_error (error);

  actual_new_folder = g_file_get_child_for_display_name (self->test_dir, folder_display_name, &error);
  g_assert_no_error (error);

  g_assert_true (g_file_equal (dummy_new_folder, actual_new_folder));
}

static void
gvfs_google_test_make_directory_using_valid_id (gconstpointer user_data)
{
  struct GVfsGoogleTestClass *self = (struct GVfsGoogleTestClass *) user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) dummy_new_folder = NULL;
  g_autoptr(GFile) actual_new_folder1 = NULL;
  g_autoptr(GFile) actual_new_folder2 = NULL;
  g_autoptr(GFileInfo) info1 = NULL;
  g_autoptr(GFileInfo) info2 = NULL;
  g_autofree gchar *parent_path = NULL;
  const gchar *intended_folder_title = gdata_entry_get_id (GDATA_ENTRY (self->test_dir_entry));
  const gchar *actual_folder_title = gdata_entry_get_title (GDATA_ENTRY (self->test_dir_entry));

  parent_path = g_file_get_path (self->test_dir);
  dummy_new_folder = g_file_new_build_filename (parent_path, intended_folder_title, NULL);

  g_assert_true (delete_and_make_new_directory (dummy_new_folder, &error));
  g_assert_no_error (error);

  error = NULL;
  actual_new_folder1 = g_file_get_child_for_display_name (self->test_dir, intended_folder_title, &error);
  g_assert_nonnull (actual_new_folder1);
  g_assert_no_error (error);

  error = NULL;
  actual_new_folder2 = g_file_get_child_for_display_name (self->test_dir, actual_folder_title, &error);
  g_assert_nonnull (actual_new_folder2);
  g_assert_no_error (error);

  info1 = g_file_query_info (actual_new_folder1, G_FILE_ATTRIBUTE_ID_FILE, G_FILE_QUERY_INFO_NONE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (info1);

  info2 = g_file_query_info (actual_new_folder2, G_FILE_ATTRIBUTE_ID_FILE, G_FILE_QUERY_INFO_NONE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (info1);

  g_assert_cmpstr (g_file_info_get_name (info1), ==, g_file_info_get_name (info2));
}

static void
gvfs_google_test_copy_file_from_one_parent_to_other_simple (gconstpointer user_data)
{
  struct GVfsGoogleTestClass *self = (struct GVfsGoogleTestClass *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *dest_path = NULL;
  g_autofree gchar *dest_file_id = NULL;
  g_autofree gchar *copy_test_dir_id = NULL;
  g_autofree gchar *dest_file_display_name = NULL;
  g_autoptr(GFile) copy_test_dir = NULL;
  g_autoptr(GFile) dest_file = NULL;

  parent_path = g_file_get_path (self->test_dir);
  copy_test_dir = g_file_get_child_for_display_name (self->test_dir, OP_COPY_TEST_DIRECTORY, &error);
  g_assert_no_error (error);
  g_assert_nonnull (copy_test_dir);

  copy_test_dir_id = get_file_attribute (copy_test_dir, G_FILE_ATTRIBUTE_ID_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (copy_test_dir_id);

  dest_path = g_file_get_path (copy_test_dir);
  dest_file = g_file_new_build_filename (dest_path, TITLE_COPIED_FILE, NULL);

  op_done = g_file_copy (self->test_dummy_file, dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (op_done);

  dest_file_id = get_file_attribute (dest_file, G_FILE_ATTRIBUTE_ID_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (dest_file_id);
  /* g_message ("Copied File ID: %s", dest_file_id); */

  dest_file_display_name = get_file_attribute (dest_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_cmpstr (dest_file_display_name, ==, TITLE_COPIED_FILE);
}

static void
gvfs_google_test_copy_file_within_same_parent_with_title_change (gconstpointer user_data)
{
  struct GVfsGoogleTestClass *self = (struct GVfsGoogleTestClass *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *dest_file_id = NULL;
  g_autofree gchar *dest_file_title = NULL;
  g_autofree gchar *dest_file_display_name = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) dest_file = NULL;
  g_autoptr(GFile) dummy_dest_file = NULL;
  g_autoptr(GFileInfo) info = NULL;

  source_file = g_file_get_child_for_display_name (self->test_dir, TITLE_DUMMY_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  dest_file_title = g_strconcat (TITLE_DUMMY_FILE, " (Copy)", NULL);
  parent_path = g_file_get_path (self->test_dir);

  dummy_dest_file = g_file_new_build_filename (parent_path, dest_file_title, NULL);

  /* Remove all copies of files having the title same as dest_file_title.
   * We don't wish to test collision in this test-case, hence we delete other
   * files having title same as dest_file_title. If we do g_file_copy without
   * deletion, multiple files with same title can be created thereby causing
   * title collisions.
   * Also, below code is prone to TOCTOU bug but we don't care about it since
   * the tests won't be run concurrently. */
  while (g_file_query_exists (dummy_dest_file, NULL))
    {
      g_file_delete (dummy_dest_file, NULL, &error);
      g_assert_no_error (error);
    }

  dest_file = g_file_new_build_filename (parent_path, dest_file_title, NULL);

  op_done = g_file_copy (source_file, dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (op_done);

  g_clear_object (&dest_file);
  dest_file = g_file_get_child_for_display_name (self->test_dir, dest_file_title, &error);
  g_assert_no_error (error);
  g_assert_nonnull (dest_file);

  g_clear_object (&info);
  info = g_file_query_info (dest_file,
                            G_FILE_ATTRIBUTE_ID_FILE","
                            G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
                            G_FILE_QUERY_INFO_NONE,
                            NULL,
                            &error);
  g_assert_no_error (error);

  dest_file_id = g_file_info_get_attribute_as_string (info, G_FILE_ATTRIBUTE_ID_FILE);
  /* g_message ("Copied File ID: %s", dest_file_id); */

  dest_file_display_name = g_file_info_get_attribute_as_string (info, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME);
  g_assert_cmpstr (dest_file_display_name, ==, dest_file_title);
}

static void
gvfs_google_test_copy_file_within_same_parent_with_same_title (gconstpointer user_data)
{
  struct GVfsGoogleTestClass *self = (struct GVfsGoogleTestClass *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *source_file_title = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) dummy_dest_file = NULL;

  source_file = g_file_get_child_for_display_name (self->test_dir, TITLE_DUMMY_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  source_file_title = get_file_attribute (source_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file_title);

  parent_path = g_file_get_path (self->test_dir);
  dummy_dest_file = g_file_new_build_filename (parent_path, source_file_title, NULL);

  op_done = g_file_copy (source_file, dummy_dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_assert_true (!op_done);
}

static void
gvfs_google_test_copy_file_within_same_parent_try_overwrite_with_id (gconstpointer user_data)
{
  struct GVfsGoogleTestClass *self = (struct GVfsGoogleTestClass *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *source_file_id = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) dummy_dest_file = NULL;

  source_file = g_file_get_child_for_display_name (self->test_dir, TITLE_DUMMY_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  source_file_id = get_file_attribute (source_file, G_FILE_ATTRIBUTE_ID_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file_id);

  parent_path = g_file_get_path (self->test_dir);
  dummy_dest_file = g_file_new_build_filename (parent_path, source_file_id, NULL);

  op_done = g_file_copy (source_file, dummy_dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_true (!op_done);
}

static void
gvfs_google_test_copy_file_within_same_parent_simple_with_source_id_as_destination_basename (gconstpointer user_data)
{
  struct GVfsGoogleTestClass *self = (struct GVfsGoogleTestClass *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *source_file_id = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) dummy_dest_file = NULL;

  source_file = g_file_get_child_for_display_name (self->test_dir, TITLE_DUMMY_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  source_file_id = get_file_attribute (source_file, G_FILE_ATTRIBUTE_ID_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file_id);

  parent_path = g_file_get_path (self->test_dir);
  dummy_dest_file = g_file_new_build_filename (parent_path, source_file_id, NULL);

  op_done = g_file_copy (source_file, dummy_dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_assert_true (!op_done);
}

static void
gvfs_google_test_copy_file_within_same_parent_try_overwrite_with_same_title (gconstpointer user_data)
{
  struct GVfsGoogleTestClass *self = (struct GVfsGoogleTestClass *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *source_file_title = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) dummy_dest_file = NULL;

  source_file = g_file_get_child_for_display_name (self->test_dir, TITLE_DUMMY_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  source_file_title = get_file_attribute (source_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file_title);

  parent_path = g_file_get_path (self->test_dir);
  dummy_dest_file = g_file_new_build_filename (parent_path, source_file_title, NULL);

  op_done = g_file_copy (source_file, dummy_dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_true (!op_done);
}

static void
gvfs_google_test_copy_file_from_one_parent_to_other_with_volatile_entry_collision_only (gconstpointer user_data)
{
  struct GVfsGoogleTestClass *self = (struct GVfsGoogleTestClass *) user_data;
  gboolean op_done;
  gchar rand_int_string[G_ASCII_DTOSTR_BUF_SIZE];
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) copy_test_dir = NULL;
  g_autoptr(GFile) dummy_dest_file = NULL;

  copy_test_dir = g_file_get_child_for_display_name (self->test_dir, OP_COPY_TEST_DIRECTORY, &error);
  g_assert_no_error (error);
  g_assert_nonnull (copy_test_dir);

  source_file = g_file_get_child_for_display_name (self->test_dir, TITLE_DUMMY_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  /* Generate any random string (here created using a random integer) and use
   * this random string as the title of the destination file. We do this so
   * that we can cause a collision just on the volatile entry and not on the
   * title. */
  g_ascii_dtostr (rand_int_string, G_ASCII_DTOSTR_BUF_SIZE, rand ());

  parent_path = g_file_get_path (copy_test_dir);
  dummy_dest_file = g_file_new_build_filename (parent_path, rand_int_string, NULL);

  op_done = g_file_copy (source_file, dummy_dest_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (op_done);
}

gint
main (gint argc, gchar *argv [])
{
  gint retval = 0;
  struct GVfsGoogleTestClass *self;
  g_autoptr(GError) error = NULL;

  setlocale (LC_ALL, "");

  g_test_init (&argc, &argv, NULL);

  gvfs_google_test_init (&self, &error);
  if (error != NULL)
    {
      g_warning ("Error (init): %s", error->message);
      return 1;
    }

  /* Test Scenario: We try to create a folder with its title set to the string
   * "valid_display_name_directory".
   *
   * Expected Behaviour: The newly created folder has the title
   * "valid_display_name_directory".
   *
   * Actual Behaviour is same as expected behaviour. */
  g_test_add_data_func ("/make_directory/using_valid_display_name",
                        (gconstpointer) self,
                        gvfs_google_test_make_directory_using_valid_display_name);

  /* Test Scenario: We try to create a folder having the title of the folder set
   * to some other file/folder's ID. So, we try to create a new folder with
   * self->test_dir_entry's ID.
   *
   * Expected Behaviour: The newly created folder has the same title as
   * self->test_dir_entry's ID.
   *
   * Actual Behaviour: The newly created folder gets its title set to
   * self->test_dir_entry's title and *NOT* its ID. */
  g_test_add_data_func ("/make_directory/using_valid_id_of_other_directory",
                        (gconstpointer) self,
                        gvfs_google_test_make_directory_using_valid_id);

  /* The below test case is equivalent to the following commandline operations:
   * (Assume that file with ID `id1` has the title $T)
   *
   * `gio copy id1 ./"$T (copy)"`           (where id2 is the ID of a folder)
   *
   * This operation mimics what nautilus does when we do a Ctrl+C & Ctrl+V
   * operation on a file inside same parent directory. It simply appends a
   * " (copy)" to the name of the file. */
  g_test_add_data_func ("/copy_file/within_same_parent_with_title_change",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_within_same_parent_with_title_change);

  /* We try to copy a file within the same parent without changing the title, i.e. this
   * operation corresponds to `gio copy ./file1_title ./file1_title` but without overwrite.
   *
   * This operation should supposedly fail with error code `G_IO_ERROR_EXISTS`
   * and string "Target file already exists". */
  g_test_add_data_func ("/copy_file/within_same_parent_simple_without_title_change",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_within_same_parent_with_same_title);

  /* We try to copy a file within the same parent and keeping the title of file
   * to be same as the ID of source file, i.e. this operation corresponds to
   * `gio copy ./id1 ./id1` but without overwrite.
   *
   * This operation should supposedly fail with error code `G_IO_ERROR_EXISTS`
   * and string "Target file already exists". */
  g_test_add_data_func ("/copy_file/within_same_parent_simple_with_source_id_as_destination_basename",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_within_same_parent_simple_with_source_id_as_destination_basename);

  /* The below test case is equivalent to the following commandline operations:
   *
   * `gio copy ./id1 ./$Title$` where $Title$ is the title of file with ID `id1`.
   *
   * This operation should supposedly fail when done over the commandline gio
   * tool with error code `G_IO_ERROR_FAILED` and string "Operation Not Supported". */
  g_test_add_data_func ("/copy_file/within_same_parent_simple_try_overwrite_with_same_title",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_within_same_parent_try_overwrite_with_same_title);

  /* The below test case is equivalent to the following commandline operations:
   *
   * `gio copy id1 ./id1`
   *
   * This operation should supposedly fail when done over the commandline gio
   * tool with error code `G_IO_ERROR_FAILED` and string "Operation Not Supported". */
  g_test_add_data_func ("/copy_file/within_same_parent_simple_try_overwrite_with_id",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_within_same_parent_try_overwrite_with_id);

  /* The below test case is equivalent to the following commandline operations:
   * `gio copy id1 id2/`  (where id2 is the ID of a folder) */
  g_test_add_data_func ("/copy_file/from_one_parent_to_other_simple",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_from_one_parent_to_other_simple);

  /* The below test cases causes just a collision on the volatile entry. This
   * is because the previously executed test copies a file into the OP_COPY_TEST_DIRECTORY.
   * So, one volatile entry was already there because of that file, and we
   * change the title here to only cause a collision on the volatile entry (and
   * not the title too).
   *
   * It basically mimics the following case:
   * `gio copy id1 id2/some_title`
   * `gio copy id1 id2/some_random_title` */
  g_test_add_data_func ("/copy_file/from_one_parent_to_other_with_volatile_entry_collision_only",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_from_one_parent_to_other_with_volatile_entry_collision_only);

  /* The below test case uses the same function as the test case with
   * identifier "/copy_file/from_one_parent_to_other_simple" because copying
   * file from source to destination is same as producing both kinds of
   * collisions at once.
   *
   * It basically mimics the following case:
   * `gio copy id1 id2/some_title`
   * `gio copy id1 id2/some_title` */
  g_test_add_data_func ("/copy_file/from_one_parent_to_other_with_both_title_and_volatile_entry_collision",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_from_one_parent_to_other_simple);

  retval = g_test_run ();

  gvfs_google_test_exit_cleanup (&self, &error);
  if (error != NULL)
    {
      g_warning ("Error (cleanup): %s", error->message);
      retval = 1;
    }

  return retval;
}
