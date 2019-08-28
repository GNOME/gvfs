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
#define TITLE_MOVED_FILE "Moved-File"

typedef struct {
  GDataDocumentsService     *service;
  GDataDocumentsEntry       *test_dir_entry;
  GDataAuthorizationDomain  *domain;
  GMount                    *mount;
  GFile                     *root;                /* GFile corresponding to root-level directory */
  GFile                     *test_dir;            /* GFile corresponding to test_dir_entry */
  GFile                     *test_dummy_file;     /* A GFile inside of test_dir which can be used for
                                                 testing copy/move operations*/
} GVfsGoogleTestData ;

typedef struct
{
  GAsyncResult *res;
  GMainLoop *loop;

} MountData;

/* ---------------------------------------------------------------------------------------------------- */
/* Helper functions begin */
/* ---------------------------------------------------------------------------------------------------- */

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
        break;

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

static void
volume_mount_cb (GVolume           *volume,
                 GAsyncResult      *res,
                 MountData         *user_data)
{
  MountData *data = user_data;
  data->res = g_object_ref (res);
  g_main_loop_quit (data->loop);
}

static gboolean
volume_mount_sync (GVolume *volume, GMountMountFlags flags, GCancellable *cancellable, GError **error)
{
  g_autoptr (GMainContext) context  = NULL;
  g_autoptr (GMainLoop) loop = NULL;
  g_autoptr(GError) child_error = NULL;
  MountData data;
  gboolean retval = FALSE;

  context = g_main_context_new ();
  loop = g_main_loop_new (context, FALSE);
  g_main_context_push_thread_default (context);
  data.loop = loop;

  g_volume_mount (volume,
                  flags,
                  NULL,
                  cancellable,
                  (GAsyncReadyCallback) volume_mount_cb,
                  (gpointer) &data);

  g_main_loop_run (loop);

  retval = g_volume_mount_finish (volume, data.res, &child_error);

  if (child_error != NULL)
    g_propagate_error (error, g_steal_pointer (&child_error));

  g_object_unref (data.res);
  g_main_context_pop_thread_default (context);
  return retval;
}

static GFile *
create_temporary_duplicate_file (GFile *source_file, GError **error)
{
  static int file_num_counter = 0;
  g_autofree gchar *dest_file_title = NULL;
  g_autofree gchar *source_file_title = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autoptr(GError) child_error = NULL;
  GFile *dest_file = NULL;
  g_autoptr(GFile) dummy_dest_file = NULL;
  g_autoptr(GFile) parent = NULL;
  gchar rand_int_string[G_ASCII_DTOSTR_BUF_SIZE];

  /* Generate a string (here created using the stringified file_num_counter) and use
   * this string as the title of the destination file. */
  g_ascii_dtostr (rand_int_string, G_ASCII_DTOSTR_BUF_SIZE, ++file_num_counter);

  source_file_title = get_file_attribute (source_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &child_error);
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return NULL;
    }

  dest_file_title = g_strconcat (source_file_title, " (", rand_int_string, ")", NULL);

  parent = g_file_get_parent (source_file);
  parent_path = g_file_get_path (parent);
  dummy_dest_file = g_file_new_build_filename (parent_path, dest_file_title, NULL);

  g_file_copy (source_file, dummy_dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &child_error);
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return NULL;
    }

  dest_file = g_file_get_child_for_display_name (parent, dest_file_title, &child_error);
  if (child_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&child_error));
      return NULL;
    }

  return dest_file;
}

/* ---------------------------------------------------------------------------------------------------- */
/* Test init and cleanup functions begin */
/* ---------------------------------------------------------------------------------------------------- */

static inline void
setup_google_mount_and_libgdata_service (GVfsGoogleTestData *self, GError **error)
{
  GList *l;
  GoaObject *test_account_goa_object = NULL;
  g_autofree gchar *mount_email = NULL;
  g_autofree gchar *volume_uuid = NULL;
  g_autolist(GoaObject) accounts = NULL;
  g_autolist(GMount) mounts = NULL;
  g_autoptr(GDataGoaAuthorizer) authorizer = NULL;
  g_autoptr(GError) child_error = NULL;
  g_autoptr(GVolumeMonitor) volume_monitor = NULL;
  g_autoptr(GVolume) volume = NULL;
  g_autoptr(GoaClient) client = NULL;

  /* We firstly check if there exists any mount with mount_name ending in
   * "@gmail.com" */
  volume_monitor = g_volume_monitor_get ();
  mounts = g_volume_monitor_get_mounts (volume_monitor);
  for (l = mounts; l != NULL; l = l->next)
    {
      g_autofree gchar *mount_name = NULL;
      mount_name = g_mount_get_name (G_MOUNT (l->data));
      if (g_str_has_suffix (mount_name, "@gmail.com"))
        {
          mount_email = g_strdup (mount_name);
          self->mount = g_object_ref (G_MOUNT (l->data));
        }
    }

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
      const gchar *goa_account_email;
      GoaObject *object;
      GoaAccount *account;

      object = GOA_OBJECT (l->data);
      account = goa_object_peek_account (object);

      if (g_strcmp0 (goa_account_get_provider_type (account), "google") == 0)
        {
          if (mount_email != NULL)
            {
              goa_account_email = goa_account_get_identity (account);
              if (g_strcmp0 (goa_account_email, mount_email) == 0)
                {
                  test_account_goa_object = GOA_OBJECT (l->data);
                  break;
                }
            }
          else
            {
              test_account_goa_object = GOA_OBJECT (l->data);
              mount_email = goa_account_dup_identity (account);
              break;
            }
        }
    }

  if (test_account_goa_object == NULL)
    {
      g_error ("No GOA account found with the same email as %s", mount_email);
      return;
    }

  /* We've found an email linked to some Google Account, but the GMount hasn't
   * been mounted yet. So, we manually mount a GMount corresponding to this
   * mount_email. */
  if (self->mount == NULL)
    {
      g_autoptr(GFile) volume_activation_root = NULL;
      volume_uuid = g_strconcat ("google-drive://", mount_email, "/", NULL);
      if ((volume = g_volume_monitor_get_volume_for_uuid (volume_monitor, volume_uuid)) == NULL)
        {
          g_error ("No GVolume found corresponding to the UUID: %s", volume_uuid);
          return;
        }

      volume_mount_sync (volume, G_MOUNT_MOUNT_NONE, NULL, &child_error);
      if (child_error != NULL)
        {
          g_propagate_error (error, g_steal_pointer (&child_error));
          return;
        }

      volume_activation_root = g_volume_get_activation_root (volume);
      self->mount = g_file_find_enclosing_mount (volume_activation_root, NULL, &child_error);
      if (child_error != NULL)
        {
          g_propagate_error (error, g_steal_pointer (&child_error));
          return;
        }
    }

  authorizer = gdata_goa_authorizer_new (test_account_goa_object);
  self->service = gdata_documents_service_new (GDATA_AUTHORIZER (authorizer));
  if (self->service == NULL)
    {
      g_error ("Couldn't initialize libgdata service for email: %s", mount_email);
      return;
    }

  self->domain = gdata_documents_service_get_primary_authorization_domain ();
}

static void
gvfs_google_test_init (GVfsGoogleTestData **_self, GError **error)
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
  GVfsGoogleTestData *self;

  *_self = g_new0 (GVfsGoogleTestData, 1);
  self = *_self;
  self->mount = NULL;

  setup_google_mount_and_libgdata_service (self, &child_error);
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

  g_test_message ("Test folder GFile ID: %s", test_dir_id);
  g_test_message ("Test folder Entry ID: %s", gdata_entry_get_id (GDATA_ENTRY (self->test_dir_entry)));

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

  g_test_message ("Test dummy GFile ID: %s", test_dummy_file_id);
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
gvfs_google_test_exit_cleanup (GVfsGoogleTestData **_self, GError **error)
{
  GVfsGoogleTestData *self = *_self;

  g_return_if_fail (GDATA_IS_DOCUMENTS_ENTRY (self->test_dir_entry));
  g_return_if_fail (GDATA_IS_DOCUMENTS_SERVICE (self->service));

  g_assert_true (delete_file_recursively (self->test_dir));

  g_object_unref (self->service);
  g_object_unref (self->test_dir_entry);
  g_object_unref (self->test_dir);
  g_object_unref (self->test_dummy_file);
  g_object_unref (self->mount);
  g_free (self);
}

/* ---------------------------------------------------------------------------------------------------- */
/* Actual test functions begin */
/* ---------------------------------------------------------------------------------------------------- */

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~ Make Directory Test Case Functions  ~~~~~~~~~~~~~~~~~~~~~~~~~~ */
static void
gvfs_google_test_make_directory_using_valid_display_name (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) dummy_new_folder = NULL;
  g_autofree gchar *parent_path = NULL;
  const gchar *folder_display_name = "Valid-Display-Name-Dir";

  parent_path = g_file_get_path (self->test_dir);
  dummy_new_folder = g_file_new_build_filename (parent_path, folder_display_name, NULL);

  g_assert_true (delete_and_make_new_directory (dummy_new_folder, &error));
  g_assert_no_error (error);
  g_assert_true (g_file_query_exists (dummy_new_folder, NULL));
}

static void
gvfs_google_test_make_directory_using_valid_id (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
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

  info1 = g_file_query_info (actual_new_folder1, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, G_FILE_QUERY_INFO_NONE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (info1);

  info2 = g_file_query_info (actual_new_folder2, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, G_FILE_QUERY_INFO_NONE, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (info1);

  g_assert_cmpstr (g_file_info_get_display_name (info1), ==, g_file_info_get_display_name (info2));
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~ Copy Test Case Functions  ~~~~~~~~~~~~~~~~~~~~~~~~~~ */

static void
gvfs_google_test_copy_file_from_one_parent_to_other_using_same_title (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *dest_path = NULL;
  g_autofree gchar *copy_test_dir_id = NULL;
  g_autofree gchar *dest_file_actual_display_name = NULL;
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

  dest_file_actual_display_name = get_file_attribute (dest_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (dest_file_actual_display_name, ==, TITLE_COPIED_FILE);
}

static void
gvfs_google_test_copy_file_from_one_parent_to_other_using_id (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *dest_path = NULL;
  g_autofree gchar *source_file_id = NULL;
  g_autofree gchar *source_file_title = NULL;
  g_autofree gchar *copy_test_dir_id = NULL;
  g_autofree gchar *dest_file_actual_display_name = NULL;
  g_autoptr(GFile) copy_test_dir = NULL;
  g_autoptr(GFile) dest_file = NULL;

  parent_path = g_file_get_path (self->test_dir);
  copy_test_dir = g_file_get_child_for_display_name (self->test_dir, OP_COPY_TEST_DIRECTORY, &error);
  g_assert_no_error (error);
  g_assert_nonnull (copy_test_dir);

  copy_test_dir_id = get_file_attribute (copy_test_dir, G_FILE_ATTRIBUTE_ID_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (copy_test_dir_id);

  source_file_id = get_file_attribute (self->test_dummy_file, G_FILE_ATTRIBUTE_ID_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (copy_test_dir_id);

  source_file_title = get_file_attribute (self->test_dummy_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file_title);

  dest_path = g_file_get_path (copy_test_dir);
  dest_file = g_file_new_build_filename (dest_path, source_file_id, NULL);

  op_done = g_file_copy (self->test_dummy_file, dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (op_done);

  dest_file_actual_display_name = get_file_attribute (dest_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_no_error (error);
  g_assert_nonnull (dest_file_actual_display_name);

  g_assert_cmpstr (dest_file_actual_display_name, ==, source_file_title);
}

static void
gvfs_google_test_copy_file_within_same_parent_with_title_change (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *dest_file_id = NULL;
  g_autofree gchar *dest_file_title = NULL;
  g_autofree gchar *dest_file_actual_display_name = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) dest_file = NULL;

  source_file = g_file_get_child_for_display_name (self->test_dir, TITLE_DUMMY_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  dest_file_title = g_strconcat (TITLE_DUMMY_FILE, " (Copy)", NULL);
  parent_path = g_file_get_path (self->test_dir);
  dest_file = g_file_new_build_filename (parent_path, dest_file_title, NULL);

  op_done = g_file_copy (source_file, dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (op_done);

  g_clear_object (&dest_file);
  dest_file = g_file_get_child_for_display_name (self->test_dir, dest_file_title, &error);
  g_assert_no_error (error);
  g_assert_nonnull (dest_file);

  dest_file_id = get_file_attribute (dest_file, G_FILE_ATTRIBUTE_ID_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (dest_file_id);

  dest_file_actual_display_name = get_file_attribute (dest_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (dest_file_actual_display_name, ==, dest_file_title);
}

static void
gvfs_google_test_copy_file_within_same_parent_with_same_title (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
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
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
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
gvfs_google_test_copy_file_within_same_parent_with_source_id_as_destination_basename (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
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
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
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
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
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

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~ Move Test Case Functions  ~~~~~~~~~~~~~~~~~~~~~~~~~~ */

static void
gvfs_google_test_move_file_within_same_parent_without_title_change (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *dest_file_title = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) dest_file = NULL;

  /* We need some file which we can move. So, we use test_dummy_file to create
   * a copy of that file and move its copy around using its ID. */
  source_file = create_temporary_duplicate_file (self->test_dummy_file, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  dest_file_title = get_file_attribute (source_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  parent_path = g_file_get_path (self->test_dir);
  dest_file = g_file_new_build_filename (parent_path, dest_file_title, NULL);

  op_done = g_file_move (source_file, dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
  g_assert_false (op_done);
}

static void
gvfs_google_test_move_file_within_same_parent_with_title_change (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *dest_file_title = NULL;
  g_autofree gchar *dest_file_actual_display_name = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) dest_file = NULL;

  /* We need some file which we can move. So, we use test_dummy_file to create
   * a copy of that file and move its copy around using its ID. */
  source_file = create_temporary_duplicate_file (self->test_dummy_file, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  dest_file_title = g_strconcat (TITLE_DUMMY_FILE, "_test_move_file_within_same_parent_with_title_change", NULL);
  parent_path = g_file_get_path (self->test_dir);
  dest_file = g_file_new_build_filename (parent_path, dest_file_title, NULL);

  op_done = g_file_move (source_file, dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (op_done);

  g_clear_object (&dest_file);
  dest_file = g_file_get_child_for_display_name (self->test_dir, dest_file_title, &error);
  g_assert_no_error (error);
  g_assert_nonnull (dest_file);

  dest_file_actual_display_name = get_file_attribute (dest_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (dest_file_actual_display_name, ==, dest_file_title);
}

static void
gvfs_google_test_move_file_from_one_parent_to_other_without_backup (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *dest_path = NULL;
  g_autofree gchar *source_file_title = NULL;
  g_autofree gchar *dest_file_actual_display_name = NULL;
  g_autoptr(GFile) move_test_dir = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) dest_file = NULL;

  move_test_dir = g_file_get_child_for_display_name (self->test_dir, OP_MOVE_TEST_DIRECTORY, &error);
  g_assert_no_error (error);
  g_assert_nonnull (move_test_dir);

  dest_path = g_file_get_path (move_test_dir);
  dest_file = g_file_new_build_filename (dest_path, TITLE_MOVED_FILE, NULL);

  /* We need some file which we can move. So, we use test_dummy_file to create
   * a copy of that file and move its copy around using its ID. */
  source_file = create_temporary_duplicate_file (self->test_dummy_file, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  source_file_title = get_file_attribute (source_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file_title);

  dest_path = g_file_get_path (move_test_dir);
  dest_file = g_file_new_build_filename (dest_path, source_file_title, NULL);

  op_done = g_file_move (source_file, dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (op_done);

  dest_file_actual_display_name = get_file_attribute (dest_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_cmpstr (dest_file_actual_display_name, ==, source_file_title);
}

static void
gvfs_google_test_move_file_from_one_parent_to_other_with_backup (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *dest_path = NULL;
  g_autofree gchar *source_file_title = NULL;
  g_autofree gchar *dest_file_actual_display_name = NULL;
  g_autoptr(GFile) move_test_dir = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) dest_file = NULL;

  parent_path = g_file_get_path (self->test_dir);
  move_test_dir = g_file_get_child_for_display_name (self->test_dir, OP_MOVE_TEST_DIRECTORY, &error);
  g_assert_no_error (error);
  g_assert_nonnull (move_test_dir);

  /* We need some file which we can move. So, we use test_dummy_file to create
   * a copy of that file and move its copy around using its ID. */
  source_file = create_temporary_duplicate_file (self->test_dummy_file, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  source_file_title = get_file_attribute (source_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file_title);

  dest_path = g_file_get_path (move_test_dir);
  dest_file = g_file_new_build_filename (dest_path, source_file_title, NULL);

  op_done = g_file_move (source_file, dest_file, G_FILE_COPY_BACKUP, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (op_done);

  dest_file_actual_display_name = get_file_attribute (dest_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_cmpstr (dest_file_actual_display_name, ==, source_file_title);
}

static void
gvfs_google_test_move_file_from_one_parent_to_other_using_same_title (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *dest_path = NULL;
  g_autofree gchar *source_file_title = NULL;
  g_autofree gchar *dest_file_actual_display_name = NULL;
  g_autoptr(GFile) move_test_dir = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) dest_file = NULL;

  parent_path = g_file_get_path (self->test_dir);
  move_test_dir = g_file_get_child_for_display_name (self->test_dir, OP_MOVE_TEST_DIRECTORY, &error);
  g_assert_no_error (error);
  g_assert_nonnull (move_test_dir);

  /* We need some file which we can move. So, we use test_dummy_file to create
   * a copy of that file and move its copy around using its ID. */
  source_file = create_temporary_duplicate_file (self->test_dummy_file, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  source_file_title = get_file_attribute (source_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file_title);

  dest_path = g_file_get_path (move_test_dir);
  dest_file = g_file_new_build_filename (dest_path, source_file_title, NULL);

  op_done = g_file_move (source_file, dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (op_done);

  dest_file_actual_display_name = get_file_attribute (dest_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_cmpstr (dest_file_actual_display_name, ==, source_file_title);
}

static void
gvfs_google_test_move_file_from_one_parent_to_other_using_new_title (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *dest_path = NULL;
  g_autofree gchar *source_file_title = NULL;
  g_autofree gchar *new_file_title = NULL;
  g_autofree gchar *dest_file_actual_display_name = NULL;
  g_autoptr(GFile) move_test_dir = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) dest_file = NULL;

  parent_path = g_file_get_path (self->test_dir);
  move_test_dir = g_file_get_child_for_display_name (self->test_dir, OP_MOVE_TEST_DIRECTORY, &error);
  g_assert_no_error (error);
  g_assert_nonnull (move_test_dir);

  /* We need some file which we can move. So, we use test_dummy_file to create
   * a copy of that file and move its copy around using its ID. */
  source_file = create_temporary_duplicate_file (self->test_dummy_file, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  source_file_title = get_file_attribute (source_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file_title);

  new_file_title = g_strconcat (source_file_title, " (NewTitle)", NULL);
  dest_path = g_file_get_path (move_test_dir);
  dest_file = g_file_new_build_filename (dest_path, new_file_title, NULL);

  op_done = g_file_move (source_file, dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (op_done);

  dest_file_actual_display_name = get_file_attribute (dest_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_cmpstr (dest_file_actual_display_name, ==, new_file_title);
}

static void
gvfs_google_test_move_file_from_one_parent_to_other_using_id (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *test_dir_path = NULL;
  g_autofree gchar *dest_path = NULL;
  g_autofree gchar *source_file_id = NULL;
  g_autofree gchar *source_file_title = NULL;
  g_autofree gchar *dest_file_actual_display_name = NULL;
  g_autoptr(GFile) move_test_dir = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) dest_file = NULL;

  /* We need some file which we can move. So, we use test_dummy_file to create
   * a copy of that file and move its copy around using its ID.   */
  source_file = create_temporary_duplicate_file (self->test_dummy_file, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  test_dir_path = g_file_get_path (self->test_dir);
  move_test_dir = g_file_get_child_for_display_name (self->test_dir, OP_MOVE_TEST_DIRECTORY, &error);
  g_assert_no_error (error);
  g_assert_nonnull (move_test_dir);

  source_file_title = get_file_attribute (source_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file_title);

  source_file_id = get_file_attribute (source_file, G_FILE_ATTRIBUTE_ID_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file_id);

  dest_path = g_file_get_path (move_test_dir);
  dest_file = g_file_new_build_filename (dest_path, source_file_id, NULL);

  op_done = g_file_move (source_file, dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (op_done);

  dest_file_actual_display_name = get_file_attribute (dest_file, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, &error);
  g_assert_no_error (error);
  g_assert_nonnull (dest_file_actual_display_name);

  g_assert_cmpstr (dest_file_actual_display_name, ==, source_file_title);
}

#ifdef HAVE_GDATA_DOCUMENTS_QUERY_SET_ORDER_BY
static void
gvfs_google_test_move_file_from_one_parent_to_other_with_both_title_and_volatile_entry_collision (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *test_dir_path = NULL;
  g_autofree gchar *parent_path = NULL;
  g_autofree gchar *source_file_id = NULL;
  g_autoptr(GFile) move_test_dir = NULL;
  g_autoptr(GFile) source_file = NULL;
  g_autoptr(GFile) dest_file = NULL;

  /* We need some file which we can firstly copy. So, we use test_dummy_file to create
   * a copy of that file and move its copy around using its ID.  */
  source_file = create_temporary_duplicate_file (self->test_dummy_file, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file);

  source_file_id = get_file_attribute (source_file, G_FILE_ATTRIBUTE_ID_FILE, &error);
  g_assert_no_error (error);
  g_assert_nonnull (source_file_id);

  test_dir_path = g_file_get_path (self->test_dir);
  move_test_dir = g_file_get_child_for_display_name (self->test_dir, OP_MOVE_TEST_DIRECTORY, &error);
  g_assert_no_error (error);
  g_assert_nonnull (move_test_dir);

  parent_path = g_file_get_path (move_test_dir);
  dest_file = g_file_new_build_filename (parent_path, source_file_id, NULL);

  /* The copy operation, i.e. `gio copy id1 ./id2/` (which is equivalent to
   * `gio copy id1 ./id2/id1`)  */
  op_done = g_file_copy (source_file, dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (op_done);

  /* The move operation, i.e. `gio move id1 ./id2/` (which is equivalent to
   * `gio move id1 ./id2/id1`) */
  op_done = g_file_move (source_file, dest_file, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_assert_no_error (error);
  g_assert_true (op_done);

  /* TODO: Assert here that both the files are present */
}
#endif

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~ Delete Test Case Functions  ~~~~~~~~~~~~~~~~~~~~~~~~~~ */

static void
gvfs_google_test_recursive_delete_test_dir_folder (gconstpointer user_data)
{
  GVfsGoogleTestData *self = (GVfsGoogleTestData *) user_data;
  gboolean op_done;
  g_autoptr(GError) error = NULL;

  op_done = delete_and_make_new_directory (self->test_dir, &error);
  g_assert_no_error (error);
  g_assert_true (op_done);
}

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~ Main Function  ~~~~~~~~~~~~~~~~~~~~~~~~~~ */

gint
main (gint argc, gchar *argv [])
{
  gint retval = EXIT_SUCCESS;
  GVfsGoogleTestData *self;
  g_autoptr(GError) error = NULL;

  setlocale (LC_ALL, "");

  g_test_init (&argc, &argv, NULL);

  gvfs_google_test_init (&self, &error);
  if (error != NULL)
    {
      g_warning ("Error (init): %s", error->message);
      return 1;
    }

  /* ~~~~~~~~~~~~~~~~~~~~~~~~~~ Make Dir test cases  ~~~~~~~~~~~~~~~~~~~~~~~~~~ */

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
   * self->test_dir_entry's title and *NOT* its ID. The reason is well documented
   * in the make_directory function in gvfsbackendgoogle.c */
  g_test_add_data_func ("/make_directory/using_valid_id_of_other_directory",
                        (gconstpointer) self,
                        gvfs_google_test_make_directory_using_valid_id);

  /* ~~~~~~~~~~~~~~~~~~~~~~~~~~ Copy test cases  ~~~~~~~~~~~~~~~~~~~~~~~~~~ */

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
   * operation corresponds to `gio copy -i ./file1_title ./file1_title` but without overwrite.
   *
   * This operation should supposedly fail with error code `G_IO_ERROR_EXISTS`
   * and string "Target file already exists". */
  g_test_add_data_func ("/copy_file/within_same_parent_with_same_title",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_within_same_parent_with_same_title);

  /* We try to copy a file within the same parent and keeping the title of file
   * to be same as the ID of source file, i.e. this operation corresponds to
   * `gio copy -i ./id1 ./id1` but without overwrite.
   *
   * This operation should supposedly fail with error code `G_IO_ERROR_EXISTS`
   * and string "Target file already exists". */
  g_test_add_data_func ("/copy_file/within_same_parent_with_source_id_as_destination_basename",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_within_same_parent_with_source_id_as_destination_basename);

  /* The below test case is equivalent to the following commandline operations:
   *
   * `gio copy ./id1 ./$Title$` where $Title$ is the title of file with ID `id1`.
   *
   * This operation should supposedly fail when done over the commandline gio
   * tool with error code `G_IO_ERROR_FAILED` and string "Operation Not Supported".
   * TODO: Fix the below test-case to use actual overwrite. */
  g_test_add_data_func ("/copy_file/within_same_parent_try_overwrite_with_same_title",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_within_same_parent_try_overwrite_with_same_title);

  /* The below test case is equivalent to the following commandline operations:
   *
   * `gio copy id1 ./id1`
   *
   * This operation should supposedly fail when done over the commandline gio
   * tool with error code `G_IO_ERROR_FAILED` and string "Operation Not Supported".
   * TODO: Fix the below test-case to use actual overwrite. */
  g_test_add_data_func ("/copy_file/within_same_parent_try_overwrite_with_id",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_within_same_parent_try_overwrite_with_id);

  /* The below test case is equivalent to the following commandline operations:
   * `gio copy id1 id2/$Title$`  (where id2 is the ID of a folder, and $Title$
   * is the title of file with ID `id1`) */
  g_test_add_data_func ("/copy_file/from_one_parent_to_other_using_same_title",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_from_one_parent_to_other_using_same_title);

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
   * identifier "/copy_file/from_one_parent_to_other" because copying
   * file from source to destination is same as producing both kinds of
   * collisions at once.
   *
   * It basically mimics the following case:
   * `gio copy id1 id2/some_title`
   * `gio copy id1 id2/some_title`
   *
   * TODO: Re-enable the below test-case once you have orderBy="modifiedDate" */
#ifdef HAVE_GDATA_DOCUMENTS_QUERY_SET_ORDER_BY
  g_test_add_data_func ("/copy_file/from_one_parent_to_other_with_both_title_and_volatile_entry_collision",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_from_one_parent_to_other_using_same_title);
#endif

  /* The below test case is equivalent to the following commandline operations:
   * `gio copy id1 id2/id1`  (where id2 is the ID of a folder) */
  g_test_add_data_func ("/copy_file/from_one_parent_to_other_using_id",
                        (gconstpointer) self,
                        gvfs_google_test_copy_file_from_one_parent_to_other_using_id);

  /* ~~~~~~~~~~~~~~~~~~~~~~~~~~ Move test cases  ~~~~~~~~~~~~~~~~~~~~~~~~~~ */

  /* The below test case is equivalent to the following commandline operations:
   * (Assume that file with ID `id1` has the title "title1")
   *
   * `gio move ./title1 ./title1`
   *
   * This operation will fail with an error */
  g_test_add_data_func ("/move_file/within_same_parent_without_title_change",
                        (gconstpointer) self,
                        gvfs_google_test_move_file_within_same_parent_without_title_change);

  /* The below test case is equivalent to the following commandline operations:
   * (Assume that file with ID `id1` has the title ${TITLE})
   *
   * `gio move ./${TITLE} ./SomeOtherTitle`
   *
   * This is just a normal rename operation in POSIX world, and the backend allows this. */
  g_test_add_data_func ("/move_file/within_same_parent_with_title_change",
                        (gconstpointer) self,
                        gvfs_google_test_move_file_within_same_parent_with_title_change);

  /* The below test case is equivalent to the following commandline operations:
   * `gio move ./id1 ./id2/$Title$` (where `id2` is the ID of a folder, and
   * $Title$ is the title of file with ID `id1`).
   *
   * This is simplest case for cross directory file moving. */
  g_test_add_data_func ("/move_file/from_one_parent_to_other_without_backup",
                        (gconstpointer) self,
                        gvfs_google_test_move_file_from_one_parent_to_other_without_backup);

  /* The below test case is equivalent to the following commandline operations
   * (with the G_FILE_COPY_BACKUP flag supplied to g_file_move function):
   * `gio move ./id1 id2/$Title$`  (where `id2` is the ID of a folder, and
   * $Title$ is the title of file with ID `id1`)
   *
   * Although the g_vfs_backend_google_move () will fail with G_IO_ERROR_NOT_SUPPORTED,
   * gio will use the fallback (copy + create + read + write + delete) to
   * support a backup operation. */
  g_test_add_data_func ("/move_file/from_one_parent_to_other_with_backup",
                        (gconstpointer) self,
                        gvfs_google_test_move_file_from_one_parent_to_other_with_backup);

  /* The below test case is equivalent to the following commandline operations:
   * `gio move ./id1 id2/$Title$`  (where `id2` is the ID of a folder, and
   * $Title$ is the title of file with ID `id1`) */
  g_test_add_data_func ("/move_file/from_one_parent_to_other_using_same_title",
                        (gconstpointer) self,
                        gvfs_google_test_move_file_from_one_parent_to_other_using_same_title);

  /* The below test case is equivalent to the following commandline operations:
   * `gio move ./id1 id2/SomeNewTitle`  (where `id2` is the ID of a folder, and
   * $Title$ is the title of file with ID `id1`) */
  g_test_add_data_func ("/move_file/from_one_parent_to_other_using_new_title",
                        (gconstpointer) self,
                        gvfs_google_test_move_file_from_one_parent_to_other_using_new_title);

  /* The below test case is equivalent to the following commandline operations:
   * `gio move ./id1 id2/id1`  (where `id2` is the ID of a folder) */
  g_test_add_data_func ("/move_file/from_one_parent_to_other_using_id",
                        (gconstpointer) self,
                        gvfs_google_test_move_file_from_one_parent_to_other_using_id);

  /* The below test case is equivalent to the following commandline operations:
   *
   * 1. `gio copy ./id1 ./id2/`                   ("id2" is the real ID of a folder)
   * 2. `gio move ./id1 ./id2/`
   *
   * In this, the copy operation will create a volatile entry i.e. (id1, id2) -> Entry tuple.
   * Later, during the move operation, this volatile entry will collide with
   * the real ID "id1". We support this operation and the below test should pass.
   * TODO: Re-enable the below test-case once you have orderBy="modifiedDate" */
#ifdef HAVE_GDATA_DOCUMENTS_QUERY_SET_ORDER_BY
  g_test_add_data_func ("/move_file/from_one_parent_to_other_with_both_title_and_volatile_entry_collision",
                        (gconstpointer) self,
                        gvfs_google_test_move_file_from_one_parent_to_other_with_both_title_and_volatile_entry_collision);
#endif

  /* ~~~~~~~~~~~~~~~~~~~~~~~~~~ Delete test cases  ~~~~~~~~~~~~~~~~~~~~~~~~~~ */

  /* We simply delete everything in the below case. If the backend crashes at
   * this point, there is some issue with cache. Moreover, delete is the case
   * which can be used to ensure the sanity of the cache. If delete wrongly
   * removes an entry (or wrongly decreases the ref_count), there's a very high
   * chance of getting a segfault sometime later.
   *
   * Also, beyond the execution of below test case, the GFile corresponding to
   * self->test_dummy_file won't be valid since that file will be deleted. As a result,
   * any operation done using test_dummy_file will result into an error. The memory
   * corresponding to the test_dummy_file GFile will be freed in the cleanup
   * function. */
  g_test_add_data_func ("/recursive_delete/test_dir_folder",
                        (gconstpointer) self,
                        gvfs_google_test_recursive_delete_test_dir_folder);

  retval = g_test_run ();

  gvfs_google_test_exit_cleanup (&self, &error);
  if (error != NULL)
    {
      g_warning ("Error (cleanup): %s", error->message);
      retval = EXIT_FAILURE;
    }

  return retval;
}
