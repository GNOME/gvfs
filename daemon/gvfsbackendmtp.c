/* GIO - GLib Input, Output and Streaming Library
 *   MTP Backend
 * 
 * Copyright (C) 2012 Philip Langdale <philipl@overt.org>
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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include <libmtp.h>

#include "gvfsbackendmtp.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobdelete.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsmonitor.h"


/************************************************
 * 	  Initialization
 * 
 */

G_DEFINE_TYPE (GVfsBackendMtp, g_vfs_backend_mtp, G_VFS_TYPE_BACKEND)

static void
g_vfs_backend_mtp_init (GVfsBackendMtp *backend)
{
    g_print ("(II) g_vfs_backend_mtp_init \n");

    g_mutex_init(&backend->mutex);
    backend->devices = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, LIBMTP_Release_Device);

    g_print ("(II) g_vfs_backend_mtp_init done.\n");
}

static void
g_vfs_backend_mtp_finalize (GObject *object)
{
	GVfsBackendMtp *backend;

	g_print ("(II) g_vfs_backend_mtp_finalize \n");

	backend = G_VFS_BACKEND_MTP (object);

    g_hash_table_destroy(backend->devices);
    g_mutex_clear(&backend->mutex);

    if (backend->test)
    	g_free ((gpointer)backend->test);  
  
	if (G_OBJECT_CLASS (g_vfs_backend_mtp_parent_class)->finalize)
      (*G_OBJECT_CLASS (g_vfs_backend_mtp_parent_class)->finalize) (object);
}







/************************************************
 * 	  Mount
 * 
 */

static void
do_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendMtp *op_backend = G_VFS_BACKEND_MTP (backend);

  g_print ("(II) try_mount \n");

  g_vfs_backend_set_display_name (backend, "mtp");

  op_backend->mount_spec = g_mount_spec_new ("mtp");
  g_vfs_backend_set_mount_spec (backend, op_backend->mount_spec);

  g_vfs_backend_set_icon_name (backend, "multimedia-player");

  LIBMTP_Init();

  g_vfs_job_succeeded (G_VFS_JOB (job));
}


static void
do_unmount (GVfsBackend *backend, GVfsJobUnmount *job,
            GMountUnmountFlags flags,
            GMountSource *mount_source)
{
  GVfsBackendMtp *op_backend;

  g_print ("(II) try_umount \n");

  op_backend = G_VFS_BACKEND_MTP (backend);
  g_mount_spec_unref (op_backend->mount_spec);
  g_vfs_job_succeeded (G_VFS_JOB(job));
}







/************************************************
 * 	  Queries
 * 
 */

static void
get_file_info(GFileInfo *info, LIBMTP_file_t *file) {
        g_file_info_set_file_type(info, file->filetype == LIBMTP_FILETYPE_FOLDER ? G_FILE_TYPE_DIRECTORY : G_FILE_TYPE_REGULAR);
        char *id = g_strdup_printf("%u", file->item_id);
        g_file_info_set_name(info, id);
        g_free(id);
        g_file_info_set_display_name(info, file->filename);
        g_file_info_set_content_type (info, file->filetype == LIBMTP_FILETYPE_FOLDER ? "inode/directory" : "application/octet-stream");
        g_file_info_set_size (info, file->filesize);
        if (file->filetype == LIBMTP_FILETYPE_FOLDER) {
          GIcon *icon = g_themed_icon_new ("folder");
          g_file_info_set_icon (info, icon);
          g_object_unref (icon);
        }
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, TRUE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, TRUE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, TRUE);
        g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_COPY_NAME, file->filename);
}


static void
do_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  GFile *file;
  GFileInfo *info;
  GError *error;
  GFileEnumerator *enumerator;
  gboolean res;
  int i;

  gchar **elements = g_strsplit_set(filename, "/", -1);
  unsigned int ne = 0;
  for (ne = 0; elements[ne] != NULL; ne++);

  g_print ("(II) try_enumerate (filename = %s, n_elements = %d) \n", filename, ne);

  g_mutex_lock (&G_VFS_BACKEND_MTP(backend)->mutex);
  //if (strcmp("/", filename) == 0) {
  if (ne == 2 && elements[1][0] == '\0') {
    LIBMTP_raw_device_t * rawdevices;
    int numrawdevices;
    LIBMTP_error_number_t err;

    err = LIBMTP_Detect_Raw_Devices(&rawdevices, &numrawdevices);
    switch(err) {
    case LIBMTP_ERROR_NO_DEVICE_ATTACHED:
      fprintf(stdout, "   No raw devices found.\n");
      break;
    case LIBMTP_ERROR_CONNECTING:
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Detect: There has been an error connecting.");
      return;
    case LIBMTP_ERROR_MEMORY_ALLOCATION:
      g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
		      "Detect: Encountered a Memory Allocation Error.");
      return;
    case LIBMTP_ERROR_NONE:
      break;
    case LIBMTP_ERROR_GENERAL:
    default:
      g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
		      "Detect: Unknown Error.");
      return;
    }

    /* Iterate over connected MTP devices */
    fprintf(stdout, "Attempting to connect device(s)\n");
    for (i = 0; i < numrawdevices; i++) {
      LIBMTP_mtpdevice_t *device;
      LIBMTP_devicestorage_t *storage;
      char *friendlyname;
      int ret;
      char *name;
      name = g_strdup_printf("device%d", i);

      device = g_hash_table_lookup(G_VFS_BACKEND_MTP(backend)->devices, name);
      if (device == NULL) {
        device = LIBMTP_Open_Raw_Device_Uncached(&rawdevices[i]);
        if (device == NULL) {
          fprintf(stderr, "Unable to open raw device %d\n", i);
          continue;
        }

        g_hash_table_insert(G_VFS_BACKEND_MTP(backend)->devices,
                            g_strdup(name), device);

        LIBMTP_Dump_Errorstack(device);
        LIBMTP_Clear_Errorstack(device);
      }

      friendlyname = LIBMTP_Get_Friendlyname(device);
      if (friendlyname == NULL) {
        printf("Device: (NULL)\n");
      } else {
        printf("Device: %s\n", friendlyname);
        free(friendlyname);
      }

      info = g_file_info_new();
      g_file_info_set_file_type(info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_attribute_boolean(info, G_FILE_ATTRIBUTE_STANDARD_IS_VIRTUAL, TRUE);
      g_file_info_set_name(info, name);
      g_file_info_set_display_name(info, "MTP Device");
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_content_type (info, "inode/directory");
      g_file_info_set_size (info, 0);
      GIcon *icon = g_themed_icon_new ("multimedia-player");
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE); 
      g_vfs_job_enumerate_add_info (job, info);
      g_free(name);
    }
  //} else if (strcmp("/device0", filename) == 0) {
  } else if (ne == 2) {
    LIBMTP_mtpdevice_t *device;
    LIBMTP_devicestorage_t *storage;
    device = g_hash_table_lookup(G_VFS_BACKEND_MTP(backend)->devices, elements[1]);
    int ret = LIBMTP_Get_Storage(device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    if (ret != 0) {
      fprintf(stderr, "Unable to find storage for device %d\n", i);
    } else {
      for (storage = device->storage; storage != 0; storage = storage->next) {
        fprintf(stdout, "Storage: %s\n", storage->StorageDescription);
        info = g_file_info_new();
        g_file_info_set_file_type(info, G_FILE_TYPE_DIRECTORY);
        g_file_info_set_attribute_boolean(info, G_FILE_ATTRIBUTE_STANDARD_IS_VIRTUAL, TRUE);
        char *id = g_strdup_printf("%u", storage->id);
        g_file_info_set_name(info, id);
        g_free(id);
        g_file_info_set_display_name(info, storage->StorageDescription);
        g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
        g_file_info_set_content_type (info, "inode/directory");
        g_file_info_set_size (info, 0);
        GIcon *icon = g_themed_icon_new_with_default_fallbacks ("drive-harddisk-removable");
        g_file_info_set_icon (info, icon);
        g_object_unref (icon);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, TRUE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE); 
        g_vfs_job_enumerate_add_info (job, info);
      }
    }
  //} else if (strcmp("/device0/65537", filename) == 0) {
  } else if (ne > 2) {
    LIBMTP_file_t *files;
    LIBMTP_file_t *file;

    LIBMTP_mtpdevice_t *device;
    device = g_hash_table_lookup(G_VFS_BACKEND_MTP(backend)->devices, elements[1]);

    int pid = (ne == 3 ? -1 : strtol(elements[ne-1], NULL, 10));

    files = LIBMTP_Get_Files_And_Folders(device, strtol(elements[2], NULL, 10), pid);
    for (file = files; file != NULL; file = file->next) {
        info = g_file_info_new();
        get_file_info(info, file);
        g_vfs_job_enumerate_add_info (job, info);
    }
  }

  g_strfreev(elements);

  g_vfs_job_enumerate_done (job);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_mutex_unlock (&G_VFS_BACKEND_MTP(backend)->mutex);
  g_print ("(II) try_enumerate done. \n");
}

static void
do_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  g_print ("(II) try_query_info (filename = %s) \n", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP(backend)->mutex);

  gchar **elements = g_strsplit_set(filename, "/", -1);
  unsigned int ne = 0;
  for (ne = 0; elements[ne] != NULL; ne++);

  if (ne == 2 && elements[1][0] == '\0') {
    g_file_info_set_file_type(info, G_FILE_TYPE_DIRECTORY);
    g_file_info_set_content_type (info, "inode/directory");
  } else if (ne < 4) {
    g_file_info_set_file_type(info, G_FILE_TYPE_DIRECTORY);
    g_file_info_set_content_type (info, "inode/directory");
  } else {
    LIBMTP_mtpdevice_t *device;
    device = g_hash_table_lookup(G_VFS_BACKEND_MTP(backend)->devices, elements[1]);
    LIBMTP_file_t *file = LIBMTP_Get_Filemetadata(device, strtol(elements[ne-1], NULL, 10));
    get_file_info(info, file);
  }
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_mutex_unlock (&G_VFS_BACKEND_MTP(backend)->mutex);
}


static void
do_query_fs_info (GVfsBackend *backend,
		  GVfsJobQueryFsInfo *job,
		  const char *filename,
		  GFileInfo *info,
		  GFileAttributeMatcher *attribute_matcher)
{
  g_print ("(II) try_query_fs_info (filename = %s) \n", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP(backend)->mutex);

  gchar **elements = g_strsplit_set(filename, "/", -1);
  unsigned int ne = 0;
  for (ne = 0; elements[ne] != NULL; ne++);

  if (ne > 2) {
    LIBMTP_mtpdevice_t *device;
    LIBMTP_devicestorage_t *storage;
    device = g_hash_table_lookup(G_VFS_BACKEND_MTP(backend)->devices, elements[1]);
    int ret = LIBMTP_Get_Storage(device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    if (ret != 0) {
      fprintf(stderr, "Unable to find storage for device %s\n", elements[1]);
    } else {
      for (storage = device->storage; storage != 0; storage = storage->next) {
        if (storage->id == strtol(elements[2], NULL, 10)) {
          g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, storage->FreeSpaceInBytes);
          g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, storage->AccessCapability);
          g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "mtpfs");
        }
      }
    }
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_mutex_unlock (&G_VFS_BACKEND_MTP(backend)->mutex);
}


/************************************************
 * 	  Operations
 * 
 */

typedef struct {
  GFileProgressCallback progress_callback;
  gpointer progress_callback_data;
  GVfsJob *job;
} MtpProgressData;


static int mtp_progress (uint64_t const sent, uint64_t const total,
                  MtpProgressData const * const data)
{
  g_print ("mtp_progress: %lu/%lu, cancelled: %d\n", sent, total, g_vfs_job_is_cancelled(data->job));
  data->progress_callback(sent, total, data->progress_callback_data);
  return g_vfs_job_is_cancelled(data->job);
}

static void
do_pull(GVfsBackend *backend,
                                GVfsJobPull *job,
                                const char *source,
                                const char *local_path,
                                GFileCopyFlags flags,
                                gboolean remove_source,
                                GFileProgressCallback progress_callback,
                                gpointer progress_callback_data)
{
  g_print ("(II) do_pull (filename = %s, local_path = %s) \n", source, local_path);
  g_mutex_lock (&G_VFS_BACKEND_MTP(backend)->mutex);

  gchar **elements = g_strsplit_set(source, "/", -1);
  unsigned int ne = 0;
  for (ne = 0; elements[ne] != NULL; ne++);

  if (ne < 4) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Can't download entity.");
  } else {
    LIBMTP_mtpdevice_t *device;
    device = g_hash_table_lookup(G_VFS_BACKEND_MTP(backend)->devices, elements[1]);
    LIBMTP_file_t *file = LIBMTP_Get_Filemetadata(device, strtol(elements[ne-1], NULL, 10));

    GFileInfo *info = g_file_info_new();
    get_file_info(info, file);
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
      GError *error;
      GFile *file = g_file_new_for_path (local_path);
      g_assert (file != NULL);
      if (file) {
        error = NULL;
	    if (g_file_make_directory (file, G_VFS_JOB (job)->cancellable, &error)) {
          g_vfs_job_succeeded (G_VFS_JOB (job));
        } else {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
          g_error_free (error);
        }
        g_object_unref (file);
      }
    } else {
      MtpProgressData *mtp_progress_data = g_new0(MtpProgressData, 1);
      mtp_progress_data->progress_callback = progress_callback;
      mtp_progress_data->progress_callback_data = progress_callback_data;
      mtp_progress_data->job = G_VFS_JOB(job);
      int ret = LIBMTP_Get_File_To_File(device, strtol(elements[ne-1], NULL, 10), local_path,
                              mtp_progress, mtp_progress_data);
      g_free(mtp_progress_data);
      if (ret != 0) {
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Error while downloading entity.");
      } else {
        g_vfs_job_succeeded (G_VFS_JOB (job));
      }
    }
  }

  g_mutex_unlock (&G_VFS_BACKEND_MTP(backend)->mutex);
}


static void
do_make_directory (GVfsBackend *backend,
                    GVfsJobMakeDirectory *job,
                    const char *filename)
{
  GError *error;
  GFile *file;

  g_print ("(II) try_make_directory (filename = %s) \n", filename);
}


static void
do_delete (GVfsBackend *backend,
            GVfsJobDelete *job,
            const char *filename)
{
  GError *error;
  GFile *file;

  g_print ("(II) try_delete (filename = %s) \n", filename);
}


//  aka 'rename'
static void
do_set_display_name (GVfsBackend *backend,
                      GVfsJobSetDisplayName *job,
                      const char *filename,
                      const char *display_name)
{
  GError *error;
  GFile *file;

  g_print ("(II) try_set_display_name '%s' --> '%s' \n", filename, display_name);

}


/************************************************
 * 	  Class init
 * 
 */


static void
g_vfs_backend_mtp_class_init (GVfsBackendMtpClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_mtp_finalize;

  backend_class->mount = do_mount;
  backend_class->unmount = do_unmount;
//  backend_class->open_for_read = do_open_for_read;
//  backend_class->read = do_read;
//  backend_class->seek_on_read = do_seek_on_read;
//  backend_class->close_read = do_close_read;
//  backend_class->close_write = do_close_write;
  backend_class->query_info = do_query_info;
  backend_class->enumerate = do_enumerate;
  backend_class->query_fs_info = do_query_fs_info;
  backend_class->pull = do_pull;
//  backend_class->create = do_create;
//  backend_class->append_to = do_append_to;
//  backend_class->replace = do_replace;
//  backend_class->write = do_write;
//  backend_class->seek_on_write = do_seek_on_write;
/*  -- disabled, read/write operations can handle copy correctly  */  
/*   backend_class->copy = do_copy; */ 
//  backend_class->move = do_move;
//  backend_class->make_symlink = do_make_symlink;
//  backend_class->make_directory = do_make_directory;
//  backend_class->delete = do_delete;
//  backend_class->trash = do_trash;
//  backend_class->set_display_name = do_set_display_name;
//  backend_class->set_attribute = do_set_attribute;
//  backend_class->create_dir_monitor = do_create_dir_monitor;
//  backend_class->create_file_monitor = do_create_file_monitor;
//  backend_class->query_settable_attributes = do_query_settable_attributes;
//  backend_class->query_writable_namespaces = do_query_writable_namespaces;
}
