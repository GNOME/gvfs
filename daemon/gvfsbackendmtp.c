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
                                             g_free,
                                             (GDestroyNotify)LIBMTP_Release_Device);

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

  if (G_OBJECT_CLASS (g_vfs_backend_mtp_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_mtp_parent_class)->finalize) (object);
}







/************************************************
 * 	  Mount
 * 
 */

static void
on_uevent (GUdevClient *client, gchar *action, GUdevDevice *device, gpointer user_data)
{
  g_print ("on_uevent action %s, device %s\n", action, g_udev_device_get_device_file (device));
}

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

  const char *subsystems[] = {"usb", NULL};
  op_backend->gudev_client = g_udev_client_new (subsystems);
  if (op_backend->gudev_client == NULL) {
    GError *error;
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Cannot create gudev client"));
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    g_error_free (error);
    return;
  }
  g_signal_connect (op_backend->gudev_client, "uevent", G_CALLBACK (on_uevent), op_backend);

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
  g_object_unref(op_backend->gudev_client);
  g_mount_spec_unref (op_backend->mount_spec);
  g_vfs_job_succeeded (G_VFS_JOB(job));
}







/************************************************
 * 	  Queries
 * 
 */

static LIBMTP_mtpdevice_t *
get_device(GVfsBackend *backend, const char *id, GVfsJob *job) {

  LIBMTP_mtpdevice_t *device;
  device = g_hash_table_lookup(G_VFS_BACKEND_MTP(backend)->devices, id);
  if (device) {
    g_print("Returning device %p\n", device);
    return device;
  }

  LIBMTP_raw_device_t * rawdevices;
  int numrawdevices;
  LIBMTP_error_number_t err;

  err = LIBMTP_Detect_Raw_Devices(&rawdevices, &numrawdevices);
  switch(err) {
  case LIBMTP_ERROR_NONE:
    break;
  case LIBMTP_ERROR_NO_DEVICE_ATTACHED:
    // No devices is not really an error.
    fprintf(stdout, "   No raw devices found.\n");
    g_hash_table_remove_all(G_VFS_BACKEND_MTP(backend)->devices);
    goto exit;
  case LIBMTP_ERROR_CONNECTING:
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
                      "MTPDetect: There has been an error connecting.");
    goto exit;
  case LIBMTP_ERROR_MEMORY_ALLOCATION:
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_FILE_ERROR, G_FILE_ERROR_NOMEM,
                      "MTPDetect: Encountered a Memory Allocation Error.");
    goto exit;
  case LIBMTP_ERROR_GENERAL:
  default:
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      "MTPDetect: Unknown Error.");
    goto exit;
  }

  /* Iterate over connected MTP devices */
  fprintf(stdout, "Attempting to connect device(s) %d\n", numrawdevices);

  GList *keys = g_hash_table_get_keys(G_VFS_BACKEND_MTP(backend)->devices);

  int i;
  for (i = 0; i < numrawdevices; i++) {
    char *name;
    name = g_strdup_printf("0x%04X-0x%04X-%u-%u",
                           rawdevices[i].device_entry.vendor_id,
                           rawdevices[i].device_entry.product_id,
                           rawdevices[i].bus_location,
                           rawdevices[i].devnum);

    device = g_hash_table_lookup(G_VFS_BACKEND_MTP(backend)->devices, name);
    if (device) {
      GList *key = g_list_find_custom(keys, name, (GCompareFunc)strcmp);
      keys = g_list_delete_link(keys, key);
      g_free(name);
      continue;
    }

    device = LIBMTP_Open_Raw_Device_Uncached(&rawdevices[i]);
    if (device == NULL) {
      fprintf(stderr, "Unable to open raw device %d\n", i);
      g_free(name);
      continue;
    }

    g_print("Storing device %s\n", name);
    g_hash_table_insert(G_VFS_BACKEND_MTP(backend)->devices,
                        name, device);

    LIBMTP_Dump_Errorstack(device);
    LIBMTP_Clear_Errorstack(device);
  }

  // Remove devices that are now orphaned.
  GList *key;
  for (key = keys; key != NULL; key = key->next) {
    g_hash_table_remove(G_VFS_BACKEND_MTP(backend)->devices, key->data);
  }
  g_list_free(keys);

 exit:
  free(rawdevices);
  return g_hash_table_lookup(G_VFS_BACKEND_MTP(backend)->devices, id);
}

static void
get_device_info(LIBMTP_mtpdevice_t *device, const char *name, GFileInfo *info) {
        char *friendlyname;
        friendlyname = LIBMTP_Get_Friendlyname(device);
        if (friendlyname == NULL) {
          printf("Device: (NULL)\n");
        } else {
          printf("Device: %s\n", friendlyname);
        }

        g_file_info_set_file_type(info, G_FILE_TYPE_DIRECTORY);
        g_file_info_set_name(info, name);
        g_file_info_set_display_name(info, friendlyname == NULL ? "Unnamed Device" : friendlyname);
        g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
        g_file_info_set_content_type (info, "inode/directory");
        g_file_info_set_size (info, 0);
        GIcon *icon = g_themed_icon_new ("multimedia-player");
        g_file_info_set_icon (info, icon);
        g_object_unref (icon);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, TRUE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
        g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, TRUE); 
        free(friendlyname);
}

static void
get_storage_info(LIBMTP_devicestorage_t *storage, GFileInfo *info) {

  char *id = g_strdup_printf("%u", storage->id);
  g_file_info_set_name(info, id);
  g_free(id);

  g_file_info_set_display_name(info, storage->StorageDescription);
  g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
  g_file_info_set_content_type (info, "inode/directory");
  g_file_info_set_size (info, 0);

  GIcon *icon;
  switch (storage->StorageType) {
  case 1:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, TRUE);
    icon = g_themed_icon_new_with_default_fallbacks ("drive-harddisk");
    break;
  case 2:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, TRUE);
    icon = g_themed_icon_new_with_default_fallbacks ("media-memory-sd");
    break;
  case 4:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, FALSE);
    icon = g_themed_icon_new_with_default_fallbacks ("media-memory-sd");
    break;
  default:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, FALSE);
    icon = g_themed_icon_new_with_default_fallbacks ("drive-harddisk");
    break;
  }
  g_file_info_set_icon (info, icon);
  g_object_unref (icon);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE); 

  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, storage->FreeSpaceInBytes);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, storage->MaxCapacity);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "mtpfs");
}

static void
get_file_info(LIBMTP_mtpdevice_t *device, GFileInfo *info, LIBMTP_file_t *file) {
  GIcon *icon = NULL;
  char *content_type = NULL;

  char *id = g_strdup_printf("%u", file->item_id);
  g_file_info_set_name(info, id);
  g_free(id);

  g_file_info_set_display_name(info, file->filename);

  switch (file->filetype) {
  case LIBMTP_FILETYPE_FOLDER:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
    g_file_info_set_file_type(info, G_FILE_TYPE_DIRECTORY);
    g_file_info_set_content_type (info, "inode/directory");
    icon = g_themed_icon_new ("folder");
    break;
  default:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, FALSE);
    g_file_info_set_file_type(info, G_FILE_TYPE_REGULAR);
    content_type = g_content_type_guess(file->filename, NULL, 0, NULL);
    g_file_info_set_content_type(info, content_type);
    icon = g_content_type_get_icon(content_type);
    break;
  }

  g_file_info_set_size (info, file->filesize);

  GTimeVal modtime = { file->modificationdate, 0 };
  g_file_info_set_modification_time (info, &modtime);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, TRUE);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE, 0644);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID, getuid());
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID, getgid());
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_OWNER_USER, g_get_user_name());
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_OWNER_USER_REAL, g_get_real_name());
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_COPY_NAME, file->filename);


  if (icon != NULL) {
    g_file_info_set_icon (info, icon);
    g_object_unref (icon);
  }
  g_free(content_type);
}


static void
do_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  GFileInfo *info;

  gchar **elements = g_strsplit_set(filename, "/", -1);
  unsigned int ne = 0;
  for (ne = 0; elements[ne] != NULL; ne++);

  g_print ("(II) try_enumerate (filename = %s, n_elements = %d) \n", filename, ne);

  g_mutex_lock (&G_VFS_BACKEND_MTP(backend)->mutex);
  if (ne == 2 && elements[1][0] == '\0') {
    // Call without an id to populate hash table.
    get_device(backend, "", G_VFS_JOB(job));
    if (G_VFS_JOB(job)->failed) {
      goto exit;
    }

    GList *keys = g_hash_table_get_keys(G_VFS_BACKEND_MTP(backend)->devices);
    GList *key;
    for (key = keys; key != NULL; key = key->next) {
      /* Iterate over connected MTP devices */
        LIBMTP_mtpdevice_t *device;
        char *name = key->data;
        device = g_hash_table_lookup(G_VFS_BACKEND_MTP(backend)->devices, name);

        info = g_file_info_new();
        get_device_info(device, name, info);
        g_vfs_job_enumerate_add_info (job, info);
        g_object_unref(info);
    }
    g_list_free(keys);
  } else if (ne == 2) {
    LIBMTP_mtpdevice_t *device;
    LIBMTP_devicestorage_t *storage;
    device = get_device(backend, elements[1], G_VFS_JOB(job));
    if (device == NULL) {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Device does not exist");
      goto exit;
    }
    int ret = LIBMTP_Get_Storage(device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    if (ret != 0) {
      LIBMTP_Dump_Errorstack(device);
      LIBMTP_Clear_Errorstack(device);
      goto success;
    }
    for (storage = device->storage; storage != 0; storage = storage->next) {
      fprintf(stdout, "Storage: %s\n", storage->StorageDescription);
      info = g_file_info_new();
      get_storage_info(storage, info);
      g_vfs_job_enumerate_add_info (job, info);
      g_object_unref(info);
    }
  } else if (ne > 2) {
    LIBMTP_file_t *files;
    LIBMTP_file_t *file;

    LIBMTP_mtpdevice_t *device;
    device = get_device(backend, elements[1], G_VFS_JOB(job));
    if (device == NULL) {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Device does not exist");
      goto exit;
    }

    int pid = (ne == 3 ? -1 : strtol(elements[ne-1], NULL, 10));

    LIBMTP_Clear_Errorstack(device);
    files = LIBMTP_Get_Files_And_Folders(device, strtol(elements[2], NULL, 10), pid);
    if (files == NULL && LIBMTP_Get_Errorstack(device) != NULL) {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Failed to enumerate contents of %s\n", filename);
      LIBMTP_Dump_Errorstack(device);
      LIBMTP_Clear_Errorstack(device);
      goto exit;
    }
    for (file = files; file != NULL; file = file->next) {
        info = g_file_info_new();
        get_file_info(device, info, file);
        g_vfs_job_enumerate_add_info (job, info);
        g_object_unref(info);
    }
  }

 success:
  g_vfs_job_enumerate_done (job);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_strfreev(elements);
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
    goto success;
  }

  LIBMTP_mtpdevice_t *device;
  device = get_device(backend, elements[1], G_VFS_JOB(job));
  if (device == NULL) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      "Device does not exist");
    goto exit;
  }

  if (ne == 2) {
    get_device_info(device, elements[1], info);
  } else if (ne < 4) {
    LIBMTP_devicestorage_t *storage;
    int ret = LIBMTP_Get_Storage(device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    if (ret != 0) {
      LIBMTP_Dump_Errorstack(device);
      LIBMTP_Clear_Errorstack(device);
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Storage not found");
      goto exit;
    }
    for (storage = device->storage; storage != 0; storage = storage->next) {
      if (storage->id == strtol(elements[ne-1], NULL, 10)) {
        g_print("(III) found storage %u\n", storage->id);
        get_storage_info(storage, info);
      }
    }
  } else {
    LIBMTP_file_t *file = NULL;
    if (strtol(elements[ne-1], NULL, 10) == 0) {
      g_print ("(II) try get files and folders\n");
      int parent_id = -1;
      if (ne > 4) {
        parent_id = strtol(elements[ne-2], NULL, 10);
      }
      LIBMTP_file_t *files = LIBMTP_Get_Files_And_Folders(device, strtol(elements[2], NULL, 10),
                                                          parent_id);
      LIBMTP_file_t *i;
      for (i = files; i != NULL; i = i->next) {
        g_print ("(II) backup query (entity = %s, name = %s) \n", i->filename, elements[ne-1]);
        if (strcmp(i->filename, elements[ne-1]) == 0) {
          file = i;
          break;
        }
      }
    } else {
      file = LIBMTP_Get_Filemetadata(device, strtol(elements[ne-1], NULL, 10));
    }

    if (file != NULL) {
      get_file_info(device, info, file);
    } else {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Error while querying entity.");
      goto exit;
    }
  }

 success:
  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_strfreev(elements);
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
    device = get_device(backend, elements[1], G_VFS_JOB(job));
    if (device == NULL) {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Device does not exist");
      goto exit;
    }
    int ret = LIBMTP_Get_Storage(device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    if (ret != 0) {
      LIBMTP_Dump_Errorstack(device);
      LIBMTP_Clear_Errorstack(device);
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Storage does not exist");
      goto exit;
    }
    for (storage = device->storage; storage != 0; storage = storage->next) {
      if (storage->id == strtol(elements[2], NULL, 10)) {
        get_storage_info(storage, info);
      }
    }
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_strfreev(elements);
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

  GFileInfo *info = NULL;
  gchar **elements = g_strsplit_set(source, "/", -1);
  unsigned int ne = 0;
  for (ne = 0; elements[ne] != NULL; ne++);

  if (ne < 4) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                      "Can't download entity.");
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = get_device(backend, elements[1], G_VFS_JOB(job));
  if (device == NULL) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      "Device does not exist");
    goto exit;
  }
  LIBMTP_file_t *file = LIBMTP_Get_Filemetadata(device, strtol(elements[ne-1], NULL, 10));
  if (file == NULL) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      "File does not exist");
    goto exit;
  }

  info = g_file_info_new();
  get_file_info(device, info, file);
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
                              (LIBMTP_progressfunc_t)mtp_progress,
                              mtp_progress_data);
      g_free(mtp_progress_data);
      if (ret != 0) {
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR,
                          g_vfs_job_is_cancelled(G_VFS_JOB(job)) ?
                            G_IO_ERROR_CANCELLED :
                            G_IO_ERROR_FAILED,
                          "Error while downloading entity.");
        goto exit;
      }
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  if (info != NULL) {
    g_object_unref(info);
  }
  g_strfreev(elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP(backend)->mutex);
}


static void
do_push(GVfsBackend *backend,
                                GVfsJobPush *job,
                                const char *destination,
                                const char *local_path,
                                GFileCopyFlags flags,
                                gboolean remove_source,
                                GFileProgressCallback progress_callback,
                                gpointer progress_callback_data)
{
  g_print ("(II) do_push (filename = %s, local_path = %s) \n", destination, local_path);
  g_mutex_lock (&G_VFS_BACKEND_MTP(backend)->mutex);

  GFile *file = NULL;
  GFileInfo *info = NULL;
  gchar **elements = g_strsplit_set(destination, "/", -1);
  unsigned int ne = 0;
  for (ne = 0; elements[ne] != NULL; ne++);

  if (ne < 4) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                      "Can't upload to this location.");
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = get_device(backend, elements[1], G_VFS_JOB(job));
  if (device == NULL) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      "Device does not exist");
    goto exit;
  }

  int parent_id = 0;

  if (ne > 4) {
    parent_id = strtol(elements[ne-2], NULL, 10);
  }

  file = g_file_new_for_path (local_path);
  if (!file) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      "Can't get file to upload.");
    goto exit;
  }

  GError *error = NULL;
  info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                           G_FILE_QUERY_INFO_NONE, G_VFS_JOB(job)->cancellable,
                           &error);
  if (!info) {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
    g_error_free (error);
    goto exit;
  }

  LIBMTP_file_t *mtpfile = LIBMTP_new_file_t();
  mtpfile->filename = strdup(elements[ne-1]);
  mtpfile->parent_id = parent_id;
  mtpfile->storage_id = strtol(elements[2], NULL, 10);
  mtpfile->filetype = LIBMTP_FILETYPE_UNKNOWN; 
  mtpfile->filesize = g_file_info_get_size(info);

  MtpProgressData *mtp_progress_data = g_new0(MtpProgressData, 1);
  mtp_progress_data->progress_callback = progress_callback;
  mtp_progress_data->progress_callback_data = progress_callback_data;
  mtp_progress_data->job = G_VFS_JOB(job);
  int ret = LIBMTP_Send_File_From_File(device, local_path, mtpfile,
                                       (LIBMTP_progressfunc_t)mtp_progress,
                                       mtp_progress_data);
  g_free(mtp_progress_data);
  LIBMTP_destroy_file_t(mtpfile);
  if (ret != 0) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Error while uploading entity.");
    goto exit;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  if (file) {
    g_object_unref(file);
  }
  if (info) {
    g_object_unref(info);
  }
  g_strfreev(elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP(backend)->mutex);
}


static void
do_make_directory (GVfsBackend *backend,
                    GVfsJobMakeDirectory *job,
                    const char *filename)
{
  g_print ("(II) try_make_directory (filename = %s) \n", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP(backend)->mutex);

  gchar **elements = g_strsplit_set(filename, "/", -1);
  unsigned int ne = 0;
  for (ne = 0; elements[ne] != NULL; ne++);

  if (ne < 4) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Can't make directory in this location.");
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = get_device(backend, elements[1], G_VFS_JOB(job));
  if (device == NULL) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Device does not exist");
    goto exit;
  }

  int parent_id = 0;
  if (ne > 4) {
    parent_id = strtol(elements[ne-2], NULL, 10);
  }

  int ret = LIBMTP_Create_Folder(device, elements[ne-1], parent_id, strtol(elements[2], NULL, 10));
  if (ret != 0) {
    LIBMTP_Dump_Errorstack(device);
    LIBMTP_Clear_Errorstack(device);
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Error while creating directory.");
    goto exit;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_strfreev(elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP(backend)->mutex);
}


static void
do_delete (GVfsBackend *backend,
            GVfsJobDelete *job,
            const char *filename)
{
  g_print ("(II) try_delete (filename = %s) \n", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP(backend)->mutex);

  gchar **elements = g_strsplit_set(filename, "/", -1);
  unsigned int ne = 0;
  for (ne = 0; elements[ne] != NULL; ne++);

  if (ne < 4) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Can't delete entity.");
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = get_device(backend, elements[1], G_VFS_JOB(job));
  if (device == NULL) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Device does not exist");
    goto exit;
  }

  int ret = LIBMTP_Delete_Object(device, strtol(elements[ne-1], NULL, 10));
  if (ret != 0) {
    LIBMTP_Dump_Errorstack(device);
    LIBMTP_Clear_Errorstack(device);
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Error while deleting entity.");
    goto exit;
  }
  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_strfreev(elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP(backend)->mutex);
}


//  aka 'rename'
static void
do_set_display_name (GVfsBackend *backend,
                      GVfsJobSetDisplayName *job,
                      const char *filename,
                      const char *display_name)
{
  g_print ("(II) try_set_display_name '%s' --> '%s' \n", filename, display_name);
  g_mutex_lock (&G_VFS_BACKEND_MTP(backend)->mutex);

  gchar **elements = g_strsplit_set(filename, "/", -1);
  unsigned int ne = 0;
  for (ne = 0; elements[ne] != NULL; ne++);

  if (ne == 3) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "Can't rename storage entities.");
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = get_device(backend, elements[1], G_VFS_JOB(job));
  if (device == NULL) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      "Device does not exist");
    goto exit;
  }

  if (ne == 2) {
    int ret = LIBMTP_Set_Friendlyname(device, display_name);
    if (ret != 0) {
        LIBMTP_Dump_Errorstack(device);
        LIBMTP_Clear_Errorstack(device);
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Error while renaming device.");
        goto exit;
    }
  } else {
    LIBMTP_file_t *file = LIBMTP_Get_Filemetadata(device, strtol(elements[ne-1], NULL, 10));
    int ret = LIBMTP_Set_File_Name(device, file, display_name);
    if (ret != 0) {
        LIBMTP_Dump_Errorstack(device);
        LIBMTP_Clear_Errorstack(device);
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR, G_IO_ERROR_FAILED,
                          "Error while renaming entity.");
        goto exit;
    }
  }
  g_vfs_job_set_display_name_set_new_path(job, filename);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_strfreev(elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP(backend)->mutex);
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
  backend_class->query_info = do_query_info;
  backend_class->enumerate = do_enumerate;
  backend_class->query_fs_info = do_query_fs_info;
  backend_class->pull = do_pull;
  backend_class->push = do_push;
//  backend_class->replace = do_replace;
  backend_class->make_directory = do_make_directory;
  backend_class->delete = do_delete;
  backend_class->set_display_name = do_set_display_name;
//  backend_class->set_attribute = do_set_attribute;
//  backend_class->create_dir_monitor = do_create_dir_monitor;
//  backend_class->create_file_monitor = do_create_file_monitor;
//  backend_class->query_settable_attributes = do_query_settable_attributes;
//  backend_class->query_writable_namespaces = do_query_writable_namespaces;
}
