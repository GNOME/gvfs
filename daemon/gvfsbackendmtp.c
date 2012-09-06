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
#include "gvfsicon.h"
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
  GMountSpec *mount_spec;

  g_mutex_init(&backend->mutex);
  g_vfs_backend_set_display_name (G_VFS_BACKEND(backend), "mtp");
  g_vfs_backend_set_icon_name (G_VFS_BACKEND(backend), "multimedia-player");

  mount_spec = g_mount_spec_new ("mtp");
  g_vfs_backend_set_mount_spec (G_VFS_BACKEND(backend), mount_spec);
  g_mount_spec_unref (mount_spec);

  backend->monitors = g_hash_table_new(g_direct_hash, g_direct_equal);

  g_print ("(II) g_vfs_backend_mtp_init done.\n");
}

static void
g_vfs_backend_mtp_finalize (GObject *object)
{
  GVfsBackendMtp *backend;

  g_print ("(II) g_vfs_backend_mtp_finalize \n");

  backend = G_VFS_BACKEND_MTP (object);

  g_hash_table_unref(backend->monitors);
  g_mutex_clear(&backend->mutex);

  if (G_OBJECT_CLASS (g_vfs_backend_mtp_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_mtp_parent_class)->finalize) (object);
}


/************************************************
 * Monitors
 */

static void
do_create_dir_monitor (GVfsBackend *backend,
                       GVfsJobCreateMonitor *job,
                       const char *filename,
                       GFileMonitorFlags flags)
{
  GVfsBackendMtp *mtp_backend = G_VFS_BACKEND_MTP (backend);

  g_print ("create_dir_monitor (%s)\n", filename);

  GVfsMonitor *vfs_monitor = g_vfs_monitor_new (backend);

  g_object_set_data_full(G_OBJECT(vfs_monitor), "gvfsbackendmtp:path",
                         g_strdup(filename), g_free);

  g_vfs_job_create_monitor_set_monitor (job, vfs_monitor);
  g_hash_table_insert(mtp_backend->monitors, vfs_monitor, NULL);
  g_object_weak_ref(G_OBJECT(vfs_monitor), (GWeakNotify)g_hash_table_remove, mtp_backend->monitors);
  g_object_unref (vfs_monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}


static void
do_create_file_monitor (GVfsBackend *backend,
                        GVfsJobCreateMonitor *job,
                        const char *filename,
                        GFileMonitorFlags flags)
{
  GVfsBackendMtp *mtp_backend = G_VFS_BACKEND_MTP (backend);

  g_print ("create_file_monitor (%s)\n", filename);

  GVfsMonitor *vfs_monitor = g_vfs_monitor_new (backend);

  g_object_set_data_full(G_OBJECT(vfs_monitor), "gvfsbackendmtp:path",
                         g_strdup(filename), g_free);

  g_vfs_job_create_monitor_set_monitor (job, vfs_monitor);
  g_hash_table_insert(mtp_backend->monitors, vfs_monitor, NULL);
  g_object_weak_ref(G_OBJECT(vfs_monitor), (GWeakNotify)g_hash_table_remove, mtp_backend->monitors);
  g_object_unref (vfs_monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
emit_event_internal(GVfsMonitor *monitor,
                    const char *path,
                    GFileMonitorEvent event)
{
  char *dir = g_dirname(path);
  const char *monitored_path = g_object_get_data(G_OBJECT(monitor), "gvfsbackendmtp:path");
  if (g_strcmp0(dir, monitored_path) == 0) {
    g_print("Event %d on directory %s for %s\n", event, dir, path);
    g_vfs_monitor_emit_event(monitor, event, path, NULL);
  } else if (g_strcmp0(path, monitored_path) == 0) {
    g_print("Event %d on file %s\n", event, path);
    g_vfs_monitor_emit_event(monitor, event, path, NULL);
  }
  g_free(dir);
}

static void
emit_create_event(gpointer key,
                  gpointer value,
                  gpointer user_data)
{
  emit_event_internal(key, user_data, G_FILE_MONITOR_EVENT_CREATED);
}

static void
emit_delete_event(gpointer key,
                  gpointer value,
                  gpointer user_data)
{
  emit_event_internal(key, user_data, G_FILE_MONITOR_EVENT_DELETED);
}

static void
emit_change_event(gpointer key,
                  gpointer value,
                  gpointer user_data)
{
  emit_event_internal(key, user_data, G_FILE_MONITOR_EVENT_CHANGED);
}


static void
fail_job (GVfsJob *job, LIBMTP_mtpdevice_t *device)
{
  LIBMTP_error_t *error = LIBMTP_Get_Errorstack(device);

  g_vfs_job_failed (job, G_IO_ERROR,
                    g_vfs_job_is_cancelled(job) ?
                      G_IO_ERROR_CANCELLED :
                      G_IO_ERROR_FAILED,
                    g_strrstr(error->error_text, ":") + 1);

  LIBMTP_Clear_Errorstack(device);
}


/************************************************
 * 	  Mount
 * 
 */

static LIBMTP_mtpdevice_t *
get_device(GVfsBackend *backend, const char *id, GVfsJob *job);


static void
on_uevent (GUdevClient *client, gchar *action, GUdevDevice *device, gpointer user_data)
{
  const char *dev_path = g_udev_device_get_device_file (device);
  g_print ("on_uevent action %s, device %s\n", action, dev_path);

  if (dev_path == NULL) {
    return;
  }

  GVfsBackendMtp *op_backend = G_VFS_BACKEND_MTP (user_data);

  if (g_strcmp0(op_backend->dev_path, dev_path) == 0 &&
      strcmp (action, "remove") == 0) {
    g_print("Quiting after remove event on device %s\n", dev_path);
    /* TODO: need a cleaner way to force unmount ourselves */
    exit (1);
  }
}

static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  const char *host;
  GError *error = NULL;
  GMountSpec *mtp_mount_spec;

  g_print ("(II) try_mount \n");

  /* TODO: Hmm.. apparently we have to set the mount spec in
   * try_mount(); doing it in mount() do_won't work.. 
   */
  host = g_mount_spec_get (mount_spec, "host");
  g_print ("  host=%s\n", host);
  if (host == NULL)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _("No device specified"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return TRUE;
    }

  mtp_mount_spec = g_mount_spec_new ("mtp");
  g_mount_spec_set (mtp_mount_spec, "host", host);
  g_vfs_backend_set_mount_spec (backend, mtp_mount_spec);
  g_mount_spec_unref (mtp_mount_spec);
  return FALSE;
}

#if HAVE_LIBMTP_READ_EVENT
static gpointer
check_event(gpointer user_data)
{
  GVfsBackendMtp *backend = user_data;

  LIBMTP_event_t event;
  int ret = 0;
  while (ret == 0) {
    uint32_t param1;
    char *path;
    ret = LIBMTP_Read_Event(backend->device, &event, &param1);
    switch (event) {
    case LIBMTP_EVENT_STORE_ADDED:
      path = g_strdup_printf ("/%u", param1);
      g_mutex_lock (&backend->mutex);
      g_hash_table_foreach (backend->monitors, emit_create_event, path);
      g_mutex_unlock (&backend->mutex);
      g_free (path);
      break;
    default:
      break;
    }
  }
  return NULL;
}
#endif

static void
do_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendMtp *op_backend = G_VFS_BACKEND_MTP (backend);

  g_print ("(II) do_mount \n");

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

  const char *host = g_mount_spec_get (mount_spec, "host");

  /* turn usb:001,041 string into an udev device name */
  if (!g_str_has_prefix (host, "[usb:")) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "Unexpected host uri format.");
    return;
  }

  char *comma;
  char *dev_path = g_strconcat ("/dev/bus/usb/", host + 5, NULL);
  if ((comma = strchr (dev_path, ',')) == NULL) {
    g_free (dev_path);
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "Malformed host uri.");
    return;
  }
  *comma = '/';
  dev_path[strlen(dev_path) -1] = '\0';
  g_print("Parsed '%s' into device name %s", host, dev_path);

  /* find corresponding GUdevDevice */
  if (!g_udev_client_query_by_device_file (op_backend->gudev_client, dev_path)) {
    g_free(dev_path);
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      "Couldn't find matching udev device.");
    return;
  }

  op_backend->dev_path = dev_path;

  LIBMTP_Init();

  get_device(backend, host, G_VFS_JOB(job));
  if (!G_VFS_JOB(job)->failed) {
    GMountSpec *mtp_mount_spec = g_mount_spec_new ("mtp");
    g_mount_spec_set (mtp_mount_spec, "host", host);
    g_vfs_backend_set_mount_spec (backend, mtp_mount_spec);
    g_mount_spec_unref (mtp_mount_spec);

    g_vfs_job_succeeded (G_VFS_JOB (job));

#if HAVE_LIBMTP_READ_EVENT
    g_thread_new("events", check_event, backend);
#endif
  }
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
  g_free(op_backend->dev_path);
  LIBMTP_Release_Device(op_backend->device);
  g_vfs_job_succeeded (G_VFS_JOB(job));
}







/************************************************
 * 	  Queries
 * 
 */

LIBMTP_mtpdevice_t *
get_device(GVfsBackend *backend, const char *id, GVfsJob *job) {
  LIBMTP_mtpdevice_t *device = NULL;

  if (G_VFS_BACKEND_MTP(backend)->device != NULL) {
    g_print("Returning device %p\n", device);
    return G_VFS_BACKEND_MTP(backend)->device;
  }

  LIBMTP_raw_device_t * rawdevices;
  int numrawdevices;
  LIBMTP_error_number_t err;

  err = LIBMTP_Detect_Raw_Devices(&rawdevices, &numrawdevices);
  switch(err) {
  case LIBMTP_ERROR_NONE:
    break;
  case LIBMTP_ERROR_NO_DEVICE_ATTACHED:
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      "MTPDetect: No devices found.");
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

  int i;
  for (i = 0; i < numrawdevices; i++) {
    char *name;
    name = g_strdup_printf("[usb:%03u,%03u]", rawdevices[i].bus_location, rawdevices[i].devnum);

    if (strcmp(id, name) == 0) {
      device = LIBMTP_Open_Raw_Device_Uncached(&rawdevices[i]);
      if (device == NULL) {
        fprintf(stderr, "Unable to open raw device %d\n", i);
        g_free(name);
        return device;
      }

      g_print("Storing device %s\n", name);
      G_VFS_BACKEND_MTP(backend)->device = device;

      LIBMTP_Dump_Errorstack(device);
      LIBMTP_Clear_Errorstack(device);
      break;
    } else {
      g_free(name);
    }
  }

 exit:
  return device;
}

static void
get_device_info(GVfsBackendMtp *backend, GFileInfo *info) {
  LIBMTP_mtpdevice_t *device = backend->device;
  const char *name = g_mount_spec_get(g_vfs_backend_get_mount_spec(G_VFS_BACKEND(backend)), "host");

  g_file_info_set_file_type(info, G_FILE_TYPE_DIRECTORY);
  g_file_info_set_name(info, name);

  char *friendlyname = LIBMTP_Get_Friendlyname(device);
  g_file_info_set_display_name(info, friendlyname == NULL ? "Unnamed Device" : friendlyname);
  free(friendlyname);

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

  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "mtpfs");

  int ret = LIBMTP_Get_Storage(device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
  if (ret != 0) {
    LIBMTP_Dump_Errorstack(device);
    LIBMTP_Clear_Errorstack(device);
    return;
  }
  guint64 freeSpace = 0;
  guint64 maxSpace = 0;
  LIBMTP_devicestorage_t *storage;
  for (storage = device->storage; storage != 0; storage = storage->next) {
    freeSpace += storage->FreeSpaceInBytes;
    maxSpace += storage->MaxCapacity;
  }

  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, freeSpace);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, maxSpace);
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
get_file_info(GVfsBackend *backend, LIBMTP_mtpdevice_t *device, GFileInfo *info, LIBMTP_file_t *file) {
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


#if HAVE_LIBMTP_GET_THUMBNAIL
  if (LIBMTP_FILETYPE_IS_IMAGE(file->filetype) ||
      LIBMTP_FILETYPE_IS_VIDEO(file->filetype) ||
      LIBMTP_FILETYPE_IS_AUDIOVIDEO(file->filetype)) {

    char *icon_id;
    GIcon *icon;
    GMountSpec *mount_spec;

    mount_spec = g_vfs_backend_get_mount_spec (backend);
    icon_id = g_strdup_printf("%u", file->item_id);
    icon = g_vfs_icon_new (mount_spec,
                           icon_id);
    g_file_info_set_attribute_object (info,
                                      G_FILE_ATTRIBUTE_PREVIEW_ICON,
                                      G_OBJECT (icon));
    g_object_unref (icon);
    g_free (icon_id);
  }
#endif

  g_file_info_set_size (info, file->filesize);

  GTimeVal modtime = { file->modificationdate, 0 };
  g_file_info_set_modification_time (info, &modtime);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, TRUE);
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
  GVfsBackendMtp *op_backend = G_VFS_BACKEND_MTP(backend);
  GFileInfo *info;

  gchar **elements = g_strsplit_set(filename, "/", -1);
  unsigned int ne = 0;
  for (ne = 0; elements[ne] != NULL; ne++);

  g_print ("(II) try_enumerate (filename = %s, n_elements = %d) \n", filename, ne);

  g_mutex_lock (&G_VFS_BACKEND_MTP(backend)->mutex);

  LIBMTP_mtpdevice_t *device;
  device = op_backend->device;

  if (ne == 2 && elements[1][0] == '\0') {
    LIBMTP_devicestorage_t *storage;

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
  } else {
    LIBMTP_file_t *files;

    int pid = (ne == 2 ? -1 : strtol(elements[ne-1], NULL, 10));

    LIBMTP_Clear_Errorstack(device);
    files = LIBMTP_Get_Files_And_Folders(device, strtol(elements[1], NULL, 10), pid);
    if (files == NULL && LIBMTP_Get_Errorstack(device) != NULL) {
      fail_job(G_VFS_JOB(job), device);
      goto exit;
    }
    while (files != NULL) {
      LIBMTP_file_t *file = files;
      files = files->next;

      info = g_file_info_new();
      get_file_info(backend, device, info, file);
      g_vfs_job_enumerate_add_info (job, info);
      g_object_unref(info);

      LIBMTP_destroy_file_t(file);
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

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP(backend)->device;

  if (ne == 2 && elements[1][0] == '\0') {
    get_device_info(G_VFS_BACKEND_MTP(backend), info);
  } else if (ne < 3) {
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
      if (ne > 3) {
        parent_id = strtol(elements[ne-2], NULL, 10);
      }
      LIBMTP_file_t *i = LIBMTP_Get_Files_And_Folders(device, strtol(elements[1], NULL, 10),
                                                      parent_id);
      while (i != NULL) {
        g_print ("(II) backup query (entity = %s, name = %s) \n", i->filename, elements[ne-1]);
        if (strcmp(i->filename, elements[ne-1]) == 0) {
          file = i;
          i = i->next;
          break;
        } else {
          LIBMTP_file_t *tmp = i;
          i = i->next;
          LIBMTP_destroy_file_t(tmp);
        }
      }
      while (i != NULL) {
        LIBMTP_file_t *tmp = i;
        i = i->next;
        LIBMTP_destroy_file_t(tmp);
      }
    } else {
      file = LIBMTP_Get_Filemetadata(device, strtol(elements[ne-1], NULL, 10));
    }

    if (file != NULL) {
      get_file_info(backend, device, info, file);
      LIBMTP_destroy_file_t(file);
    } else {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Error while querying entity.");
      goto exit;
    }
  }

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

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP(backend)->device;

  if (ne == 2 && elements[1][0] == '\0') {
    get_device_info(G_VFS_BACKEND_MTP(backend), info);
  } else {
    LIBMTP_devicestorage_t *storage;
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
      if (storage->id == strtol(elements[1], NULL, 10)) {
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
  if (data->progress_callback) {
    data->progress_callback(sent, total, data->progress_callback_data);
  }
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

  if (ne < 3) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                      "Can't download entity.");
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP(backend)->device;

  LIBMTP_file_t *file = LIBMTP_Get_Filemetadata(device, strtol(elements[ne-1], NULL, 10));
  if (file == NULL) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      "File does not exist");
    goto exit;
  }

  info = g_file_info_new();
  get_file_info(backend, device, info, file);
  LIBMTP_destroy_file_t(file);
  file = NULL;
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
        fail_job(G_VFS_JOB(job), device);
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

  if (ne < 3) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                      "Can't upload to this location.");
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP(backend)->device;

  int parent_id = 0;

  if (ne > 3) {
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
  mtpfile->storage_id = strtol(elements[1], NULL, 10);
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
    LIBMTP_Dump_Errorstack(device);
    LIBMTP_Clear_Errorstack(device);
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Error while uploading entity.");
    goto exit;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach(G_VFS_BACKEND_MTP(backend)->monitors, emit_create_event, (char *)destination);

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

  if (ne < 3) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Can't make directory in this location.");
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP(backend)->device;

  int parent_id = 0;
  if (ne > 3) {
    parent_id = strtol(elements[ne-2], NULL, 10);
  }

  int ret = LIBMTP_Create_Folder(device, elements[ne-1], parent_id, strtol(elements[1], NULL, 10));
  if (ret == 0) {
    LIBMTP_Dump_Errorstack(device);
    LIBMTP_Clear_Errorstack(device);
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Error while creating directory.");
    goto exit;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach(G_VFS_BACKEND_MTP(backend)->monitors, emit_create_event, (char *)filename);

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

  if (ne < 3) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      "Can't delete entity.");
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP(backend)->device;

  int ret = LIBMTP_Delete_Object(device, strtol(elements[ne-1], NULL, 10));
  if (ret != 0) {
    fail_job(G_VFS_JOB(job), device);
    goto exit;
  }
  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach(G_VFS_BACKEND_MTP(backend)->monitors, emit_delete_event, (char *)filename);

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

  if (ne < 3) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      "Can't rename storage entities.");
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP(backend)->device;

  LIBMTP_file_t *file = LIBMTP_Get_Filemetadata(device, strtol(elements[ne-1], NULL, 10));
  int ret = LIBMTP_Set_File_Name(device, file, display_name);
  if (ret != 0) {
    fail_job(G_VFS_JOB(job), device);
    goto exit;
  }
  LIBMTP_destroy_file_t(file);
  file = NULL;
  g_vfs_job_set_display_name_set_new_path(job, filename);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach(G_VFS_BACKEND_MTP(backend)->monitors, emit_change_event, (char *)filename);

 exit:
  g_strfreev(elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP(backend)->mutex);
}


#if HAVE_LIBMTP_GET_THUMBNAIL
static void
do_open_icon_for_read (GVfsBackend *backend,
                       GVfsJobOpenIconForRead *job,
                       const char *icon_id)
{
  g_print ("open_icon_for_read (%s)\n", icon_id);
  g_mutex_lock (&G_VFS_BACKEND_MTP(backend)->mutex);

  guint id = strtol(icon_id, NULL, 10);

  if (id > 0) {
    unsigned char *data;
    unsigned int size;
    int ret = LIBMTP_Get_Thumbnail(G_VFS_BACKEND_MTP(backend)->device, id,
                                   &data, &size);
    if (ret == 0) {
      g_print("File %u has thumbnail: %u\n", id, size);
      GByteArray *bytes = g_byte_array_sized_new(size);
      g_byte_array_append(bytes, data, size);
      free(data);
      g_vfs_job_open_for_read_set_can_seek (G_VFS_JOB_OPEN_FOR_READ(job), FALSE);
      g_vfs_job_open_for_read_set_handle (G_VFS_JOB_OPEN_FOR_READ(job), bytes);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    } else {
      LIBMTP_filesampledata_t *sample_data = LIBMTP_new_filesampledata_t();
      ret = LIBMTP_Get_Representative_Sample(G_VFS_BACKEND_MTP(backend)->device, id, sample_data);
      if (ret == 0) {
        g_print("File %u has sampledata: %u\n", id, size);
        GByteArray *bytes = g_byte_array_sized_new(sample_data->size);
        g_byte_array_append(bytes, sample_data->data, sample_data->size);
        LIBMTP_destroy_filesampledata_t(sample_data);
        g_vfs_job_open_for_read_set_can_seek (G_VFS_JOB_OPEN_FOR_READ(job), FALSE);
        g_vfs_job_open_for_read_set_handle (G_VFS_JOB_OPEN_FOR_READ(job), bytes);
        g_vfs_job_succeeded (G_VFS_JOB (job));
      } else {
        g_print("File %u has no thumbnail:\n", id);
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR,
                          G_IO_ERROR_NOT_FOUND,
                          _("No thumbnail for entity '%s'"),
                          icon_id);
      }
    }
  } else {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR,
                      G_IO_ERROR_INVALID_ARGUMENT,
                      _("Malformed icon identifier '%s'"),
                      icon_id);
  }
  g_mutex_unlock (&G_VFS_BACKEND_MTP(backend)->mutex);
}

static gboolean
try_read (GVfsBackend *backend,
          GVfsJobRead *job,
          GVfsBackendHandle handle,
          char *buffer,
          gsize bytes_requested)
{
  GByteArray *bytes = handle;

  g_print ("try_read (%u %lu)\n", bytes->len, bytes_requested);

  gsize bytes_to_copy =  MIN(bytes->len, bytes_requested);
  if (bytes_to_copy == 0) {
    goto out;
  }
  memcpy(buffer, bytes->data, bytes_to_copy);
  g_byte_array_remove_range(bytes, 0, bytes_to_copy);

 out:
  g_vfs_job_read_set_size (job, bytes_to_copy);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
}

static void
do_close_read (GVfsBackend *backend,
                GVfsJobCloseRead *job,
                GVfsBackendHandle handle)
{
  g_print ("do_close_read\n");
  g_byte_array_unref(handle);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}
#endif /* HAVE_LIBMTP_GET_THUMBNAIL */


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

  backend_class->try_mount = try_mount;
  backend_class->mount = do_mount;
  backend_class->unmount = do_unmount;
  backend_class->query_info = do_query_info;
  backend_class->enumerate = do_enumerate;
  backend_class->query_fs_info = do_query_fs_info;
  backend_class->pull = do_pull;
  backend_class->push = do_push;
  backend_class->make_directory = do_make_directory;
  backend_class->delete = do_delete;
  backend_class->set_display_name = do_set_display_name;
  backend_class->create_dir_monitor = do_create_dir_monitor;
  backend_class->create_file_monitor = do_create_file_monitor;
#if HAVE_LIBMTP_GET_THUMBNAIL
  backend_class->open_icon_for_read = do_open_icon_for_read;
  backend_class->try_read = try_read;
  backend_class->close_read = do_close_read;
#endif
}
