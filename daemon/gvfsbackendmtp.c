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
 * Public License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
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
#if HAVE_LIBUSB
#include <libusb.h>
#endif

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
#include "gvfsjobmakedirectory.h"
#include "gvfsmonitor.h"
#include "gvfsgphoto2utils.h"


/************************************************
 * Storage constants copied from ptp.h
 *
 * ptp.h is treated as a private header by libmtp
 ************************************************/

/* PTP Storage Types */

#define PTP_ST_Undefined                        0x0000
#define PTP_ST_FixedROM                         0x0001
#define PTP_ST_RemovableROM                     0x0002
#define PTP_ST_FixedRAM                         0x0003
#define PTP_ST_RemovableRAM                     0x0004


/************************************************
 * Constants
 ************************************************/

#if HAVE_LIBUSB
#define EVENT_POLL_PERIOD { 3600, 0 }
#else
#define EVENT_POLL_PERIOD { 1, 0 }
#endif

/************************************************
 * Private Types
 ************************************************/

typedef enum {
  HANDLE_FILE,
  HANDLE_PREVIEW,
} HandleType;

typedef struct {
  HandleType handle_type;
  uint32_t id;
  goffset offset;
  gsize size;

  /* For previews only */
  GByteArray *bytes;

  /* For write only */
  GVfsJobOpenForWriteMode mode;
} RWHandle;

typedef struct {
  uint32_t storage;
  uint32_t id;
} CacheEntry;

typedef struct {
  LIBMTP_event_t event;
  uint32_t param1;
} EventData;


/************************************************
 * Static prototypes
 ************************************************/

static void
emit_delete_event (gpointer key,
                   gpointer value,
                   gpointer user_data);

static void
handle_event (EventData *data, GVfsBackendMtp *backend);


/************************************************
 * Storage name helper
 ************************************************/

/**
 * create_storage_name:
 *
 * Returns a unique, printable storage name for a LIBMTP_devicestorage_t
 * based on its StorageDescription, appending the storage ID if necessary
 * to make it unique.
 *
 * The caller takes ownership of the returned string.
 * This function never returns NULL strings.
 *
 * The passed-in `storage->StorageDescription` may be NULL.
 */
static char *create_storage_name (const LIBMTP_devicestorage_t *storage)
{
  /* The optional post-fixing of storage's name with ID requires us to
     know in advance whether the storage's description string is unique
     or not. Since this function is called in several places, it is
     safest to perform this check here, each time that storage name needs
     to be created. */
  /* TODO: The returned name is not unique if suffix-adding happens
           to introduce a collision with another storage's unsuffixed
           description; unlikely but possible. */
  gboolean is_unique = TRUE;
  const LIBMTP_devicestorage_t *tmp_storage;

  /* `storage->StorageDescription` may be NULL, so we ensure to only use
     functions that can handle this, like `g_strcmp0()`. */

  /* Forward search for duplicates */
  for (tmp_storage = storage->next; tmp_storage != 0; tmp_storage = tmp_storage->next) {
    if (!g_strcmp0 (storage->StorageDescription, tmp_storage->StorageDescription)) {
      is_unique = FALSE;
      break;
    }
  }

  /* Backward search, if necessary */
  if (is_unique) {
    for (tmp_storage = storage->prev; tmp_storage != 0; tmp_storage = tmp_storage->prev) {
      /* Compare descriptions */
      if (!g_strcmp0 (storage->StorageDescription, tmp_storage->StorageDescription)) {
        is_unique = FALSE;
        break;
      }
    }
  }

  /* If description is unique, we can use it as storage name; otherwise,
     we add storage ID to it */
  if (is_unique) {
    /* Never return a NULL string (`g_strdup` returns NULL on NULL).
       Use the storage ID on empty strings to avoid duplicate entries
       for devices with multiple storages without description. */
    if (storage->StorageDescription && strlen (storage->StorageDescription) > 0) {
      return g_strdup (storage->StorageDescription);
    } else {
      /* Translators: This is shown as the name for MTP devices
       *              without StorageDescription.
       *              The %X is the formatted storage ID. */
      return g_strdup_printf (_("Storage (%X)"), storage->id);
    }
  } else {
    return g_strdup_printf ("%s (%X)", storage->StorageDescription, storage->id);
  }
}


/************************************************
 * Cache Helpers
 ************************************************/

static void
add_cache_entry (GVfsBackendMtp *backend,
                 char *path,
                 uint32_t storage,
                 uint32_t id)
{
  CacheEntry *entry = g_new0 (CacheEntry, 1);
  entry->storage = storage;
  entry->id = id;
  g_debug ("(II) add_cache_entry: %s: %X, %X\n",
           path, entry->storage, entry->id);
  g_hash_table_replace (backend->file_cache,
                        path, entry);
}


static char *
build_partial_path (char **elements,
                    unsigned int ne)
{
  char **pe = g_new0 (char *, ne + 2);
  int i;
  pe[0] = g_strdup("/");
  for (i = 0; i < ne; i++) {
    pe[i + 1] = elements[i];
  }
  char *path = g_build_filenamev(pe);
  g_free (pe);
  return path;
}

/**
 * get_file_for_filename:
 *
 * Get the entity ID for an element given its filename and
 * the IDs of its parents.
 *
 * Called with backend mutex lock held.
 */
static void
add_cache_entries_for_filename (GVfsBackendMtp *backend,
                                const char *path)
{
  LIBMTP_file_t *file = NULL;
  LIBMTP_mtpdevice_t *device = backend->device;

  gchar **elements = g_strsplit_set (path, "/", -1);
  unsigned int ne = g_strv_length (elements);

  g_debug ("(III) add_cache_entries_for_filename: %s, %u\n", path, ne);

  if (ne < 2) {
    g_debug ("(III) Ignoring query on invalid path\n");
    goto exit;
  }

  /* Identify Storage */
  LIBMTP_devicestorage_t *storage;

  int ret = LIBMTP_Get_Storage (device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
  if (ret != 0) {
    LIBMTP_Dump_Errorstack (device);
    LIBMTP_Clear_Errorstack (device);
    goto exit;
  }
  for (storage = device->storage; storage != 0; storage = storage->next) {
    /* Construct the name for storage and compare it to first element of path */
    char *storage_name = create_storage_name (storage);
    int is_equal = !g_strcmp0 (elements[1], storage_name);
    g_free(storage_name);

    if (is_equal) {
      char *partial = build_partial_path (elements, 2);
      add_cache_entry (backend, partial, storage->id, -1);
      break;
    }
  }
  if (!storage) {
    g_debug ("(III) Ignoring query on invalid storage\n");
    goto exit;
  }

  long parent_id = -1;
  int i;

  for (i = 2; i < ne; i++) {
    LIBMTP_file_t *f =
      LIBMTP_Get_Files_And_Folders (device, storage->id, parent_id);
    while (f != NULL) {
      g_debug ("(III) query (entity = %s, name = %s)\n", f->filename, elements[i]);
      if (strcmp (f->filename, elements[i]) == 0) {
        file = f;
        f = f->next;
        char *partial = build_partial_path (elements, i + 1);
        add_cache_entry (backend, partial, file->storage_id, file->item_id);
        break;
      } else {
        LIBMTP_file_t *tmp = f;
        f = f->next;
        LIBMTP_destroy_file_t (tmp);
      }
    }
    while (f != NULL) {
      LIBMTP_file_t *tmp = f;
      f = f->next;
      LIBMTP_destroy_file_t (tmp);
    }
    if (!file) {
      g_debug ("(III) Ignoring query for non-existent file\n");
      goto exit;
    }
    parent_id = file->item_id;
  }

 exit:
  g_strfreev (elements);

  g_debug ("(III) add_cache_entries_for_filename done\n");
}


static CacheEntry *get_cache_entry (GVfsBackendMtp *backend,
                                    const char *path)
{
  g_debug ("(III) get_cache_entry: %s\n", path);
  CacheEntry *entry = g_hash_table_lookup (backend->file_cache, path);
  if (!entry) {
    add_cache_entries_for_filename (backend, path);
    entry = g_hash_table_lookup (backend->file_cache, path);
  }
  g_debug ("(III) get_cache_entry done: %p\n", entry);
  return entry;
}


static gboolean
remove_cache_entry_by_prefix (gpointer key,
                              gpointer value,
                              gpointer user_data)
{
  const char *path = key;
  const char *prefix = user_data;

  return g_str_has_prefix (path, prefix);
}


static void
remove_cache_entry (GVfsBackendMtp *backend,
                    const char *path)
{
  g_debug ("(III) remove_cache_entry: %s\n", path);
  g_hash_table_foreach_remove (backend->file_cache,
                               remove_cache_entry_by_prefix,
                               (gpointer) path);
  g_debug ("(III) remove_cache_entry done\n");
}


static void
remove_cache_entry_by_id (GVfsBackendMtp *backend,
                          uint32_t id)
{
  GHashTableIter iter;
  gpointer key, value;
  g_debug ("(III) remove_cache_entry_by_id: %X\n", id);

  g_hash_table_iter_init (&iter, backend->file_cache);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    const char *path = key;
    const CacheEntry *entry = value;

    if (entry->id == id || (entry->id == -1 && entry->storage == id)) {
      g_hash_table_foreach (backend->monitors,
                            emit_delete_event,
                            (char *)path);
      g_hash_table_iter_remove (&iter);

      /* We do not break here because we can end up with multiple entries
         that have same storage/object ID, and we should find and remove
         them all instead of just the first one (Bug #733886) */
    }
  }

  g_debug ("(III) remove_cache_entry_by_id done\n");
}


/************************************************
 * Initialization
 ************************************************/

G_DEFINE_TYPE (GVfsBackendMtp, g_vfs_backend_mtp, G_VFS_TYPE_BACKEND)

static void
g_vfs_backend_mtp_init (GVfsBackendMtp *backend)
{
  g_debug ("(I) g_vfs_backend_mtp_init\n");
  GMountSpec *mount_spec;
  const char *debug;

  g_mutex_init (&backend->mutex);
  g_vfs_backend_set_display_name (G_VFS_BACKEND (backend), "mtp");
  g_vfs_backend_set_icon_name (G_VFS_BACKEND (backend), "multimedia-player");
  g_vfs_backend_handle_readonly_lockdown (G_VFS_BACKEND (backend));

  mount_spec = g_mount_spec_new ("mtp");
  g_vfs_backend_set_mount_spec (G_VFS_BACKEND (backend), mount_spec);
  g_mount_spec_unref (mount_spec);

  backend->monitors = g_hash_table_new (NULL, NULL);

  backend->event_pool = g_thread_pool_new ((GFunc) handle_event,
                                           backend, 1, FALSE, NULL);

  debug = g_getenv ("GVFS_MTP_DEBUG");
  if (debug != NULL) {
    int level;

    if (g_ascii_strcasecmp ("ptp", debug) == 0)
      level = LIBMTP_DEBUG_PTP;
    else if (g_ascii_strcasecmp ("usb", debug) == 0)
      level = LIBMTP_DEBUG_USB | LIBMTP_DEBUG_PTP;
    else if (g_ascii_strcasecmp ("data", debug) == 0)
      level = LIBMTP_DEBUG_DATA | LIBMTP_DEBUG_USB | LIBMTP_DEBUG_PTP;
    else /* "all" */
      level = LIBMTP_DEBUG_ALL;

    LIBMTP_Set_Debug (level);
  }

  g_debug ("(I) g_vfs_backend_mtp_init done.\n");
}

static void
remove_monitor_weak_ref (gpointer monitor,
                         gpointer unused,
                         gpointer monitors)
{
  g_object_weak_unref (G_OBJECT(monitor), (GWeakNotify)g_hash_table_remove, monitors);
}

static void
g_vfs_backend_mtp_finalize (GObject *object)
{
  GVfsBackendMtp *backend;

  g_debug ("(I) g_vfs_backend_mtp_finalize\n");

  backend = G_VFS_BACKEND_MTP (object);

  g_thread_pool_free (backend->event_pool, TRUE, TRUE);

  g_hash_table_foreach (backend->monitors, remove_monitor_weak_ref, backend->monitors);
  g_hash_table_unref (backend->monitors);

  /* Leak the mutex if the backend is force unmounted to avoid crash caused by
   * abort(), when trying to clear already locked mutex. */
  if (!backend->force_unmounted)
    g_mutex_clear (&backend->mutex);

  (*G_OBJECT_CLASS (g_vfs_backend_mtp_parent_class)->finalize) (object);

  g_debug ("(I) g_vfs_backend_mtp_finalize done.\n");
}


/************************************************
 * Monitors
 ************************************************/

/**
 * do_create_dir_monitor:
 *
 * Called with backend mutex lock held.
 */
static void
do_create_dir_monitor (GVfsBackend *backend,
                       GVfsJobCreateMonitor *job,
                       const char *filename,
                       GFileMonitorFlags flags)
{
  GVfsBackendMtp *mtp_backend = G_VFS_BACKEND_MTP (backend);

  g_debug ("(I) create_dir_monitor (%s)\n", filename);

  GVfsMonitor *vfs_monitor = g_vfs_monitor_new (backend);

  g_object_set_data_full (G_OBJECT (vfs_monitor), "gvfsbackendmtp:path",
                          g_strdup (filename), g_free);

  g_vfs_job_create_monitor_set_monitor (job, vfs_monitor);
  g_hash_table_add (mtp_backend->monitors, vfs_monitor);
  g_object_weak_ref (G_OBJECT (vfs_monitor), (GWeakNotify)g_hash_table_remove, mtp_backend->monitors);
  g_object_unref (vfs_monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_debug ("(I) create_dir_monitor done.\n");
}


/**
 * do_create_file_monitor:
 *
 * Called with backend mutex lock held.
 */
static void
do_create_file_monitor (GVfsBackend *backend,
                        GVfsJobCreateMonitor *job,
                        const char *filename,
                        GFileMonitorFlags flags)
{
  GVfsBackendMtp *mtp_backend = G_VFS_BACKEND_MTP (backend);

  g_debug ("(I) create_file_monitor (%s)\n", filename);

  GVfsMonitor *vfs_monitor = g_vfs_monitor_new (backend);

  g_object_set_data_full (G_OBJECT (vfs_monitor), "gvfsbackendmtp:path",
                          g_strdup (filename), g_free);

  g_vfs_job_create_monitor_set_monitor (job, vfs_monitor);
  g_hash_table_add (mtp_backend->monitors, vfs_monitor);
  g_object_weak_ref (G_OBJECT (vfs_monitor), (GWeakNotify)g_hash_table_remove, mtp_backend->monitors);
  g_object_unref (vfs_monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_debug ("(I) create_file_monitor done.\n");
}


static void
emit_event_internal (GVfsMonitor *monitor,
                     const char *path,
                     GFileMonitorEvent event)
{
  g_debug ("(III) emit_event_internal (%s, %d)\n", path, event);

  char *dir = g_path_get_dirname (path);
  const char *monitored_path = g_object_get_data (G_OBJECT (monitor), "gvfsbackendmtp:path");
  if (g_strcmp0 (dir, monitored_path) == 0) {
    g_debug ("(III) emit_event_internal: Event %d on directory %s for %s\n", event, dir, path);
    g_vfs_monitor_emit_event (monitor, event, path, NULL);
  } else if (g_strcmp0 (path, monitored_path) == 0) {
    g_debug ("(III) emit_event_internal: Event %d on file %s\n", event, path);
    g_vfs_monitor_emit_event (monitor, event, path, NULL);
  }
  g_free (dir);

  g_debug ("(III) emit_event_internal done.\n");
}


static void
emit_create_event (gpointer key,
                   gpointer value,
                   gpointer user_data)
{
  g_debug ("(II) emit_create_event.\n");
  emit_event_internal (key, user_data, G_FILE_MONITOR_EVENT_CREATED);
}


static void
emit_delete_event (gpointer key,
                   gpointer value,
                   gpointer user_data)
{
  g_debug ("(II) emit_delete_event.\n");
  emit_event_internal (key, user_data, G_FILE_MONITOR_EVENT_DELETED);
}


/************************************************
 * Errors
 ************************************************/

static void
fail_job (GVfsJob *job, LIBMTP_mtpdevice_t *device)
{
  const char *text;
  LIBMTP_error_t *error = LIBMTP_Get_Errorstack (device);

  if (error) {
    text = g_strrstr (error->error_text, ":") + 1;
  } else {
    text = _("Unknown error.");
  }
  g_vfs_job_failed (job, G_IO_ERROR,
                    g_vfs_job_is_cancelled (job) ?
                      G_IO_ERROR_CANCELLED :
                      G_IO_ERROR_FAILED,
                    _("libmtp error: %s"),
                    text);

  LIBMTP_Clear_Errorstack (device);
}


/************************************************
 * Mounts
 ************************************************/

static LIBMTP_mtpdevice_t *
get_device (GVfsBackend *backend, uint32_t bus_num, uint32_t dev_num , GVfsJob *job);


static void
on_uevent (GUdevClient *client, gchar *action, GUdevDevice *device, gpointer user_data)
{
  const char *dev_path = g_udev_device_get_device_file (device);
  g_debug ("(I) on_uevent (action %s, device %s)\n", action, dev_path);

  if (dev_path == NULL) {
    return;
  }

  GVfsBackendMtp *op_backend = G_VFS_BACKEND_MTP (user_data);

  if (g_strcmp0 (op_backend->dev_path, dev_path) == 0 &&
      g_str_equal (action, "remove")) {
    g_debug ("(I) on_uevent: Quiting after remove event on device %s\n", dev_path);

    /* Emit delete events to tell clients files are gone. */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init (&iter, op_backend->file_cache);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
      const char *path = key;

      g_hash_table_foreach (op_backend->monitors,
                            emit_delete_event,
                           (char *)path);
    }

    /* Finally, emit delete event to tell the clients the device root file is gone. */
    g_hash_table_foreach (op_backend->monitors,
                          emit_delete_event,
                          (char *)"/");

    op_backend->force_unmounted = TRUE;
    g_atomic_int_set (&op_backend->unmount_started, TRUE);
    g_vfs_backend_force_unmount ((GVfsBackend*)op_backend);

    g_signal_handlers_disconnect_by_func (op_backend->gudev_client, on_uevent, op_backend);
  }

  g_debug ("(I) on_uevent done.\n");
}

static void
check_event_cb(int ret, LIBMTP_event_t event, uint32_t param1, void *user_data)
{
  GVfsBackendMtp *backend = user_data;

  g_debug ("(II) check_event_cb: %d, %d, %d\n", ret, event, param1);
  backend->event_completed = TRUE;

  if (ret != LIBMTP_HANDLER_RETURN_OK ||
      g_atomic_int_get (&backend->unmount_started)) {
    return;
  }

  EventData *data = g_new(EventData, 1);
  data->event = event;
  data->param1 = param1;
  gboolean tret = g_thread_pool_push (backend->event_pool, data, NULL);
  g_debug ("(II) check_event_cb push work to pool: %d\n", tret);
}

static gpointer
check_event (gpointer user_data)
{
  GVfsBackendMtp *backend = user_data;

  while (!g_atomic_int_get (&backend->unmount_started)) {
    int ret;
    LIBMTP_mtpdevice_t *device = backend->device;
    if (backend->event_completed) {
      g_debug ("(I) check_event: Read event needs to be issued.\n");
      ret = LIBMTP_Read_Event_Async (device, check_event_cb, backend);
      if (ret != 0) {
        g_debug ("(I) check_event: Read_Event_Async failed: %d\n", ret);
      }
      backend->event_completed = FALSE;
    }
    /*
     * Return from polling periodically to check for unmount.
     */
    struct timeval tv = EVENT_POLL_PERIOD;
    g_debug ("(I) check_event: Polling for events.\n");
    ret = LIBMTP_Handle_Events_Timeout_Completed (&tv, &(backend->event_completed));
    if (ret != 0) {
      g_debug ("(I) check_event: polling returned error: %d\n", ret);
    }
  }
  return NULL;
}

void
handle_event (EventData *ed, GVfsBackendMtp *backend)
{
  LIBMTP_event_t event = ed->event;
  uint32_t param1 = ed->param1;
  g_free (ed);

  g_mutex_lock (&backend->mutex);
  if (!g_atomic_int_get (&backend->unmount_started)) {
    switch (event) {
    case LIBMTP_EVENT_STORE_ADDED:
      {
        LIBMTP_mtpdevice_t *device = backend->device;
        LIBMTP_devicestorage_t *storage;

        int ret = LIBMTP_Get_Storage (device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
        if (ret != 0) {
          LIBMTP_Dump_Errorstack (device);
          LIBMTP_Clear_Errorstack (device);
        } else {
          for (storage = device->storage; storage != 0; storage = storage->next) {
            if (storage->id == param1) {
              char *storage_name = create_storage_name (storage);
              char *path = g_build_filename ("/", storage_name, NULL);
              g_free (storage_name);

              add_cache_entry (G_VFS_BACKEND_MTP (backend),
                              path,
                              storage->id,
                              -1);
              g_hash_table_foreach (backend->monitors, emit_create_event, path);
            }
          }
        }
        break;
      }
    case LIBMTP_EVENT_OBJECT_REMOVED:
        remove_cache_entry_by_id (G_VFS_BACKEND_MTP (backend), param1);
        break;
    case LIBMTP_EVENT_STORE_REMOVED:
      {
        /* Clear the cache entries and emit delete event; first for all
           entries under the storage in question... */
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init (&iter, backend->file_cache);
        while (g_hash_table_iter_next (&iter, &key, &value)) {
          const char *path = key;
          const CacheEntry *entry = value;

          if (entry->storage == param1) {
            g_hash_table_foreach (backend->monitors,
                                  emit_delete_event,
                                  (char *)path);
            g_hash_table_iter_remove (&iter);
          }
        }

        /* ... and then for the storage itself */
        remove_cache_entry_by_id (G_VFS_BACKEND_MTP (backend), param1);
        break;
      }
    case LIBMTP_EVENT_OBJECT_ADDED:
      {
        LIBMTP_file_t *object = LIBMTP_Get_Filemetadata(backend->device, param1);
        if (object) {
          /* Obtain parent's path by searching cache by object's parent
             ID; if the latter is zero, the object is located in storage's
             root, otherwise, it is located in one of subfolders */
          const char *parent_path = NULL;
          GHashTableIter iter;
          gpointer key, value;

          g_hash_table_iter_init (&iter, backend->file_cache);
          while (g_hash_table_iter_next (&iter, &key, &value)) {
            const char *path = key;
            const CacheEntry *entry = value;

            if (object->parent_id != 0) {
              if (object->parent_id == entry->id && object->storage_id == entry->storage) {
                parent_path = path;
                break;
              }
            } else {
              if (entry->id == -1 && object->storage_id == entry->storage) {
                parent_path = path;
                break;
              }
            }
          }

          if (parent_path) {
            /* Create path, add cache entry and emit create event(s) */
            char *path = g_build_filename (parent_path, object->filename, NULL);

            add_cache_entry (G_VFS_BACKEND_MTP (backend),
                             path,
                             object->storage_id,
                             object->item_id);
            g_hash_table_foreach (backend->monitors, emit_create_event, path);
          }

          LIBMTP_destroy_file_t (object);
        }
        break;
      }
    default:
      break;
    }
  }
  g_mutex_unlock (&backend->mutex);
}

static gboolean
mtp_heartbeat (GVfsBackendMtp *backend)
{
  if (g_mutex_trylock (&backend->mutex)) {
    char *name = LIBMTP_Get_Friendlyname (backend->device);
    if (name) {
      free (name);
    }
    g_mutex_unlock (&backend->mutex);
  }
  return TRUE;
}

static char *
get_dev_path_and_device_from_host (GVfsJob *job,
                                   GUdevClient *gudev_client,
                                   const char *host,
                                   uint32_t *bus_num,
                                   uint32_t *dev_num,
                                   GUdevDevice **device)
{
  GList *devices, *l;
  g_debug ("(II) get_dev_path_from_host: %s\n", host);

  *device = NULL;

  /* find corresponding GUdevDevice */
  devices = g_udev_client_query_by_subsystem (gudev_client, "usb");
  for (l = devices; l != NULL; l = l->next) {
    const char *id = g_udev_device_get_property (l->data, "ID_SERIAL");
    if (g_strcmp0 (id, host) == 0) {
      *device = g_object_ref (l->data);
      *bus_num = g_ascii_strtoull (g_udev_device_get_property (l->data, "BUSNUM"),
                                   NULL, 10);
      *dev_num = g_ascii_strtoull (g_udev_device_get_property (l->data, "DEVNUM"),
                                   NULL, 10);
      break;
    }
  }
  g_list_free_full (devices, g_object_unref);

  if (*device) {
    return g_strdup_printf ("/dev/bus/usb/%03u/%03u", *bus_num, *dev_num);
  }

  /* For compatibility, handle old style host specifications. */
  if (g_str_has_prefix (host, "[usb:")) {
    /* Split [usb:001,002] into: '[usb', '001', '002', '' */
    char **elements = g_strsplit_set (host, ":,]", -1);
    if (g_strv_length (elements) == 4 && elements[3][0] == '\0') {
      *bus_num = g_ascii_strtoull (elements[1], NULL, 10);
      *dev_num = g_ascii_strtoull (elements[2], NULL, 10);

      /* These values are non-zero, so zero means a parsing error. */
      if (*bus_num != 0 && *dev_num != 0) {
        char *dev_path = g_strdup_printf ("/dev/bus/usb/%s/%s",
                                          elements[1], elements[2]);
        *device = g_udev_client_query_by_device_file (gudev_client, dev_path);
        if (*device) {
          g_strfreev (elements);
          return dev_path;
        }
        g_free (dev_path);
      }
    }
    g_strfreev (elements);
  }

  g_vfs_job_failed_literal (G_VFS_JOB (job),
                            G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            _("Couldn’t find matching udev device."));
  return NULL;
}

static void
do_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendMtp *op_backend = G_VFS_BACKEND_MTP (backend);
  GUdevDevice *device;
  uint32_t bus_num, dev_num;

  g_debug ("(I) do_mount\n");

  const char *host = g_mount_spec_get (mount_spec, "host");
  g_debug ("(I) do_mount: host=%s\n", host);
  if (host == NULL) {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_FAILED, _("No device specified"));
    return;
  }

  const char *subsystems[] = {"usb", NULL};
  op_backend->gudev_client = g_udev_client_new (subsystems);
  if (op_backend->gudev_client == NULL) {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_FAILED, _("Cannot create gudev client"));
    return;
  }

  char *dev_path = get_dev_path_and_device_from_host (G_VFS_JOB (job),
                                                      op_backend->gudev_client,
                                                      host,
                                                      &bus_num,
                                                      &dev_num,
                                                      &device);
  if (dev_path == NULL) {
    g_object_unref (op_backend->gudev_client);
    /* get_dev_path_from_host() sets job state. */
    return;
  }
  op_backend->dev_path = dev_path;

  op_backend->volume_name = g_vfs_get_volume_name (device, "ID_MTP");
  op_backend->volume_icon = g_vfs_get_volume_icon (device);
  op_backend->volume_symbolic_icon = g_vfs_get_volume_symbolic_icon (device);
  g_object_unref (device);

  LIBMTP_Init ();

  get_device (backend, bus_num, dev_num, G_VFS_JOB (job));
  if (!G_VFS_JOB (job)->failed) {
    g_signal_connect (op_backend->gudev_client, "uevent", G_CALLBACK (on_uevent), op_backend);

    op_backend->file_cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

    GMountSpec *mtp_mount_spec = g_mount_spec_new ("mtp");
    g_mount_spec_set (mtp_mount_spec, "host", host);
    g_vfs_backend_set_mount_spec (backend, mtp_mount_spec);
    g_mount_spec_unref (mtp_mount_spec);

    g_vfs_job_succeeded (G_VFS_JOB (job));

    op_backend->hb_id =
      g_timeout_add_seconds (900, (GSourceFunc)mtp_heartbeat, op_backend);

    op_backend->event_completed = TRUE;
    op_backend->event_thread = g_thread_new ("events", check_event, backend);
  }
  g_debug ("(I) do_mount done.\n");
}


static void
do_unmount (GVfsBackend *backend, GVfsJobUnmount *job,
            GMountUnmountFlags flags,
            GMountSource *mount_source)
{
  GVfsBackendMtp *op_backend;

  g_debug ("(I) do_umount\n");

  op_backend = G_VFS_BACKEND_MTP (backend);

  g_mutex_lock (&op_backend->mutex);

  g_atomic_int_set (&op_backend->unmount_started, TRUE);

#if HAVE_LIBUSB
  libusb_interrupt_event_handler (NULL);
#endif

  /* Thread will terminate after flag is set. */
  g_thread_join (op_backend->event_thread);

  /* It's no longer safe to handle events. */
  g_thread_pool_set_max_threads (op_backend->event_pool, 0, NULL);

  /* Emit delete events to tell clients files are gone. */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, op_backend->file_cache);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    const char *path = key;

    g_hash_table_foreach (op_backend->monitors,
                          emit_delete_event,
                         (char *)path);
  }
  g_hash_table_unref (op_backend->file_cache);

  g_source_remove (op_backend->hb_id);

  g_signal_handlers_disconnect_by_func (op_backend->gudev_client, on_uevent, op_backend);

  g_object_unref (op_backend->gudev_client);
  g_clear_pointer (&op_backend->dev_path, g_free);
  g_clear_pointer (&op_backend->volume_name, g_free);
  g_clear_pointer (&op_backend->volume_icon, g_free);
  g_clear_pointer (&op_backend->volume_symbolic_icon, g_free);
  LIBMTP_Release_Device (op_backend->device);

  g_mutex_unlock (&op_backend->mutex);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_debug ("(I) do_umount done.\n");
}


/************************************************
 * 	  Queries
 * 
 */


/**
 * get_device:
 *
 * Called with backend mutex lock held.
 */
LIBMTP_mtpdevice_t *
get_device (GVfsBackend *backend, uint32_t bus_num, uint32_t dev_num,
            GVfsJob *job) {
  g_debug ("(II) get_device: %u,%u\n", bus_num, dev_num);

  LIBMTP_mtpdevice_t *device = NULL;

  if (G_VFS_BACKEND_MTP (backend)->device != NULL) {
    g_debug ("(II) get_device: Returning cached device %p\n", device);
    return G_VFS_BACKEND_MTP (backend)->device;
  }

  LIBMTP_raw_device_t * rawdevices;
  int numrawdevices;
  LIBMTP_error_number_t err;

  err = LIBMTP_Detect_Raw_Devices (&rawdevices, &numrawdevices);
  switch (err) {
  case LIBMTP_ERROR_NONE:
    break;
  case LIBMTP_ERROR_NO_DEVICE_ATTACHED:
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("No MTP devices found"));
    goto exit;
  case LIBMTP_ERROR_CONNECTING:
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
                              _("Unable to connect to MTP device"));
    goto exit;
  case LIBMTP_ERROR_MEMORY_ALLOCATION:
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_FILE_ERROR, G_FILE_ERROR_NOMEM,
                              _("Unable to allocate memory while detecting MTP devices"));
    goto exit;
  case LIBMTP_ERROR_GENERAL:
  default:
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_FAILED,
                              _("Generic libmtp error"));
    goto exit;
  }

  /* Iterate over connected MTP devices */
  int i;
  for (i = 0; i < numrawdevices; i++) {
    if (rawdevices[i].bus_location == bus_num &&
        rawdevices[i].devnum == dev_num) {
      device = LIBMTP_Open_Raw_Device_Uncached (&rawdevices[i]);
      if (device == NULL) {
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR, G_IO_ERROR_FAILED,
                          _("Unable to open MTP device “%03u,%03u”"),
                          bus_num, dev_num);
        goto exit;
      }

      g_debug ("(II) get_device: Storing device %03u,%03u\n", bus_num, dev_num);
      G_VFS_BACKEND_MTP (backend)->device = device;

      LIBMTP_Dump_Errorstack (device);
      LIBMTP_Clear_Errorstack (device);
      break;
    }
  }

  if (device == NULL) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      _("Device not found"));
    goto exit;
  }

  /* Check supported methods/extensions. */
  LIBMTP_device_extension_t *extension;
  for (extension = device->extensions; extension != NULL; extension = extension->next) {
    if (g_strcmp0 ("android.com", extension->name) == 0) {
      G_VFS_BACKEND_MTP (backend)->android_extension = TRUE;
      break;
    }
  }

  G_VFS_BACKEND_MTP (backend)->get_partial_object_capability
    = LIBMTP_Check_Capability (device, LIBMTP_DEVICECAP_GetPartialObject);
#if HAVE_LIBMTP_1_1_15
  G_VFS_BACKEND_MTP (backend)->move_object_capability
    = LIBMTP_Check_Capability (device, LIBMTP_DEVICECAP_MoveObject);
  G_VFS_BACKEND_MTP (backend)->copy_object_capability
    = LIBMTP_Check_Capability (device, LIBMTP_DEVICECAP_CopyObject);
#endif

 exit:
  g_debug ("(II) get_device done.\n");
  return device;
}


/**
 * get_device_info:
 *
 * Called with backend mutex lock held.
 */
static void
get_device_info (GVfsBackendMtp *backend, GFileInfo *info)
{
  LIBMTP_mtpdevice_t *device = backend->device;
  const char *name;

  name = g_mount_spec_get (g_vfs_backend_get_mount_spec (G_VFS_BACKEND (backend)), "host");

  g_debug ("(II) get_device_info: %s\n", name);

  g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
  g_file_info_set_name (info, name);

  char *friendlyname = LIBMTP_Get_Friendlyname (device);
  g_file_info_set_display_name (info, friendlyname == NULL ?
                                      backend->volume_name : friendlyname);
  free (friendlyname);

  g_file_info_set_content_type (info, "inode/directory");
  g_file_info_set_size (info, 0);

  GIcon *icon = g_themed_icon_new (backend->volume_icon);
  g_file_info_set_icon (info, icon);
  g_object_unref (icon);

  icon = g_themed_icon_new (backend->volume_symbolic_icon);
  g_file_info_set_symbolic_icon (info, icon);
  g_object_unref (icon);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);

  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "mtpfs");
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, FALSE);

  int ret = LIBMTP_Get_Storage (device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
  if (ret != 0) {
    LIBMTP_Dump_Errorstack (device);
    LIBMTP_Clear_Errorstack (device);
    g_debug ("(II) get_device_info done with no stores.\n");
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

  g_debug ("(II) get_device_info done.\n");
}


/**
 * get_storage_info:
 *
 * Called with backend mutex lock held.
 */
static void
get_storage_info (LIBMTP_devicestorage_t *storage, GFileInfo *info) {

  g_debug ("(II) get_storage_info: %X\n", storage->id);

  char *storage_name = create_storage_name(storage);
  g_file_info_set_name (info, storage_name);
  g_file_info_set_display_name (info, storage_name);
  g_free(storage_name);

  g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
  g_file_info_set_content_type (info, "inode/directory");
  g_file_info_set_size (info, 0);

  GIcon *icon, *symbolic_icon;
  switch (storage->StorageType) {
  case PTP_ST_FixedROM:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, TRUE);
    icon = g_themed_icon_new_with_default_fallbacks ("drive-harddisk");
    symbolic_icon = g_themed_icon_new_with_default_fallbacks ("drive-harddisk-symbolic");
    break;
  case PTP_ST_RemovableROM:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, TRUE);
    icon = g_themed_icon_new_with_default_fallbacks ("media-flash-sd");
    symbolic_icon = g_themed_icon_new_with_default_fallbacks ("media-flash-sd-symbolic");
    break;
  case PTP_ST_RemovableRAM:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, FALSE);
    icon = g_themed_icon_new_with_default_fallbacks ("media-flash-sd");
    symbolic_icon = g_themed_icon_new_with_default_fallbacks ("media-flash-sd-symbolic");
    break;
  case PTP_ST_FixedRAM:
  default:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, FALSE);
    icon = g_themed_icon_new_with_default_fallbacks ("drive-harddisk");
    symbolic_icon = g_themed_icon_new_with_default_fallbacks ("drive-harddisk-symbolic");
    break;
  }
  g_file_info_set_icon (info, icon);
  g_file_info_set_symbolic_icon (info, symbolic_icon);
  g_object_unref (icon);
  g_object_unref (symbolic_icon);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);

  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, storage->FreeSpaceInBytes);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, storage->MaxCapacity);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "mtpfs");
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_NEVER);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, FALSE);

  g_debug ("(II) get_storage_info done.\n");
}


/**
 * get_file_info:
 *
 * Called with backend mutex lock held.
 */
static void
get_file_info (GVfsBackend *backend,
               LIBMTP_mtpdevice_t *device,
               GFileInfo *info,
               LIBMTP_file_t *file) {
  GIcon *icon = NULL;
  GIcon *symbolic_icon = NULL;
  char *content_type = NULL;
  gboolean uncertain_content_type = FALSE;
  char *mount_id = NULL;
  char *file_id = NULL;

  g_debug ("(II) get_file_info: %X\n", file->item_id);

  g_file_info_set_name (info, file->filename);
  g_file_info_set_display_name (info, file->filename);

  mount_id = g_mount_spec_to_string (g_vfs_backend_get_mount_spec (G_VFS_BACKEND (backend)));
  file_id = g_strdup_printf ("%s:%d", mount_id, file->item_id);
  g_free (mount_id);

  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ID_FILE, file_id);
  g_free (file_id);

  switch (file->filetype) {
  case LIBMTP_FILETYPE_FOLDER:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
    g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
    content_type = g_strdup ("inode/directory");
    break;
  default:
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, FALSE);
    g_file_info_set_file_type (info, G_FILE_TYPE_REGULAR);
    content_type = g_content_type_guess (file->filename, NULL, 0, &uncertain_content_type);
    break;
  }

  if (!uncertain_content_type)
    g_file_info_set_content_type (info, content_type);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE,
                                    content_type);

  icon = g_content_type_get_icon (content_type);
  symbolic_icon = g_content_type_get_symbolic_icon (content_type);

  if (LIBMTP_FILETYPE_IS_IMAGE (file->filetype) ||
      LIBMTP_FILETYPE_IS_VIDEO (file->filetype) ||
      LIBMTP_FILETYPE_IS_AUDIOVIDEO (file->filetype)) {

    GIcon *preview;
    char *icon_id;
    GMountSpec *mount_spec;

    mount_spec = g_vfs_backend_get_mount_spec (backend);
    icon_id = g_strdup_printf ("%X", file->item_id);
    preview = g_vfs_icon_new (mount_spec,
                              icon_id);
    g_file_info_set_attribute_object (info,
                                      G_FILE_ATTRIBUTE_PREVIEW_ICON,
                                      G_OBJECT (preview));
    g_object_unref (preview);
    g_free (icon_id);
  }

  g_file_info_set_size (info, file->filesize);

  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, file->modificationdate);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, 0);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, TRUE);


  if (icon != NULL) {
    g_file_info_set_icon (info, icon);
    g_object_unref (icon);
  }
  if (symbolic_icon != NULL) {
    g_file_info_set_symbolic_icon (info, symbolic_icon);
    g_object_unref (symbolic_icon);
  }
  g_free (content_type);

  g_debug ("(II) get_file_info done.\n");
}


static void
do_enumerate (GVfsBackend *backend,
              GVfsJobEnumerate *job,
              const char *filename,
              GFileAttributeMatcher *attribute_matcher,
              GFileQueryInfoFlags flags)
{
  GVfsBackendMtp *op_backend = G_VFS_BACKEND_MTP (backend);
  GFileInfo *info;

  gchar **elements = g_strsplit_set (filename, "/", -1);
  unsigned int ne = g_strv_length (elements);

  g_debug ("(I) do_enumerate (filename = %s, n_elements = %d)\n", filename, ne);

  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  LIBMTP_mtpdevice_t *device;
  device = op_backend->device;

  if (ne == 2 && elements[1][0] == '\0') {
    LIBMTP_devicestorage_t *storage;

    int ret = LIBMTP_Get_Storage (device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    if (ret != 0) {
      LIBMTP_Dump_Errorstack (device);
      LIBMTP_Clear_Errorstack (device);
      g_vfs_job_succeeded (G_VFS_JOB (job));
      goto success;
    }
    for (storage = device->storage; storage != 0; storage = storage->next) {
      info = g_file_info_new ();
      get_storage_info (storage, info);
      g_vfs_job_enumerate_add_info (job, info);
      g_object_unref (info);

      char *storage_name = create_storage_name (storage);
      add_cache_entry (G_VFS_BACKEND_MTP (backend),
                       g_build_filename (filename, storage_name, NULL),
                       storage->id,
                       -1);

      g_free (storage_name);
    }
    g_vfs_job_succeeded (G_VFS_JOB (job));
  } else {
    CacheEntry *entry = get_cache_entry (G_VFS_BACKEND_MTP (backend),
                                         filename);
    if (entry == NULL) {
      LIBMTP_Dump_Errorstack (device);
      LIBMTP_Clear_Errorstack (device);
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                _("File not found"));
      goto exit;
    }

    /* Invalidate existing cache entries in case any are stale. */
    char *remove_prefix = g_strdup_printf("%s/", filename);
    remove_cache_entry (G_VFS_BACKEND_MTP (backend), remove_prefix);
    g_free (remove_prefix);

    LIBMTP_Clear_Errorstack (device);

#if HAVE_LIBMTP_1_1_21
    g_autofree uint32_t *handlers = NULL;
    int count = LIBMTP_Get_Children (device, entry->storage, entry->id, &handlers);

    if (count < 0) {
      fail_job (G_VFS_JOB (job), device);
      goto exit;
    }

    g_vfs_job_succeeded (G_VFS_JOB (job));

    for (int i = 0; i < count; i++) {
      LIBMTP_file_t *file;

      if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
        goto exit;

      // Get metadata for one file, if it fails, try next file
      file = LIBMTP_Get_Filemetadata (device, handlers[i]);
      if (file == NULL) {
        continue;
      }
      info = g_file_info_new ();
      get_file_info (backend, device, info, file);
      g_vfs_job_enumerate_add_info (job, info);
      g_object_unref (info);

      add_cache_entry (G_VFS_BACKEND_MTP (backend),
                       g_build_filename (filename, file->filename, NULL),
                       file->storage_id,
                       file->item_id);

      LIBMTP_destroy_file_t (file);
    }

#else
    LIBMTP_file_t *files;

    files = LIBMTP_Get_Files_And_Folders (device, entry->storage, entry->id);
    if (files == NULL && LIBMTP_Get_Errorstack (device) != NULL) {
      fail_job (G_VFS_JOB (job), device);
      goto exit;
    }

    g_vfs_job_succeeded (G_VFS_JOB (job));

    while (files != NULL) {
      LIBMTP_file_t *file = files;
      files = files->next;

      info = g_file_info_new ();
      get_file_info (backend, device, info, file);
      g_vfs_job_enumerate_add_info (job, info);
      g_object_unref (info);

      add_cache_entry (G_VFS_BACKEND_MTP (backend),
                       g_build_filename (filename, file->filename, NULL),
                       file->storage_id,
                       file->item_id);

      LIBMTP_destroy_file_t (file);
    }
#endif
  }

 success:
  g_vfs_job_enumerate_done (job);

 exit:
  g_strfreev (elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);
  g_debug ("(I) do_enumerate done.\n");
}


static void
do_query_info (GVfsBackend *backend,
               GVfsJobQueryInfo *job,
               const char *filename,
               GFileQueryInfoFlags flags,
               GFileInfo *info,
               GFileAttributeMatcher *matcher)
{
  g_debug ("(I) do_query_info (filename = %s)\n", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  gchar **elements = g_strsplit_set (filename, "/", -1);
  unsigned int ne = g_strv_length (elements);

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  if (ne == 2 && elements[1][0] == '\0') {
    get_device_info (G_VFS_BACKEND_MTP (backend), info);
  } else if (ne < 3) {
    CacheEntry *entry = get_cache_entry (G_VFS_BACKEND_MTP (backend),
                                         filename);
    if (!entry) {
      LIBMTP_Dump_Errorstack (device);
      LIBMTP_Clear_Errorstack (device);
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                _("Directory doesn’t exist"));
      goto exit;
    }

    LIBMTP_devicestorage_t *storage;
    gboolean found = FALSE;

    int ret = LIBMTP_Get_Storage (device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    if (ret != 0) {
      fail_job (G_VFS_JOB (job), device);
      goto exit;
    }
    for (storage = device->storage; storage != 0; storage = storage->next) {
      if (storage->id == entry->storage) {
        g_debug ("(I) found storage %X\n", storage->id);
        found = TRUE;
        get_storage_info (storage, info);
        break;
      }
    }

    if (!found) {
      g_debug ("(W) storage %X not found?!\n", entry->storage);
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                _("Directory doesn’t exist"));
      goto exit;
    }
  } else {
    CacheEntry *entry = get_cache_entry (G_VFS_BACKEND_MTP (backend),
                                         filename);
    if (!entry) {
      LIBMTP_Dump_Errorstack (device);
      LIBMTP_Clear_Errorstack (device);
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                _("File not found"));
      goto exit;
    }


    LIBMTP_file_t *file = NULL;
    file = LIBMTP_Get_Filemetadata (device, entry->id);

    if (file != NULL) {
      get_file_info (backend, device, info, file);
      LIBMTP_destroy_file_t (file);
    } else {
      fail_job (G_VFS_JOB (job), device);
      goto exit;
    }
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_strfreev (elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);
  g_debug ("(I) do_query_info done.\n");
}


static void
do_query_fs_info (GVfsBackend *backend,
		  GVfsJobQueryFsInfo *job,
		  const char *filename,
		  GFileInfo *info,
		  GFileAttributeMatcher *attribute_matcher)
{
  g_debug ("(I) do_query_fs_info (filename = %s)\n", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  gchar **elements = g_strsplit_set (filename, "/", -1);
  unsigned int ne = g_strv_length (elements);

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  if (ne == 2 && elements[1][0] == '\0') {
    get_device_info (G_VFS_BACKEND_MTP (backend), info);
  } else {
    CacheEntry *entry = get_cache_entry (G_VFS_BACKEND_MTP (backend),
                                         filename);
    if (entry == NULL) {
      LIBMTP_Dump_Errorstack (device);
      LIBMTP_Clear_Errorstack (device);
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                _("File not found"));
      goto exit;
    }

    LIBMTP_devicestorage_t *storage;

    int ret = LIBMTP_Get_Storage (device, LIBMTP_STORAGE_SORTBY_NOTSORTED);
    if (ret != 0) {
      fail_job (G_VFS_JOB (job), device);
      goto exit;
    }
    for (storage = device->storage; storage != 0; storage = storage->next) {
      if (storage->id == entry->storage) {
        get_storage_info (storage, info);
      }
    }
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_strfreev (elements);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  g_debug ("(I) do_query_fs_info done.\n");
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


static int
mtp_progress (uint64_t const sent, uint64_t const total,
              MtpProgressData const * const data)
{
  if (data->progress_callback) {
    data->progress_callback (sent, total, data->progress_callback_data);
  }
  return g_vfs_job_is_cancelled (data->job);
}


/**
 * Validate whether a given combination of source and destination
 * are valid for copying/moving. If not valid, set the appropriate
 * error on the job.
 */
static gboolean
validate_source_and_dest (gboolean dest_exists,
                          gboolean dest_is_dir,
                          gboolean source_is_dir,
                          gboolean source_can_be_dir,
                          GFileCopyFlags flags,
                          GVfsJob *job)
{
  /* Test all the GIO defined failure conditions */
  if (dest_exists) {
    if (flags & G_FILE_COPY_OVERWRITE) {
      if (!source_is_dir && dest_is_dir) {
        g_vfs_job_failed_literal (G_VFS_JOB (job),
                                  G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                  _("Target is a directory"));
        return FALSE;
      } else if (source_is_dir && dest_is_dir) {
        g_vfs_job_failed_literal (G_VFS_JOB (job),
                                  G_IO_ERROR, G_IO_ERROR_WOULD_MERGE,
                                  _("Can’t merge directories"));
        return FALSE;
      } else if (source_is_dir && !dest_is_dir) {
        g_vfs_job_failed_literal (G_VFS_JOB (job),
                                  G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
                                  _("Can’t recursively copy directory"));
        return FALSE;
      }

      /* Source can overwrite dest as both are files */
      return TRUE;
    } else {
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_EXISTS,
                                _("Target file already exists"));
      return FALSE;
    }
  } else if (source_is_dir && !source_can_be_dir) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
                              _("Can’t recursively copy directory"));
    return FALSE;
  }

  /* Source is valid and dest doesn't exist */
  return TRUE;
}


static void
do_make_directory (GVfsBackend *backend,
                   GVfsJobMakeDirectory *job,
                   const char *filename)
{
  g_debug ("(I) do_make_directory (filename = %s)\n", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  char *dir_name = g_path_get_dirname (filename);
  char *base_name = g_path_get_basename (filename);

  gchar **elements = g_strsplit_set (filename, "/", -1);
  unsigned int ne = g_strv_length (elements);

  if (ne < 3) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                              _("Cannot make directory in this location"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  CacheEntry *entry = get_cache_entry (G_VFS_BACKEND_MTP (backend), filename);
  if (entry != NULL && entry->id != -1) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_EXISTS,
                              _("Target file already exists"));
    goto exit;
  }

  entry = get_cache_entry (G_VFS_BACKEND_MTP (backend), dir_name);
  if (!entry) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("Directory doesn’t exist"));
    goto exit;
  }

  int ret = LIBMTP_Create_Folder (device, base_name, entry->id, entry->storage);
  if (ret == 0) {
    fail_job (G_VFS_JOB (job), device);
    goto exit;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors, emit_create_event, (char *)filename);

 exit:
  g_strfreev (elements);
  g_free (dir_name);
  g_free (base_name);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  g_debug ("(I) do_make_directory done.\n");
}


static void
do_pull (GVfsBackend *backend,
         GVfsJobPull *job,
         const char *source,
         const char *local_path,
         GFileCopyFlags flags,
         gboolean remove_source,
         GFileProgressCallback progress_callback,
         gpointer progress_callback_data)
{
  g_debug ("(I) do_pull (filename = %s, local_path = %s)\n", source, local_path);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  GFile *local_file = NULL;
  GFileInfo *local_info = NULL;
  GFileInfo *info = NULL;
  guint64 mtime;

  if (remove_source && (flags & G_FILE_COPY_NO_FALLBACK_FOR_MOVE)) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      _("Operation not supported"));
    goto exit;
  }

  CacheEntry *entry = get_cache_entry (G_VFS_BACKEND_MTP (backend), source);
  if (entry == NULL) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("File doesn’t exist"));
    goto exit;
  } else if (entry->id == -1) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                              _("Not a regular file"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  LIBMTP_file_t *file = LIBMTP_Get_Filemetadata (device, entry->id);
  if (file == NULL) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("File doesn’t exist"));
    goto exit;
  }

  info = g_file_info_new ();
  get_file_info (backend, device, info, file);
  LIBMTP_destroy_file_t (file);

  local_file = g_file_new_for_path (local_path);

  gboolean source_is_dir =
    g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;

  GError *error = NULL;
  local_info =
    g_file_query_info (local_file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                       0, G_VFS_JOB (job)->cancellable, &error);
  gboolean dest_exists = local_info || error->code != G_IO_ERROR_NOT_FOUND;
  if (!local_info && error->code != G_IO_ERROR_NOT_FOUND) {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    g_error_free (error);
    goto exit;
  } else if (error != NULL) {
    g_error_free (error);
  }

  gboolean dest_is_dir = dest_exists &&
      g_file_info_get_file_type (local_info) == G_FILE_TYPE_DIRECTORY;

  gboolean valid_pull = validate_source_and_dest (dest_exists,
                                                  dest_is_dir,
                                                  source_is_dir,
                                                  FALSE, // source_can_be_dir
                                                  flags,
                                                  G_VFS_JOB (job));
  if (!valid_pull) {
    goto exit;
  } else if (dest_exists) {
    /* Source and Dest are files */
    g_debug ("(I) Removing destination.\n");
    GError *error = NULL;
    gboolean ret = g_file_delete (local_file,
                                  G_VFS_JOB (job)->cancellable,
                                  &error);
    if (!ret) {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto exit;
    }
  }

  MtpProgressData mtp_progress_data;
  mtp_progress_data.progress_callback = progress_callback;
  mtp_progress_data.progress_callback_data = progress_callback_data;
  mtp_progress_data.job = G_VFS_JOB (job);
  int ret = LIBMTP_Get_File_To_File (device,
                                     entry->id,
                                     local_path,
                                     (LIBMTP_progressfunc_t)mtp_progress,
                                     &mtp_progress_data);
  if (ret != 0) {
    fail_job (G_VFS_JOB (job), device);
    goto exit;
  }

  /* Ignore errors here. Failure to copy metadata is not a hard error */
  mtime = g_file_info_get_attribute_uint64 (info,
                                            G_FILE_ATTRIBUTE_TIME_MODIFIED);
  g_file_set_attribute_uint64 (local_file,
                               G_FILE_ATTRIBUTE_TIME_MODIFIED, mtime,
                               G_FILE_QUERY_INFO_NONE,
                               G_VFS_JOB (job)->cancellable, NULL);
  g_file_set_attribute_uint32 (local_file,
                               G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, 0,
                               G_FILE_QUERY_INFO_NONE,
                               G_VFS_JOB (job)->cancellable, NULL);

  /* Attempt to delete object if requested but don't fail it it fails. */
  if (remove_source) {
    g_debug ("(I) Removing source.\n");
    LIBMTP_Delete_Object (device, entry->id);
    g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                          emit_delete_event,
                          (char *)source);
    remove_cache_entry (G_VFS_BACKEND_MTP (backend),
                        source);
  }
  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_clear_object (&local_info);
  g_clear_object (&local_file);
  g_clear_object (&info);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  g_debug ("(I) do_pull done.\n");
}


static LIBMTP_filetype_t
get_filetype_from_info (GFileInfo *info) {
  const char *content_type = g_file_info_get_content_type (info);
  g_debug ("(II) get_filetype_from_info (info = %s)\n", content_type);

  LIBMTP_filetype_t ret;
  if (!content_type) {
    ret = LIBMTP_FILETYPE_UNKNOWN;
  } else if (strcmp (content_type, "audio/wav") == 0 ||
             strcmp (content_type, "audio/x-wav") == 0 ||
             strcmp (content_type, "audio/vnd.wave") == 0) {
    ret = LIBMTP_FILETYPE_WAV;
  } else if (strcmp (content_type, "audio/mpeg") == 0 ||
             strcmp (content_type, "audio/x-mp3") == 0 ||
             strcmp (content_type, "audio/x-mpeg") == 0 ||
             strcmp (content_type, "audio/mp3") == 0) {
    ret = LIBMTP_FILETYPE_MP3;
  } else if (strcmp (content_type, "audio/x-ms-wma") == 0 ||
             strcmp (content_type, "audio/wma") == 0) {
    ret = LIBMTP_FILETYPE_WMA;
  } else if (strcmp (content_type, "audio/ogg") == 0 ||
             strcmp (content_type, "audio/x-ogg") == 0) {
    ret = LIBMTP_FILETYPE_OGG;
  } else if (strcmp (content_type, "audio/audible") == 0 ||
             strcmp (content_type, "audio/x-pn-audibleaudio") == 0) {
    ret = LIBMTP_FILETYPE_AUDIBLE;
  } else if (strcmp (content_type, "video/mp4") == 0 ||
             strcmp (content_type, "video/x-m4v") == 0 ||
             strcmp (content_type, "video/mp4v-es") == 0) {
    ret = LIBMTP_FILETYPE_MP4;
  } else if (strcmp (content_type, "video/x-ms-wmv") == 0) {
    ret = LIBMTP_FILETYPE_WMV;
  } else if (strcmp (content_type, "video/x-msvideo") == 0 ||
             strcmp (content_type, "video/x-avi") == 0 ||
             strcmp (content_type, "video/avi") == 0 ||
             strcmp (content_type, "video/divx") == 0 ||
             strcmp (content_type, "video/msvideo") == 0 ||
             strcmp (content_type, "video/vnd.divx") == 0) {
    ret = LIBMTP_FILETYPE_AVI;
  } else if (strcmp (content_type, "video/mpeg") == 0 ||
             strcmp (content_type, "video/x-mpeg") == 0 ||
             strcmp (content_type, "video/x-mpeg2") == 0) {
    ret = LIBMTP_FILETYPE_MPEG;
  } else if (strcmp (content_type, "video/x-ms-asf") == 0 ||
             strcmp (content_type, "video/x-ms-wm") == 0 ||
             strcmp (content_type, "video/vnd.ms-asf") == 0) {
    ret = LIBMTP_FILETYPE_ASF;
  } else if (strcmp (content_type, "video/quicktime") == 0) {
    ret = LIBMTP_FILETYPE_QT;
  } else if (strcmp (content_type, "image/jpeg") == 0 ||
             strcmp (content_type, "image/pjpeg") == 0) {
    ret = LIBMTP_FILETYPE_JPEG;
  } else if (strcmp (content_type, "image/tiff") == 0) {
    ret = LIBMTP_FILETYPE_TIFF;
  } else if (strcmp (content_type, "image/bmp") == 0 ||
             strcmp (content_type, "image/x-bmp") == 0 ||
             strcmp (content_type, "image/x-MS-bmp") == 0) {
    ret = LIBMTP_FILETYPE_BMP;
  } else if (strcmp (content_type, "image/gif") == 0) {
    ret = LIBMTP_FILETYPE_GIF;
  } else if (strcmp (content_type, "image/x-pict") == 0) {
    ret = LIBMTP_FILETYPE_PICT;
  } else if (strcmp (content_type, "image/png") == 0) {
    ret = LIBMTP_FILETYPE_PNG;
  } else if (strcmp (content_type, "text/x-vcalendar") == 0) {
    ret = LIBMTP_FILETYPE_VCALENDAR1;
  } else if (strcmp (content_type, "text/calendar") == 0 ||
             strcmp (content_type, "application/ics") == 0) {
    ret = LIBMTP_FILETYPE_VCALENDAR2;
  } else if (strcmp (content_type, "text/x-vcard") == 0 ||
             strcmp (content_type, "text/directory") == 0) {
    ret = LIBMTP_FILETYPE_VCARD2;
  } else if (strcmp (content_type, "text/vcard") == 0) {
    ret = LIBMTP_FILETYPE_VCARD3;
  } else if (strcmp (content_type, "image/x-wmf") == 0 ||
             strcmp (content_type, "image/wmf") == 0 ||
             strcmp (content_type, "image/x-win-metafile") == 0 ||
             strcmp (content_type, "application/x-wmf") == 0 ||
             strcmp (content_type, "application/wmf") == 0 ||
             strcmp (content_type, "application/x-msmetafile") == 0) {
    ret = LIBMTP_FILETYPE_WINDOWSIMAGEFORMAT;
  } else if (strcmp (content_type, "application/x-ms-dos-executable") == 0) {
    ret = LIBMTP_FILETYPE_WINEXEC;
  } else if (strcmp (content_type, "text/plain") == 0) {
    ret = LIBMTP_FILETYPE_TEXT;
  } else if (strcmp (content_type, "text/html") == 0) {
    ret = LIBMTP_FILETYPE_HTML;
  } else if (strcmp (content_type, "audio/aac") == 0) {
    ret = LIBMTP_FILETYPE_AAC;
  } else if (strcmp (content_type, "audio/flac") == 0 ||
             strcmp (content_type, "audio/x-flac") == 0 ||
             strcmp (content_type, "audio/x-flac+ogg") == 0 ||
             strcmp (content_type, "audio/x-oggflac") == 0) {
    ret = LIBMTP_FILETYPE_FLAC;
  } else if (strcmp (content_type, "audio/mp2") == 0 ||
             strcmp (content_type, "audio/x-mp2") == 0) {
    ret = LIBMTP_FILETYPE_MP2;
  } else if (strcmp (content_type, "audio/mp4") == 0 ||
             strcmp (content_type, "audio/x-m4a") == 0) {
    ret = LIBMTP_FILETYPE_M4A;
  } else if (strcmp (content_type, "application/msword") == 0 ||
             strcmp (content_type, "application/vnd.ms-word") == 0 ||
             strcmp (content_type, "application/x-msword") == 0 ||
             strcmp (content_type, "zz-application/zz-winassoc-doc") == 0) {
    ret = LIBMTP_FILETYPE_DOC;
  } else if (strcmp (content_type, "text/xml") == 0 ||
             strcmp (content_type, "application/xml") == 0) {
    ret = LIBMTP_FILETYPE_XML;
  } else if (strcmp (content_type, "application/msexcel") == 0 ||
             strcmp (content_type, "application/vnd.ms-excel") == 0 ||
             strcmp (content_type, "application/x-msexcel") == 0 ||
             strcmp (content_type, "zz-application/zz-winassoc-xls") == 0) {
    ret = LIBMTP_FILETYPE_XLS;
  } else if (strcmp (content_type, "application/mspowerpoint") == 0 ||
             strcmp (content_type, "application/vnd.ms-powerpoint") == 0 ||
             strcmp (content_type, "application/x-mspowerpoint") == 0 ||
             strcmp (content_type, "application/powerpoint") == 0) {
    ret = LIBMTP_FILETYPE_PPT;
  } else if (strcmp (content_type, "message/rfc822") == 0) {
    ret = LIBMTP_FILETYPE_MHT;
  } else if (strcmp (content_type, "image/jp2") == 0) {
    ret = LIBMTP_FILETYPE_JP2;
  } else if (strcmp (content_type, "image/jpx") == 0) {
    ret = LIBMTP_FILETYPE_JPX;
  } else if (strcmp (content_type, "audio/x-mpegurl") == 0 ||
             strcmp (content_type, "audio/mpegurl") == 0 ||
             strcmp (content_type, "application/m3u") == 0 ||
             strcmp (content_type, "audio/x-mp3-playlist") == 0 ||
             strcmp (content_type, "audio/m3u") == 0 ||
             strcmp (content_type, "audio/x-m3u") == 0) {
    ret = LIBMTP_FILETYPE_PLAYLIST;
  } else if (strncmp (content_type, "audio/", 6) == 0) {
    // Must come after all other audio types.
    ret = LIBMTP_FILETYPE_UNDEF_AUDIO;
  } else if (strncmp (content_type, "video/", 6) == 0) {
    // Must come after all other video types.
    ret = LIBMTP_FILETYPE_UNDEF_VIDEO;
  } else {
    ret = LIBMTP_FILETYPE_UNKNOWN;
  }

  /* Unmappable Types
    LIBMTP_FILETYPE_JFIF,
    LIBMTP_FILETYPE_FIRMWARE,
    LIBMTP_FILETYPE_MEDIACARD,
    LIBMTP_FILETYPE_ALBUM,
  */

  g_debug ("(II) get_filetype_from_info done.\n");

  return ret;
}


static void
do_push (GVfsBackend *backend,
         GVfsJobPush *job,
         const char *destination,
         const char *local_path,
         GFileCopyFlags flags,
         gboolean remove_source,
         GFileProgressCallback progress_callback,
         gpointer progress_callback_data)
{
  g_debug ("(I) do_push (filename = %s, local_path = %s)\n", destination, local_path);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  char *dir_name = g_path_get_dirname (destination);
  char *filename = g_path_get_basename (destination);

  GFile *local_file = NULL;
  GFileInfo *info = NULL;
  gchar **elements = g_strsplit_set (destination, "/", -1);
  unsigned int ne = g_strv_length (elements);

  if (remove_source && (flags & G_FILE_COPY_NO_FALLBACK_FOR_MOVE)) {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      _("Operation not supported"));
    goto exit;
  }

  if (ne < 3) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                              _("Cannot write to this location"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  CacheEntry *entry = get_cache_entry (G_VFS_BACKEND_MTP (backend), destination);
  gboolean dest_exists = entry != NULL && entry->id != -1;
  gboolean dest_is_dir = FALSE;

  if (dest_exists) {
    LIBMTP_file_t *file = LIBMTP_Get_Filemetadata (device, entry->id);
    if (file != NULL) {
      dest_is_dir = file->filetype == LIBMTP_FILETYPE_FOLDER;
      LIBMTP_destroy_file_t (file);
    }
  }

  CacheEntry *parent = get_cache_entry (G_VFS_BACKEND_MTP (backend), dir_name);
  if (!parent) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("Directory doesn’t exist"));
    goto exit;
  }

  GError *error = NULL;
  local_file = g_file_new_for_path (local_path);
  info =
    g_file_query_info (local_file,
                       G_FILE_ATTRIBUTE_STANDARD_TYPE","
                       G_FILE_ATTRIBUTE_STANDARD_SIZE","
                       G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
                       0, G_VFS_JOB (job)->cancellable, &error);
  if (!info) {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    g_error_free (error);
    goto exit;
  }

  gboolean source_is_dir =
    g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;

  gboolean valid_push = validate_source_and_dest (dest_exists,
                                                  dest_is_dir,
                                                  source_is_dir,
                                                  FALSE, // source_can_be_dir
                                                  flags,
                                                  G_VFS_JOB (job));
  if (!valid_push) {
    goto exit;
  } else if (dest_exists) {
    /* Source and Dest are files */
    g_debug ("(I) Removing destination.\n");
    int ret = LIBMTP_Delete_Object (device, entry->id);
    if (ret != 0) {
      fail_job (G_VFS_JOB (job), device);
      goto exit;
    }
    g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                          emit_delete_event,
                          (char *)destination);
    remove_cache_entry (G_VFS_BACKEND_MTP (backend),
                        destination);
  }

  LIBMTP_file_t *mtpfile = LIBMTP_new_file_t ();
  mtpfile->filename = strdup (filename);
  mtpfile->parent_id = parent->id;
  mtpfile->storage_id = parent->storage;
  mtpfile->filetype = get_filetype_from_info (info);
  mtpfile->filesize = g_file_info_get_size (info);

  MtpProgressData mtp_progress_data;
  mtp_progress_data.progress_callback = progress_callback;
  mtp_progress_data.progress_callback_data = progress_callback_data;
  mtp_progress_data.job = G_VFS_JOB (job);
  int ret = LIBMTP_Send_File_From_File (device, local_path, mtpfile,
                                        (LIBMTP_progressfunc_t)mtp_progress,
                                        &mtp_progress_data);
  LIBMTP_destroy_file_t (mtpfile);
  if (ret != 0) {
    fail_job (G_VFS_JOB (job), device);
    goto exit;
  }

  /* Attempt to delete object if requested but don't fail it it fails. */
  if (remove_source) {
    g_debug ("(I) Removing source.\n");
    g_file_delete (local_file, G_VFS_JOB (job)->cancellable, NULL);
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                        emit_create_event,
                        (char *)destination);

 exit:
  g_clear_object (&local_file);
  g_clear_object (&info);
  g_strfreev (elements);
  g_free (dir_name);
  g_free (filename);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  g_debug ("(I) do_push done.\n");
}


static void
do_delete (GVfsBackend *backend,
            GVfsJobDelete *job,
            const char *filename)
{
  g_debug ("(I) do_delete (filename = %s)\n", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  CacheEntry *entry = get_cache_entry (G_VFS_BACKEND_MTP (backend), filename);
  if (entry == NULL) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("File doesn’t exist"));
    goto exit;
  } else if (entry->id == -1) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                              _("Not a regular file"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;
  LIBMTP_file_t *file = LIBMTP_Get_Filemetadata (device, entry->id);
  if (file->filetype == LIBMTP_FILETYPE_FOLDER) {
    LIBMTP_file_t *files;
    LIBMTP_Clear_Errorstack (device);
    files = LIBMTP_Get_Files_And_Folders (device, entry->storage, entry->id);
    if (LIBMTP_Get_Errorstack (device) != NULL) {
      fail_job (G_VFS_JOB (job), device);
      goto exit;
    }

    if (files != NULL) {
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_NOT_EMPTY,
                                g_strerror (ENOTEMPTY));
      g_debug ("(II) Directory size %" G_GSIZE_FORMAT "\n", file->filesize);
      goto exit;
    }
  }

  int ret = LIBMTP_Delete_Object (device, entry->id);
  if (ret != 0) {
    fail_job (G_VFS_JOB (job), device);
    goto exit;
  }
  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                        emit_delete_event,
                        (char *)filename);
  remove_cache_entry (G_VFS_BACKEND_MTP (backend),
                      filename);

 exit:
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  g_debug ("(I) do_delete done.\n");
}


static void
do_set_display_name (GVfsBackend *backend,
                      GVfsJobSetDisplayName *job,
                      const char *filename,
                      const char *display_name)
{
  g_debug ("(I) do_set_display_name '%s' --> '%s'\n", filename, display_name);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  CacheEntry *entry = get_cache_entry (G_VFS_BACKEND_MTP (backend), filename);
  if (entry == NULL) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("File doesn’t exist"));
    goto exit;
  } else if (entry->id == -1) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                              _("Not a regular file"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  LIBMTP_file_t *file = LIBMTP_Get_Filemetadata (device, entry->id);
  int ret = LIBMTP_Set_File_Name (device, file, display_name);
  if (ret != 0) {
    fail_job (G_VFS_JOB (job), device);
    goto exit;
  }

  char *dir_name = g_path_get_dirname (filename);
  char *new_name = g_build_filename (dir_name, display_name, NULL);

  remove_cache_entry (G_VFS_BACKEND_MTP (backend),
                      filename);
  add_cache_entry (G_VFS_BACKEND_MTP (backend), new_name, file->storage_id, file->item_id);

  LIBMTP_destroy_file_t (file);
  file = NULL;

  g_vfs_job_set_display_name_set_new_path (job, new_name);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                        emit_create_event,
                        (char *)new_name);
  g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                        emit_delete_event,
                        (char *)filename);
  g_free (dir_name);

 exit:
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  g_debug ("(I) do_set_display_name done.\n");
}


static void
do_open_for_read (GVfsBackend *backend,
                  GVfsJobOpenForRead *job,
                  const char *filename)
{
  if (!G_VFS_BACKEND_MTP (backend)->android_extension &&
      !G_VFS_BACKEND_MTP (backend)->get_partial_object_capability) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                              _("Operation not supported"));
    return;
  }

  g_debug ("(I) do_open_for_read (%s)\n", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  CacheEntry *entry = get_cache_entry (G_VFS_BACKEND_MTP (backend), filename);
  if (entry == NULL) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("File doesn’t exist"));
    goto exit;
  } else if (entry->id == -1) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                              _("Not a regular file"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  LIBMTP_file_t *file = LIBMTP_Get_Filemetadata (device, entry->id);
  if (file == NULL) {
    fail_job (G_VFS_JOB (job), device);
    goto exit;
  }

  if (file->filetype == LIBMTP_FILETYPE_FOLDER) {
    LIBMTP_destroy_file_t (file);
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                             _("Can’t open directory"));
    goto exit;
  }

  RWHandle *handle = g_new0(RWHandle, 1);
  handle->handle_type = HANDLE_FILE;
  handle->id = entry->id;
  handle->offset = 0;
  handle->size = file->filesize;

  LIBMTP_destroy_file_t (file);

  g_vfs_job_open_for_read_set_can_seek (G_VFS_JOB_OPEN_FOR_READ (job), TRUE);
  g_vfs_job_open_for_read_set_handle (G_VFS_JOB_OPEN_FOR_READ (job), handle);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  g_debug ("(I) do_open_for_read done.\n");
}


static void
do_open_icon_for_read (GVfsBackend *backend,
                       GVfsJobOpenIconForRead *job,
                       const char *icon_id)
{
  g_debug ("(I) do_open_icon_for_read (%s)\n", icon_id);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  guint id = strtol (icon_id, NULL, 16);

  if (id > 0) {
    GByteArray *bytes = NULL;
    unsigned char *data;
    unsigned int size;
    int ret;

    ret = LIBMTP_Get_Thumbnail (G_VFS_BACKEND_MTP (backend)->device, id,
                                &data, &size);
    if (ret == 0) {
      g_debug ("File %X has thumbnail: %u\n", id, size);
      if (size > 0) {
        bytes = g_byte_array_sized_new (size);
        g_byte_array_append (bytes, data, size);
      }
      free (data);
    }

    if (!bytes) {
      LIBMTP_filesampledata_t *sample_data = LIBMTP_new_filesampledata_t ();
      ret = LIBMTP_Get_Representative_Sample (G_VFS_BACKEND_MTP (backend)->device,
                                              id, sample_data);
      if (ret == 0) {
        g_debug ("File %X has sampledata: %" G_GSIZE_FORMAT "\n", id, sample_data->size);
        if (sample_data->size > 0) {
          bytes = g_byte_array_sized_new (sample_data->size);
          g_byte_array_append (bytes, (const guint8 *)sample_data->data, sample_data->size);
          size = sample_data->size;
        }
      }
      LIBMTP_destroy_filesampledata_t (sample_data);
    }

    if (!bytes) {
      g_debug ("File %X has no thumbnail or sampledata\n", id);
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("No thumbnail for entity “%s”"),
                        icon_id);
      goto exit;
    }

    RWHandle *handle = g_new0(RWHandle, 1);
    handle->handle_type = HANDLE_PREVIEW;
    handle->id = id;
    handle->offset = 0;
    handle->size = size;
    handle->bytes = bytes;
    g_vfs_job_open_for_read_set_can_seek (G_VFS_JOB_OPEN_FOR_READ (job), TRUE);
    g_vfs_job_open_for_read_set_handle (G_VFS_JOB_OPEN_FOR_READ (job), handle);
    g_vfs_job_succeeded (G_VFS_JOB (job));
  } else {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR,
                      G_IO_ERROR_INVALID_ARGUMENT,
                      _("Malformed icon identifier “%s”"),
                      icon_id);
    goto exit;
  }

 exit:
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  g_debug ("(I) do_open_icon_for_read done.\n");
}


static void
do_seek_on_read (GVfsBackend *backend,
                 GVfsJobSeekRead *job,
                 GVfsBackendHandle opaque_handle,
                 goffset    offset,
                 GSeekType  type)
{
  RWHandle *handle = opaque_handle;
  uint32_t id = handle->id;
  goffset old_offset = handle->offset;
  gsize size = handle->size;

  g_debug ("(I) do_seek_on_read (%X %lu %ld %u)\n", id, old_offset, offset, type);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  if (type == G_SEEK_END) {
    offset = size + offset;
  } else if (type == G_SEEK_CUR) {
    offset += old_offset;
  }

  if (offset < 0) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                              _("End of stream"));
    goto exit;
  }

  handle->offset = offset;
  g_vfs_job_seek_read_set_offset (job, offset);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);
  g_debug ("(I) do_seek_on_read done. (%lu)\n", offset);
}


static void
do_read (GVfsBackend *backend,
         GVfsJobRead *job,
         GVfsBackendHandle opaque_handle,
         char *buffer,
         gsize bytes_requested)
{
  RWHandle *handle = opaque_handle;
  uint32_t id = handle->id;
  goffset offset = handle->offset;

  g_debug ("(I) do_read (%X %lu %lu)\n", id, offset, bytes_requested);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  uint32_t actual;
  if (handle->handle_type == HANDLE_FILE) {
    if (!G_VFS_BACKEND_MTP (backend)->android_extension &&
        offset > G_MAXUINT32) {
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                _("Operation not supported"));
      goto exit;
    }

    /*
     * Almost all android devices have a bug where they do not enforce
     * POSIX semantics for read past EOF, leading to undefined
     * behaviour including device-side hangs. We'd better handle it
     * here.
     */
    if (offset >= handle->size) {
      g_debug ("(II) skipping read with offset past EOF\n");
      actual = 0;
      goto finished;
    } else if (offset + bytes_requested > handle->size) {
      g_debug ("(II) reducing bytes_requested to avoid reading past EOF\n");
      bytes_requested = handle->size - offset;
    }

    unsigned char *temp;
    int ret = LIBMTP_GetPartialObject (G_VFS_BACKEND_MTP (backend)->device, id, offset,
                                       bytes_requested, &temp, &actual);
    if (ret != 0) {
      fail_job (G_VFS_JOB (job), G_VFS_BACKEND_MTP (backend)->device);
      g_debug ("(I) job failed.\n");
      goto exit;
    }

    memcpy (buffer, temp, actual);
    free (temp);
  } else {
    GByteArray *bytes = handle->bytes;
    actual = MIN (bytes->len - offset, bytes_requested);
    memcpy (buffer, bytes->data + offset, actual);
  }

 finished:
  handle->offset = offset + actual;
  g_vfs_job_read_set_size (job, actual);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);
  g_debug ("(I) do_read done.\n");
}

static void
do_close_read (GVfsBackend *backend,
               GVfsJobCloseRead *job,
               GVfsBackendHandle opaque_handle)
{
  g_debug ("(I) do_close_read\n");
  RWHandle *handle = opaque_handle;
  if (handle->bytes) {
    g_byte_array_unref (handle->bytes);
  }
  g_free(handle);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_debug ("(I) do_close_read done.\n");
}


static uint16_t
zero_get_func (void* params,
               void* priv,
               uint32_t wantlen,
               unsigned char *data,
               uint32_t *gotlen)
{
  *gotlen = 0;
  return LIBMTP_HANDLER_RETURN_OK;
}


static void
open_for_write (GVfsBackend *backend,
                GVfsJobOpenForWrite *job,
                const char *filename,
                GFileCreateFlags flags)
{
  if (!G_VFS_BACKEND_MTP (backend)->android_extension) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                              _("Operation not supported"));
    return;
  }

  g_debug ("(I) open_for_write (%s)\n", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  char *dir_name = g_path_get_dirname (filename);
  char *basename = g_path_get_basename (filename);

  gchar **elements = g_strsplit_set (filename, "/", -1);
  unsigned int ne = g_strv_length (elements);

  if (ne < 3) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                              _("Cannot write to this location"));
    goto exit;
  }

  CacheEntry *entry = get_cache_entry (G_VFS_BACKEND_MTP (backend), filename);
  if (job->mode == OPEN_FOR_WRITE_CREATE &&
      entry != NULL) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_EXISTS,
                              _("Target file already exists"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;
  LIBMTP_file_t *file;

  if (entry == NULL) {
    CacheEntry *parent = get_cache_entry (G_VFS_BACKEND_MTP (backend), dir_name);
    if (parent == NULL) {
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                _("Directory doesn’t exist"));
      goto exit;
    }

    file = LIBMTP_new_file_t ();
    file->filename = strdup (basename);
    file->parent_id = parent->id;
    file->storage_id = parent->storage;
    file->filetype = LIBMTP_FILETYPE_UNKNOWN;
    file->filesize = 0;

    int ret = LIBMTP_Send_File_From_Handler (device, zero_get_func, NULL,
                                             file, NULL, NULL);
    if (ret != 0) {
      LIBMTP_destroy_file_t (file);
      fail_job (G_VFS_JOB (job), device);
      g_debug ("(I) Failed to create empty file.\n");
      goto exit;
    }
  } else {
    file = LIBMTP_Get_Filemetadata (device, entry->id);
    if (file == NULL) {
      fail_job (G_VFS_JOB (job), device);
      g_debug ("(I) Failed to get metadata.\n");
      goto exit;
    }

    if (file->filetype == LIBMTP_FILETYPE_FOLDER) {
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                _("Target file is a directory"));
      LIBMTP_destroy_file_t (file);
      goto exit;
    }
  }

  int ret = LIBMTP_BeginEditObject (device, file->item_id);
  if (ret != 0) {
    fail_job (G_VFS_JOB (job), device);
    g_debug ("(I) Failed to begin edit.\n");
    LIBMTP_destroy_file_t (file);
    goto exit;
  }

  RWHandle *handle = g_new0(RWHandle, 1);
  handle->handle_type = HANDLE_FILE;
  handle->id = file->item_id;
  handle->offset = (job->mode == OPEN_FOR_WRITE_APPEND) ? file->filesize : 0;
  handle->size = file->filesize;
  handle->mode = job->mode;

  LIBMTP_destroy_file_t (file);

  g_vfs_job_open_for_write_set_initial_offset (job, handle->offset);
  g_vfs_job_open_for_write_set_can_seek (G_VFS_JOB_OPEN_FOR_WRITE (job), TRUE);
  g_vfs_job_open_for_write_set_can_truncate (G_VFS_JOB_OPEN_FOR_WRITE (job), TRUE);
  g_vfs_job_open_for_write_set_handle (G_VFS_JOB_OPEN_FOR_WRITE (job), handle);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                        emit_create_event,
                        (char *)filename);
 exit:
  g_strfreev (elements);
  g_free (basename);
  g_free (dir_name);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  g_debug ("(I) open_for_write done.\n");
}


static void
do_create (GVfsBackend *backend,
           GVfsJobOpenForWrite *job,
           const char *filename,
           GFileCreateFlags flags)
{
  open_for_write (backend, job, filename, flags);
}


static void
do_append_to (GVfsBackend *backend,
              GVfsJobOpenForWrite *job,
              const char *filename,
              GFileCreateFlags flags)
{
  open_for_write (backend, job, filename, flags);
}


static void
do_edit (GVfsBackend *backend,
         GVfsJobOpenForWrite *job,
         const char *filename,
         GFileCreateFlags flags)
{
  open_for_write (backend, job, filename, flags);
}


static void
do_replace (GVfsBackend *backend,
            GVfsJobOpenForWrite *job,
            const char *filename,
            const char *etag,
            gboolean make_backup,
            GFileCreateFlags flags)
{
  if (!G_VFS_BACKEND_MTP (backend)->android_extension) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                              _("Operation not supported"));
    return;
  }

  g_debug ("(I) do_replace (%s)\n", filename);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  CacheEntry *entry = get_cache_entry (G_VFS_BACKEND_MTP (backend), filename);
  if (entry == NULL) {
    g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);
    return do_create(backend, job, filename, flags);
  } else if (entry->id == -1) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                              _("Not a regular file"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  LIBMTP_file_t *file = LIBMTP_Get_Filemetadata (device, entry->id);
  if (file == NULL) {
    fail_job (G_VFS_JOB (job), device);
    g_debug ("(I) Failed to get metadata.\n");
    goto exit;
  }

  if (file->filetype == LIBMTP_FILETYPE_FOLDER) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                              _("Target file is a directory"));
    LIBMTP_destroy_file_t (file);
    goto exit;
  }

  int ret = LIBMTP_BeginEditObject (device, entry->id);
  if (ret != 0) {
    fail_job (G_VFS_JOB (job), device);
    g_debug ("(I) Failed to begin edit.\n");
    goto exit;
  }

  ret = LIBMTP_TruncateObject (device, entry->id, 0);
  if (ret != 0) {
    fail_job (G_VFS_JOB (job), device);
    g_debug ("(I) Failed to truncate.\n");
    goto exit;
  }

  RWHandle *handle = g_new0(RWHandle, 1);
  handle->handle_type = HANDLE_FILE;
  handle->id = entry->id;
  handle->offset = 0;
  handle->size = 0;
  handle->mode = job->mode;

  LIBMTP_destroy_file_t (file);

  g_vfs_job_open_for_write_set_can_seek (G_VFS_JOB_OPEN_FOR_WRITE (job), TRUE);
  g_vfs_job_open_for_write_set_can_truncate (G_VFS_JOB_OPEN_FOR_WRITE (job), TRUE);
  g_vfs_job_open_for_write_set_handle (G_VFS_JOB_OPEN_FOR_WRITE (job), handle);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  g_debug ("(I) do_replace done.\n");
}

#define PAD_BLOCK_SIZE 1024

static int
pad_file (GVfsBackend *backend,
          uint32_t id,
          goffset offset,
          gsize size)
{
  unsigned char zero_buffer[PAD_BLOCK_SIZE] = { 0 };
  gsize written;
  int ret = 0;

  for (written = 0; written < size; written += PAD_BLOCK_SIZE)
    {
      ret = LIBMTP_SendPartialObject (G_VFS_BACKEND_MTP (backend)->device,
                                      id,
                                      offset + written,
                                      zero_buffer,
                                      MIN (size - written, PAD_BLOCK_SIZE));
      if (ret != 0)
        break;
    }

  return ret;
}

static void
do_write (GVfsBackend *backend,
          GVfsJobWrite *job,
          GVfsBackendHandle opaque_handle,
          char *buffer,
          gsize buffer_size)
{
  RWHandle *handle = opaque_handle;
  uint32_t id = handle->id;
  goffset offset = handle->offset;

  g_debug ("(I) do_write (%X %lu %lu)\n", id, offset, buffer_size);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  if (handle->mode == OPEN_FOR_WRITE_APPEND) {
    offset = handle->size;
  }

  if (offset > handle->size) {
    int ret = pad_file (backend, id, handle->size, offset - handle->size);
    if (ret != 0) {
      fail_job (G_VFS_JOB (job), G_VFS_BACKEND_MTP (backend)->device);
      g_debug ("(I) job failed.\n");
      goto exit;
    }
  }

  int ret = LIBMTP_SendPartialObject (G_VFS_BACKEND_MTP (backend)->device, id, offset,
                                      (unsigned char *)buffer, buffer_size);
  if (ret != 0) {
    fail_job (G_VFS_JOB (job), G_VFS_BACKEND_MTP (backend)->device);
    g_debug ("(I) job failed.\n");
    goto exit;
  }

  handle->offset = offset + buffer_size;
  if (handle->offset > handle->size) {
    handle->size = handle->offset;
  }

  g_vfs_job_write_set_written_size (job, buffer_size);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);
  g_debug ("(I) do_write done.\n");
}


static void
do_seek_on_write (GVfsBackend *backend,
                  GVfsJobSeekWrite *job,
                  GVfsBackendHandle opaque_handle,
                  goffset offset,
                  GSeekType type)
{
  RWHandle *handle = opaque_handle;
  uint32_t id = handle->id;
  goffset old_offset = handle->offset;
  gsize size = handle->size;

  g_debug ("(I) do_seek_on_write (%X %lu %ld %u)\n", id, old_offset, offset, type);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  if (type == G_SEEK_END) {
    offset = size + offset;
  } else if (type == G_SEEK_CUR) {
    offset += old_offset;
  }

  if (offset < 0) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                              _("End of stream"));
    goto exit;
  }

  handle->offset = offset;
  g_vfs_job_seek_write_set_offset (job, offset);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);
  g_debug ("(I) do_seek_on_write done. (%lu)\n", offset);
}


static void
do_truncate (GVfsBackend *backend,
             GVfsJobTruncate *job,
             GVfsBackendHandle opaque_handle,
             goffset size)
{
  RWHandle *handle = opaque_handle;
  uint32_t id = handle->id;

  g_debug("(I) do_truncate (%ld)\n", size);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  if (LIBMTP_TruncateObject (G_VFS_BACKEND_MTP (backend)->device, id, size) == 0) {
    handle->size = size;
    g_vfs_job_succeeded (G_VFS_JOB (job));
  } else {
    fail_job (G_VFS_JOB (job), G_VFS_BACKEND_MTP (backend)->device);
    g_debug ("(I) Failed to truncate.\n");
  }

  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);
  g_debug ("(I) truncate done.\n");
}


static void
do_close_write (GVfsBackend *backend,
                GVfsJobCloseWrite *job,
                GVfsBackendHandle opaque_handle)
{
  g_debug ("(I) do_close_write\n");
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  RWHandle *handle = opaque_handle;

  int ret = LIBMTP_EndEditObject (G_VFS_BACKEND_MTP (backend)->device, handle->id);
  if (ret != 0) {
    fail_job (G_VFS_JOB (job), G_VFS_BACKEND_MTP (backend)->device);
    goto exit;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 exit:
  g_free(handle);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);
  g_debug ("(I) do_close_write done.\n");
}


#if HAVE_LIBMTP_1_1_15
static void
do_move (GVfsBackend *backend,
         GVfsJobMove *job,
         const char *source,
         const char *destination,
         GFileCopyFlags flags,
         GFileProgressCallback progress_callback,
         gpointer progress_callback_data)
{
  if (!G_VFS_BACKEND_MTP (backend)->move_object_capability) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                              _("Operation not supported"));
    return;
  }

  g_debug ("(I) do_move (source = %s, dest = %s)\n", source, destination);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  char *dir_name = g_path_get_dirname (destination);
  char *src_name = g_path_get_basename (source);
  char *dest_name = g_path_get_basename (destination);

  gchar **elements = g_strsplit_set (destination, "/", -1);
  unsigned int ne = g_strv_length (elements);

  if (ne < 3) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                              _("Cannot write to this location"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  CacheEntry *src_entry = get_cache_entry (G_VFS_BACKEND_MTP (backend), source);
  if (src_entry == NULL) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("File doesn’t exist"));
    goto exit;
  } else if (src_entry->id == -1) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                              _("Not a regular file"));
    goto exit;
  }

  gboolean source_is_dir = FALSE;
  uint64_t filesize = 0;
  LIBMTP_file_t *file = LIBMTP_Get_Filemetadata (device, src_entry->id);
  if (file != NULL) {
    source_is_dir = (file->filetype == LIBMTP_FILETYPE_FOLDER);
    /*
     * filesize is 0 for directories. However, given that we will only move
     * a directory if it's staying on the same storage, then these moves
     * will always be fast, and will finish too quickly for the progress
     * value to matter. Moves between storages will be decomposed, with
     * each file moved separately.
     */
    filesize = file->filesize;
    LIBMTP_destroy_file_t (file);
  }

  CacheEntry *entry = get_cache_entry (G_VFS_BACKEND_MTP (backend), destination);
  gboolean dest_exists = (entry != NULL && entry->id != -1);
  gboolean dest_is_dir = FALSE;

  if (dest_exists) {
    LIBMTP_file_t *file = LIBMTP_Get_Filemetadata (device, entry->id);
    if (file != NULL) {
      dest_is_dir = (file->filetype == LIBMTP_FILETYPE_FOLDER);
      LIBMTP_destroy_file_t (file);
    }
  }

  CacheEntry *parent = get_cache_entry (G_VFS_BACKEND_MTP (backend), dir_name);
  if (!parent) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("Directory doesn’t exist"));
    goto exit;
  }

  // Only do directory moves on the same storage, where they are fast.
  gboolean source_can_be_dir = (parent->storage == src_entry->storage);
  gboolean valid_move = validate_source_and_dest (dest_exists,
                                                  dest_is_dir,
                                                  source_is_dir,
                                                  source_can_be_dir,
                                                  flags,
                                                  G_VFS_JOB (job));
  if (!valid_move) {
    goto exit;
  } else if (dest_exists) {
    /* Source and Dest are files */
    g_debug ("(I) Removing destination.\n");
    int ret = LIBMTP_Delete_Object (device, entry->id);
    if (ret != 0) {
      fail_job (G_VFS_JOB (job), device);
      goto exit;
    }
    g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                          emit_delete_event,
                          (char *)destination);
    remove_cache_entry (G_VFS_BACKEND_MTP (backend),
                        destination);
  }

  // If file names are different, rename it first
  if (g_strcmp0 (src_name, dest_name) != 0) {
    g_debug ("(I) do_move: File names different, attempting rename from '%s' to '%s'\n", 
             src_name, dest_name);
    LIBMTP_file_t *file = LIBMTP_Get_Filemetadata (device, src_entry->id);
    if (file != NULL) {
      int ret = LIBMTP_Set_File_Name (device, file, dest_name);
      LIBMTP_destroy_file_t (file);
      
      if (ret != 0) {
        fail_job (G_VFS_JOB (job), device);
        goto exit;
      }
    }
  }
  
  // Determine whether it is a move within the same directory
  char *src_dir = g_path_get_dirname (source);
  gboolean same_dir = (g_strcmp0 (src_dir, dir_name) == 0);
  g_free (src_dir);

  if (!same_dir) {
    /* Unlike most calls, we must pass 0 for the root directory.*/
    uint32_t parent_id = (parent->id == -1 ? 0 : parent->id);
    int ret = LIBMTP_Move_Object (device,
                                  src_entry->id,
                                  parent->storage,
                                  parent_id);
    if (ret != 0) {
      fail_job (G_VFS_JOB (job), device);
      goto exit;
    }
  }

  if (progress_callback) {
    progress_callback (filesize, filesize, progress_callback_data);
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                        emit_delete_event,
                        (char *)source);
  g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                        emit_create_event,
                        (char *)destination);

 exit:
  g_strfreev (elements);
  g_free (dir_name);
  g_free (src_name);
  g_free (dest_name);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  g_debug ("(I) do_move done.\n");
}


static void
do_copy (GVfsBackend *backend,
         GVfsJobCopy *job,
         const char *source,
         const char *destination,
         GFileCopyFlags flags,
         GFileProgressCallback progress_callback,
         gpointer progress_callback_data)
{
  if (!G_VFS_BACKEND_MTP (backend)->copy_object_capability) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                              _("Operation not supported"));
    return;
  }

  g_debug ("(I) do_copy (source = %s, dest = %s)\n", source, destination);
  g_mutex_lock (&G_VFS_BACKEND_MTP (backend)->mutex);

  char *dir_name = g_path_get_dirname (destination);
  char *filename = g_path_get_basename (destination);

  gchar **elements = g_strsplit_set (destination, "/", -1);
  unsigned int ne = g_strv_length (elements);

  if (ne < 3) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                              _("Cannot write to this location"));
    goto exit;
  }

  LIBMTP_mtpdevice_t *device;
  device = G_VFS_BACKEND_MTP (backend)->device;

  CacheEntry *src_entry = get_cache_entry (G_VFS_BACKEND_MTP (backend), source);
  if (src_entry == NULL) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("File doesn’t exist"));
    goto exit;
  } else if (src_entry->id == -1) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                              _("Not a regular file"));
    goto exit;
  }

  gboolean source_is_dir = FALSE;
  uint64_t filesize = 0;
  LIBMTP_file_t *file = LIBMTP_Get_Filemetadata (device, src_entry->id);
  if (file != NULL) {
    source_is_dir = (file->filetype == LIBMTP_FILETYPE_FOLDER);
    filesize = file->filesize;
    LIBMTP_destroy_file_t (file);
  }

  CacheEntry *entry = get_cache_entry (G_VFS_BACKEND_MTP (backend), destination);
  gboolean dest_exists = (entry != NULL && entry->id != -1);
  gboolean dest_is_dir = FALSE;

  if (dest_exists) {
    LIBMTP_file_t *file = LIBMTP_Get_Filemetadata (device, entry->id);
    if (file != NULL) {
      dest_is_dir = (file->filetype == LIBMTP_FILETYPE_FOLDER);
      LIBMTP_destroy_file_t (file);
    }
  }

  CacheEntry *parent = get_cache_entry (G_VFS_BACKEND_MTP (backend), dir_name);
  if (!parent) {
    g_vfs_job_failed_literal (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("Directory doesn’t exist"));
    goto exit;
  }

  /*
   * We ignore the ability to copy whole folders because we get poor progress
   * updates in this situation. At least with file-by-file copies, we can
   * notify as each file completes.
   */
  gboolean valid_copy = validate_source_and_dest (dest_exists,
                                                  dest_is_dir,
                                                  source_is_dir,
                                                  FALSE, // source_can_be_dir
                                                  flags,
                                                  G_VFS_JOB (job));
  if (!valid_copy) {
    goto exit;
  } else if (dest_exists) {
    /* Source and Dest are files */
    g_debug ("(I) Removing destination.\n");
    int ret = LIBMTP_Delete_Object (device, entry->id);
    if (ret != 0) {
      fail_job (G_VFS_JOB (job), device);
      goto exit;
    }
    g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                          emit_delete_event,
                          (char *)destination);
    remove_cache_entry (G_VFS_BACKEND_MTP (backend),
                        destination);
  }

  /* Unlike most calls, we must pass 0 for the root directory.*/
  uint32_t parent_id = (parent->id == -1) ? 0 : parent->id;
  int ret = LIBMTP_Copy_Object (device,
                                src_entry->id,
                                parent->storage,
                                parent_id);
  if (ret != 0) {
    fail_job (G_VFS_JOB (job), device);
    goto exit;
  }

  if (progress_callback) {
    progress_callback (filesize, filesize, progress_callback_data);
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_foreach (G_VFS_BACKEND_MTP (backend)->monitors,
                        emit_create_event,
                        (char *)destination);

 exit:
  g_strfreev (elements);
  g_free (dir_name);
  g_free (filename);
  g_mutex_unlock (&G_VFS_BACKEND_MTP (backend)->mutex);

  g_debug ("(I) do_copy done.\n");
}
#endif /* HAVE_LIBMTP_1_1_15 */


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
  backend_class->make_directory = do_make_directory;
  backend_class->delete = do_delete;
  backend_class->set_display_name = do_set_display_name;
  backend_class->create_dir_monitor = do_create_dir_monitor;
  backend_class->create_file_monitor = do_create_file_monitor;
  backend_class->open_for_read = do_open_for_read;
  backend_class->open_icon_for_read = do_open_icon_for_read;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->read = do_read;
  backend_class->close_read = do_close_read;
  backend_class->create = do_create;
  backend_class->append_to = do_append_to;
  backend_class->edit = do_edit;
  backend_class->replace = do_replace;
  backend_class->write = do_write;
  backend_class->seek_on_write = do_seek_on_write;
  backend_class->truncate = do_truncate;
  backend_class->close_write = do_close_write;
#if HAVE_LIBMTP_1_1_15
  backend_class->move = do_move;
  backend_class->copy = do_copy;
#endif
}
