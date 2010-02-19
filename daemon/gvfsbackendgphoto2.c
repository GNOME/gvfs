
/* GVFS gphoto2 file system driver
 * 
 * Copyright (C) 2007-2008 Red Hat, Inc.
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
 *
 * Author: David Zeuthen <davidz@redhat.com>
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
#include <gphoto2.h>
#ifdef HAVE_GUDEV
  #include <gudev/gudev.h>
#elif defined(HAVE_HAL)
  #include <libhal.h>
  #include <dbus/dbus.h>
#else
  #error Needs hal or gudev
#endif
#include <sys/time.h>

#include "gvfsbackendgphoto2.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobopeniconforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsjobunmount.h"
#include "gvfsmonitor.h"
#include "gvfsjobseekwrite.h"
#include "gvfsicon.h"

/* showing debug traces */
#if 1
#define DEBUG_SHOW_TRACES 1
#endif

/* showing libgphoto2 output */
#if 0
#define DEBUG_SHOW_LIBGPHOTO2_OUTPUT 1
#endif

/* use this to disable caching */
#if 0
#define DEBUG_NO_CACHING 1
#endif

/*--------------------------------------------------------------------------------------------------------------*/

/* TODO:
 *
 *  - write support
 *    - it's in; we support writing. yay.
 *      - though there's no way to rename an non-empty folder yet
 *    - there's an assumption, for caching, that the device won't
 *      be able to put files while we're using it. May have to
 *      revisit that if such devices exist.
 *      - one solution: make cache items valid for only five seconds or something
 *
 *    - Note that most PTP devices (e.g. digital cameras) don't support writing
 *      - Most MTP devices (e.g. digital audio players) do
 *
 *    - However, some MTP devices are just busted when using ~ backup
 *      style; see below. This is with my (davidz) Sandisk Sansa
 *      e250. This is probably a firmware bug; when investigating
 *      libgphoto2 reports everything as peachy.
 *
 *         $ pwd
 *         /home/davidz/.gvfs/gphoto2 mount on usb%3A001,051/foo
 *         $ echo a > a
 *         $ echo b > b
 *         $ ls -l
 *         total 0
 *         -rw------- 1 davidz davidz 2 2008-03-02 13:22 a
 *         -rw------- 1 davidz davidz 2 2008-03-02 13:22 b
 *         $ cat a
 *         a
 *         $ cat b
 *         b
 *         $ mv b a
 *         $ ls -l
 *         total 0
 *         -rw------- 1 davidz davidz 2 2008-03-02 13:22 a
 *         $ cat a
 *         b
 *         $ rm a
 *
 *      See, this worked fine.. Now see what happens if we
 *      use different files names
 *
 *         $ echo a > file.txt
 *         $ echo b > file.txt~
 *         $ ls -l
 *         total 0
 *         -rw------- 1 davidz davidz 2 2008-03-02 13:22 file.txt
 *         -rw------- 1 davidz davidz 2 2008-03-02 13:22 file.txt~
 *         $ cat file.txt
 *         a
 *         $ cat file.txt~
 *         b
 *         $ mv file.txt~ file.txt
 *         $ ls -l
 *         total 0
 *         -rw------- 1 davidz davidz 0 1969-12-31 18:59 file.txt
 *         $ cat file.txt 
 *         $
 *
 *      Awesome. I hate hardware.
 *
 *      - This is a bit bad as it affects most text editors (vim, emacs,
 *        gedit) and it actually results in data loss. However, there's
 *        little we can do about it.
 *
 *      - Would be nice to test this on other MTP devices to verify
 *        it's indeed a firmware bug in the Sansa Sandisk e250.
 *
 *      - This shouldn't affect stuff like Banshee or Rhythmbox using
 *        this backend for MTP support though; despite this bug basic
 *        file operations works nicely.
 *        - http://bugzilla.gnome.org/show_bug.cgi?id=520121
 *
 *      - Need to test this with a native gio version of gedit that should
 *        use replace() directly instead of fooling around with ~-style
 *        backup files
 *
 *  - adding a payload cache don't make much sense as libgphoto2 has a LRU cache already
 *    - (see comment in the do_close_write() function)
 *
 *  - Support PTP/IP devices nicely
 *    - Need hardware for testing
 *    - Should actually work out of the box; just try mounting e.g.
 *      gphoto2://[ptpip:<something]/ from either Nautilus or via
 *      gvfs-mount(1).
 *    - Need to automatically unmount when the device stops answering
 *    - May need authentication bits
 *    - Need integration into network://
 *      - does such devices use DNS-SD or UPNP?
 *    
 */

struct _GVfsBackendGphoto2
{
  GVfsBackend parent_instance;

  /* a gphoto2 specific identifier for the gphoto2 camera such as usb:001,041 */
  char *gphoto2_port;
  GPContext *context;
  Camera *camera;

  /* see comment in ensure_ignore_prefix() */
  char *ignore_prefix;

#ifdef HAVE_GUDEV
  GUdevClient *gudev_client;
  GUdevDevice *udev_device;
#elif defined(HAVE_HAL)
  DBusConnection *dbus_connection;
  LibHalContext *hal_ctx;
  char *hal_udi;
  char *hal_name;
#endif
  char *icon_name;

  /* whether we can write to the device */
  gboolean can_write;
  /* whether we can delete files from to the device */
  gboolean can_delete;

  /* This lock protects all members in this class that are not
   * used both on the main thread and on the IO thread. 
   *
   * It is used, among other places, in the try_* functions to return
   * already cached data quickly (to e.g. enumerate and get file info
   * while we're reading or writing a file from the device).
   *
   * Must only be held for very short amounts of time (e.g. no IO).
   */
  GMutex *lock;

  /* CACHES */

  /* free_space is set to -1 if we don't know or have modified the
   * device since last time we read it. If -1 we can't do
   * try_query_fs_info() and will fall back to do_query_fs_info().
   */
  gint64 free_space;
  gint64 capacity;

  /* fully qualified path -> GFileInfo */
  GHashTable *info_cache;

  /* dir name -> CameraList of (sub-) directory names in given directory */
  GHashTable *dir_name_cache;

  /* dir name -> CameraList of file names in given directory */
  GHashTable *file_name_cache;

  /* monitors (only used on the IO thread) */
  GList *dir_monitor_proxies;
  GList *file_monitor_proxies;

  /* list of open read handles (only used on the IO thread) */
  GList *open_read_handles;

  /* list of open write handles (only used on the IO thread) */
  GList *open_write_handles;
};

G_DEFINE_TYPE (GVfsBackendGphoto2, g_vfs_backend_gphoto2, G_VFS_TYPE_BACKEND);

/* ------------------------------------------------------------------------------------------------- */

typedef struct {
  /* this is the path of the dir/file including ignore_prefix */
  char *path;
  GVfsMonitor *vfs_monitor;
} MonitorProxy;

static void
monitor_proxy_free (MonitorProxy *proxy)
{
  g_free (proxy->path);
  /* vfs_monitor is owned by the gvfs core; see the functions
   * vfs_dir_monitor_destroyed() and do_create_monitor()
   */
}

/* ------------------------------------------------------------------------------------------------- */

typedef struct {
  /* filename as given from the vfs without ignore prefix e.g. /foo.txt */
  char *filename;

  /* filename with ignore prefix splitted into dir and name; e.g. "/store_00010001/" and "foo.txt" */
  char *dir;
  char *name;

  char *data;
  unsigned long int size;
  unsigned long int cursor;
  unsigned long int allocated_size;

  gboolean job_is_replace;
  gboolean job_is_append_to;

  gboolean delete_before;

  gboolean is_dirty;
} WriteHandle;

/* how much more memory to ask for when using g_realloc() when writing a file */
#define WRITE_INCREMENT 4096

typedef struct {
  CameraFile *file;

  const char *data;
  unsigned long int size;
  unsigned long int cursor;
} ReadHandle;

/* ------------------------------------------------------------------------------------------------- */

static void
DEBUG (const gchar *message, ...)
{
#ifdef DEBUG_SHOW_TRACES
  va_list args;
  va_start (args, message);
  g_vfprintf (stderr, message, args);
  va_end (args);
  g_fprintf (stderr, "\n");
  fflush (stderr);
#endif
}

/* ------------------------------------------------------------------------------------------------- */

static int commit_write_handle (GVfsBackendGphoto2 *gphoto2_backend, WriteHandle *write_handle);

static void
write_handle_free (WriteHandle *write_handle)
{
  g_free (write_handle->filename);
  g_free (write_handle->dir);
  g_free (write_handle->name);
  g_free (write_handle->data);
  g_free (write_handle);
}

/* This must be called before reading from the device to ensure that
 * all pending writes are written to the device.
 *
 * Must only be called on the IO thread.
 */
static void
ensure_not_dirty (GVfsBackendGphoto2 *gphoto2_backend)
{
  GList *l;

  for (l = gphoto2_backend->open_write_handles; l != NULL; l = l->next)
    {
      WriteHandle *write_handle = l->data;

      DEBUG ("ensure_not_dirty: looking at handle for '%s", write_handle->filename);

      if (write_handle->is_dirty)
        commit_write_handle (gphoto2_backend, write_handle);
    }
}

/* ------------------------------------------------------------------------------------------------- */

/* used when gphoto2 will take ownership of this data for it's LRU cache - and will use free(3) to free it */
static char *
dup_for_gphoto2 (char *gmem, unsigned long int size)
{
  char *mem;
  mem = malloc (size);
  memcpy (mem, gmem, size);
  return mem;
}

/* ------------------------------------------------------------------------------------------------- */

static void
monitors_emit_internal (GVfsBackendGphoto2 *gphoto2_backend, 
                        const char *dir, 
                        const char *name, 
                        GFileMonitorEvent event,
                        const char *event_name)
{
  GList *l;
  char *filepath;

  g_return_if_fail (g_str_has_prefix (dir, gphoto2_backend->ignore_prefix));

  DEBUG ("monitors_emit_internal() %s for '%s' '%s'", event_name, dir, name);

  for (l = gphoto2_backend->dir_monitor_proxies; l != NULL; l = l->next)
    {
      MonitorProxy *proxy = l->data;
      if (strcmp (proxy->path, dir) == 0)
        {
          char *path;
          path = g_build_filename (dir + strlen (gphoto2_backend->ignore_prefix), name, NULL);
          g_vfs_monitor_emit_event (proxy->vfs_monitor, event, path, NULL);
          DEBUG ("  emitted %s for '%s' on dir monitor for '%s'", event_name, path, dir);
          g_free (path);
        }
    }

  filepath = g_build_filename (dir, name, NULL);
  for (l = gphoto2_backend->file_monitor_proxies; l != NULL; l = l->next)
    {
      MonitorProxy *proxy = l->data;
      if (strcmp (proxy->path, filepath) == 0)
        {
          const char *path = filepath + strlen (gphoto2_backend->ignore_prefix);
          g_vfs_monitor_emit_event (proxy->vfs_monitor, event, path, NULL);
          DEBUG ("  emitted %s for '%s' on file monitor", event_name, path);
        }
    }
  g_free (filepath);
}

/* ------------------------------------------------------------------------------------------------- */

/* call this when a file/directory have been added to a directory */
static void
monitors_emit_created (GVfsBackendGphoto2 *gphoto2_backend, const char *dir, const char *name)
{
  DEBUG ("monitors_emit_created(): '%s' '%s'", dir, name);
  monitors_emit_internal (gphoto2_backend, dir, name, G_FILE_MONITOR_EVENT_CREATED, "CREATED");
}

/* ------------------------------------------------------------------------------------------------- */

/* call this when a file/directory have been deleted from a directory */
static void
monitors_emit_deleted (GVfsBackendGphoto2 *gphoto2_backend, const char *dir, const char *name)
{
  DEBUG ("monitors_emit_deleted(): '%s' '%s'", dir, name);
  monitors_emit_internal (gphoto2_backend, dir, name, G_FILE_MONITOR_EVENT_DELETED, "DELETED");
}

/* ------------------------------------------------------------------------------------------------- */

/* call this when a file/directory have been changed in a directory */
static void
monitors_emit_changed (GVfsBackendGphoto2 *gphoto2_backend, const char *dir, const char *name)
{
  DEBUG ("monitors_emit_changed(): '%s' '%s'", dir, name);
  monitors_emit_internal (gphoto2_backend, dir, name, G_FILE_MONITOR_EVENT_CHANGED, "CHANGED");
}

/* ------------------------------------------------------------------------------------------------- */

static void
caches_invalidate_all (GVfsBackendGphoto2 *gphoto2_backend)
{
  DEBUG ("caches_invalidate_all()");

  g_mutex_lock (gphoto2_backend->lock);
  if (gphoto2_backend->dir_name_cache != NULL)
    g_hash_table_remove_all (gphoto2_backend->dir_name_cache);
  if (gphoto2_backend->file_name_cache != NULL)
    g_hash_table_remove_all (gphoto2_backend->file_name_cache);
  if (gphoto2_backend->info_cache != NULL)
    g_hash_table_remove_all (gphoto2_backend->info_cache);
  gphoto2_backend->capacity = -1;  
  gphoto2_backend->free_space = -1;  
  g_mutex_unlock (gphoto2_backend->lock);
}

/* ------------------------------------------------------------------------------------------------- */

static void
caches_invalidate_free_space (GVfsBackendGphoto2 *gphoto2_backend)
{
  g_mutex_lock (gphoto2_backend->lock);
  gphoto2_backend->free_space = -1;  
  g_mutex_unlock (gphoto2_backend->lock);
}

/* ------------------------------------------------------------------------------------------------- */

static void
caches_invalidate_dir (GVfsBackendGphoto2 *gphoto2_backend, const char *dir)
{
  DEBUG ("caches_invalidate_dir() for '%s'", dir);
  g_mutex_lock (gphoto2_backend->lock);
  g_hash_table_remove (gphoto2_backend->dir_name_cache, dir);
  g_hash_table_remove (gphoto2_backend->file_name_cache, dir);
  g_hash_table_remove (gphoto2_backend->info_cache, dir);
  g_mutex_unlock (gphoto2_backend->lock);
}

/* ------------------------------------------------------------------------------------------------- */

static void
caches_invalidate_file (GVfsBackendGphoto2 *gphoto2_backend, const char *dir, const char *name)
{
  char *full_name;

  full_name = g_build_filename (dir, name, NULL);

  g_mutex_lock (gphoto2_backend->lock);
  /* this is essentially: caches_invalidate_dir (gphoto2_backend, dir); */
  g_hash_table_remove (gphoto2_backend->dir_name_cache, dir);
  g_hash_table_remove (gphoto2_backend->file_name_cache, dir);
  g_hash_table_remove (gphoto2_backend->info_cache, dir);

  g_hash_table_remove (gphoto2_backend->info_cache, full_name);
  g_mutex_unlock (gphoto2_backend->lock);

  DEBUG ("caches_invalidate_file() for '%s'", full_name);
  g_free (full_name);
}

/* ------------------------------------------------------------------------------------------------- */

static GError *
get_error_from_gphoto2 (const char *message, int rc)
{
  GError *error;

  switch (rc)
    {
    case GP_ERROR_FILE_EXISTS:
    case GP_ERROR_DIRECTORY_EXISTS:
      /* Translator: %s represents a more specific error message and %d the specific error code */
      error = g_error_new (G_IO_ERROR, 
                           G_IO_ERROR_EXISTS, _("%s: %d: Directory or file exists"), message, rc);
      break;

    case GP_ERROR_FILE_NOT_FOUND:
    case GP_ERROR_DIRECTORY_NOT_FOUND:
      /* Translator: %s represents a more specific error message and %d the specific error code */
      error = g_error_new (G_IO_ERROR, 
                           G_IO_ERROR_NOT_FOUND, _("%s: %d: No such file or directory"), message, rc);
      break;

    case GP_ERROR_PATH_NOT_ABSOLUTE:
      /* Translator: %s represents a more specific error message and %d the specific error code */
      error = g_error_new (G_IO_ERROR, 
                           G_IO_ERROR_INVALID_FILENAME, _("%s: %d: Invalid filename"), message, rc);
      break;

    case GP_ERROR_NOT_SUPPORTED:
      /* Translator: %s represents a more specific error message and %d the specific error code */
      error = g_error_new (G_IO_ERROR, 
                           G_IO_ERROR_NOT_SUPPORTED, _("%s: %d: Not Supported"), message, rc);
      break;

    default:
      error = g_error_new (G_IO_ERROR, 
                           G_IO_ERROR_FAILED, "%s: %d: %s", message, rc, gp_result_as_string (rc));
      break;
    }
  return error;
}

/* ------------------------------------------------------------------------------------------------- */

static void
release_device (GVfsBackendGphoto2 *gphoto2_backend)
{
  GList *l;

  g_free (gphoto2_backend->gphoto2_port);
  gphoto2_backend->gphoto2_port = NULL;

  if (gphoto2_backend->context != NULL)
    {
      gp_context_unref (gphoto2_backend->context);
      gphoto2_backend->context = NULL;
    }

  if (gphoto2_backend->camera != NULL)
    {
      gp_camera_unref (gphoto2_backend->camera);
      gphoto2_backend->camera = NULL;
    }

#ifdef HAVE_GUDEV
  if (gphoto2_backend->gudev_client != NULL)
    g_object_unref (gphoto2_backend->gudev_client);
  if (gphoto2_backend->udev_device != NULL)
    g_object_unref (gphoto2_backend->udev_device);

#elif defined(HAVE_HAL)
  if (gphoto2_backend->dbus_connection != NULL)
    {
      dbus_connection_close (gphoto2_backend->dbus_connection);
      dbus_connection_unref (gphoto2_backend->dbus_connection);
      gphoto2_backend->dbus_connection = NULL;
    }

  if (gphoto2_backend->hal_ctx != NULL)
    {
      libhal_ctx_free (gphoto2_backend->hal_ctx);
      gphoto2_backend->hal_ctx = NULL;

    }
  g_free (gphoto2_backend->hal_udi);
  gphoto2_backend->hal_udi = NULL;
  g_free (gphoto2_backend->hal_name);
  gphoto2_backend->hal_name = NULL;
#endif
  g_free (gphoto2_backend->icon_name);
  gphoto2_backend->icon_name = NULL;

  g_free (gphoto2_backend->ignore_prefix);
  gphoto2_backend->ignore_prefix = NULL;

  if (gphoto2_backend->info_cache != NULL)
    {
      g_hash_table_unref (gphoto2_backend->info_cache);
      gphoto2_backend->info_cache = NULL;
    }
  if (gphoto2_backend->dir_name_cache != NULL)
    {
      g_hash_table_unref (gphoto2_backend->dir_name_cache);
      gphoto2_backend->dir_name_cache = NULL;
    }
  if (gphoto2_backend->file_name_cache != NULL)
    {
      g_hash_table_unref (gphoto2_backend->file_name_cache);
      gphoto2_backend->file_name_cache = NULL;
    }

  for (l = gphoto2_backend->dir_monitor_proxies; l != NULL; l = l->next)
    {
      MonitorProxy *proxy = l->data;
      monitor_proxy_free (proxy);
    }
  g_list_free (gphoto2_backend->dir_monitor_proxies);
  gphoto2_backend->dir_monitor_proxies = NULL;

  for (l = gphoto2_backend->file_monitor_proxies; l != NULL; l = l->next)
    {
      MonitorProxy *proxy = l->data;
      monitor_proxy_free (proxy);
    }
  g_list_free (gphoto2_backend->file_monitor_proxies);
  gphoto2_backend->file_monitor_proxies = NULL;

  if (gphoto2_backend->lock != NULL)
    {
      g_mutex_free (gphoto2_backend->lock);
      gphoto2_backend->lock = NULL;
    }
  gphoto2_backend->capacity = -1;
  gphoto2_backend->free_space = -1;
}

/* ------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_gphoto2_finalize (GObject *object)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (object);

  DEBUG ("finalizing %p", object);

  release_device (gphoto2_backend);

  if (G_OBJECT_CLASS (g_vfs_backend_gphoto2_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_gphoto2_parent_class)->finalize) (object);
}

/* ------------------------------------------------------------------------------------------------- */

#ifdef DEBUG_SHOW_LIBGPHOTO2_OUTPUT
static void
_gphoto2_logger_func (GPLogLevel level, const char *domain, const char *format, va_list args, void *data)
{
  g_fprintf (stderr, "libgphoto2: %s: ", domain);
  g_vfprintf (stderr, format, args);
  va_end (args);
  g_fprintf (stderr, "\n");
}
#endif

static void
g_vfs_backend_gphoto2_init (GVfsBackendGphoto2 *gphoto2_backend)
{
  GVfsBackend *backend = G_VFS_BACKEND (gphoto2_backend);
  GMountSpec *mount_spec;

  DEBUG ("initing %p", gphoto2_backend);

  g_vfs_backend_set_display_name (backend, "gphoto2");

  mount_spec = g_mount_spec_new ("gphoto2");
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  g_mount_spec_unref (mount_spec);

#ifdef DEBUG_SHOW_LIBGPHOTO2_OUTPUT
  gp_log_add_func (GP_LOG_ALL, _gphoto2_logger_func, NULL);
#endif
}

/* ------------------------------------------------------------------------------------------------- */

static char *
compute_icon_name (GVfsBackendGphoto2 *gphoto2_backend)
{
  char *result;

  if (gphoto2_backend->icon_name == NULL)
    {
      result = g_strdup_printf ("camera-photo");
    }
  else
    {
      result = g_strdup (gphoto2_backend->icon_name);
    }

  return result;
}

/* ------------------------------------------------------------------------------------------------- */

static char *
compute_display_name (GVfsBackendGphoto2 *gphoto2_backend)
{
  char *result = NULL;

#ifdef HAVE_GUDEV
  const char *s;

  /* the real "nice" and user-visible name is computed in the monitor; just try
   * using the product name here */
  if (gphoto2_backend->udev_device != NULL)
    {
      s = g_udev_device_get_sysfs_attr (gphoto2_backend->udev_device, "product");
      if (s == NULL)
        s = g_udev_device_get_property (gphoto2_backend->udev_device, "ID_MODEL");

      if (s != NULL)
        result = g_strdup (s);
    }
  if (result == NULL )
    {
      /* Translator: %s represents the device, e.g. usb:001,042  */
      result = g_strdup_printf (_("Digital Camera (%s)"), gphoto2_backend->gphoto2_port);
    }
#elif defined(HAVE_HAL)
  if (gphoto2_backend->hal_name == NULL)
    {
      /* Translator: %s represents the device, e.g. usb:001,042  */
      result = g_strdup_printf (_("Digital Camera (%s)"), gphoto2_backend->gphoto2_port);
    }
  else
    {
      result = g_strdup (gphoto2_backend->hal_name);
    }
#endif

  return result;
}

/* ------------------------------------------------------------------------------------------------- */

#ifdef HAVE_GUDEV
static void
setup_for_device (GVfsBackendGphoto2 *gphoto2_backend)
{
  gchar *devname;
  char *comma;
  gboolean is_media_player = FALSE;
  char *camera_x_content_types[] = {"x-content/image-dcf", NULL};
  char *media_player_x_content_types[] = {"x-content/audio-player", NULL};

  /* turn usb:001,041 string into an udev device name */
  if (!g_str_has_prefix (gphoto2_backend->gphoto2_port, "usb:"))
    return;
  devname = g_strconcat ("/dev/bus/usb/", gphoto2_backend->gphoto2_port+4, NULL);
  if ((comma = strchr (devname, ',')) == NULL)
    {
      g_free (devname);
      return;
    }
  *comma = '/';
  DEBUG ("Parsed '%s' into device name %s", gphoto2_backend->gphoto2_port, devname);

  /* find corresponding GUdevDevice */
  gphoto2_backend->udev_device = g_udev_client_query_by_device_file (gphoto2_backend->gudev_client, devname);
  g_free (devname);
  if (gphoto2_backend->udev_device)
    {
      DEBUG ("-> sysfs path %s, subsys %s, name %s", g_udev_device_get_sysfs_path (gphoto2_backend->udev_device), g_udev_device_get_subsystem (gphoto2_backend->udev_device), g_udev_device_get_name (gphoto2_backend->udev_device));

      /* determine icon name */
      if (g_udev_device_has_property (gphoto2_backend->udev_device, "ID_MEDIA_PLAYER_ICON_NAME"))
	{
          gphoto2_backend->icon_name = g_strdup (g_udev_device_get_property (gphoto2_backend->udev_device, "ID_MEDIA_PLAYER_ICON_NAME"));
	  is_media_player = TRUE;
	}
      else if (g_udev_device_has_property (gphoto2_backend->udev_device, "ID_MEDIA_PLAYER"))
	{
          gphoto2_backend->icon_name = g_strdup ("multimedia-player");
	  is_media_player = TRUE;
	}
      else
          gphoto2_backend->icon_name = g_strdup ("camera-photo");
    }
  else
      DEBUG ("-> did not find matching udev device");

  if (is_media_player)
      g_vfs_backend_set_x_content_types (G_VFS_BACKEND (gphoto2_backend), media_player_x_content_types);
  else
      g_vfs_backend_set_x_content_types (G_VFS_BACKEND (gphoto2_backend), camera_x_content_types);
}

static void
on_uevent (GUdevClient *client, gchar *action, GUdevDevice *device, gpointer user_data)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (user_data);

  DEBUG ("on_uevent action %s, device %s", action, g_udev_device_get_device_file (device));

  if (gphoto2_backend->udev_device != NULL && 
      g_strcmp0 (g_udev_device_get_device_file (gphoto2_backend->udev_device), g_udev_device_get_device_file (device)) == 0 &&
      strcmp (action, "remove") == 0)
    {
      DEBUG ("we have been removed!");

      /* nuke all caches so we're a bit more valgrind friendly */
      caches_invalidate_all (gphoto2_backend);

      /* TODO: need a cleaner way to force unmount ourselves */
      exit (1);
    }
}

#elif defined(HAVE_HAL)
static void
find_udi_for_device (GVfsBackendGphoto2 *gphoto2_backend)
{
  int num_camera_devices;
  int num_mtp_devices;
  int num_devices;
  char **camera_devices;
  char **mtp_devices;
  char **devices;
  int n, m;
  int usb_bus_num;
  int usb_device_num;
  char **tokens;
  char *endp;
  char *camera_x_content_types[] = {"x-content/image-dcf", NULL};
  char *music_player_x_content_types[] = {"x-content/audio-player", NULL};

  gphoto2_backend->hal_udi = NULL;

  /* parse the usb:001,041 string */

  if (!g_str_has_prefix (gphoto2_backend->gphoto2_port, "usb:"))
    {
      return;
    }

  tokens = g_strsplit (gphoto2_backend->gphoto2_port + 4, ",", 0);
  if (g_strv_length (tokens) != 2)
    {
      g_strfreev (tokens);
      return;
    }

  usb_bus_num = strtol (tokens[0], &endp, 10);
  if (*endp != '\0')
    {
      g_strfreev (tokens);
      return;
    }

  usb_device_num = strtol (tokens[1], &endp, 10);
  if (*endp != '\0')
    {
      g_strfreev (tokens);
      return;
    }

  g_strfreev (tokens);

  DEBUG ("Parsed '%s' into bus=%d device=%d", gphoto2_backend->gphoto2_port, usb_bus_num, usb_device_num);

  camera_devices = libhal_find_device_by_capability (gphoto2_backend->hal_ctx,
                                                     "camera",
                                                     &num_camera_devices,
                                                     NULL);
  mtp_devices = libhal_find_device_by_capability (gphoto2_backend->hal_ctx,
                                                  "portable_audio_player",
                                                  &num_mtp_devices,
                                                  NULL);
  for (m = 0; m < 2 && gphoto2_backend->hal_udi == NULL; m++)
    {
      devices = m == 0 ? camera_devices : mtp_devices;
      num_devices = m == 0 ? num_camera_devices : num_mtp_devices;

      if (devices != NULL)
        {
          for (n = 0; n < num_devices && gphoto2_backend->hal_udi == NULL; n++)
            {
              char *udi = devices[n];
              LibHalPropertySet *ps;
              
              ps = libhal_device_get_all_properties (gphoto2_backend->hal_ctx, udi, NULL);
              if (ps != NULL)
                {
                  const char *subsystem;
              
                  subsystem = libhal_ps_get_string (ps, "info.subsystem");
                  if (subsystem != NULL && strcmp (subsystem, "usb") == 0)
                    {
                      int device_usb_bus_num;
                      int device_usb_device_num;
                      const char *icon_from_hal;
                      const char *name_from_hal;
                      
                      device_usb_bus_num = libhal_ps_get_int32 (ps, "usb.bus_number");
                      device_usb_device_num = libhal_ps_get_int32 (ps, "usb.linux.device_number");
                      icon_from_hal = libhal_ps_get_string (ps, "info.desktop.icon");
                      name_from_hal = libhal_ps_get_string (ps, "info.desktop.name");
                      
                      DEBUG ("looking at usb device '%s' with bus=%d, device=%d", 
                             udi, device_usb_bus_num, device_usb_device_num);
                      
                      if (device_usb_bus_num == usb_bus_num && 
                          device_usb_device_num == usb_device_num)
                        {
                          char *name;
                          const char *parent_udi;
                          LibHalPropertySet *ps2;

                          DEBUG ("udi '%s' is the one!", udi);
                          
                          /* IMPORTANT: 
                           * 
                           * Keep this naming code in sync with
                           *
                           *   hal/ghalvolume;do_update_from_hal_for_camera() 
                           */
                          name = NULL;
                          parent_udi = libhal_ps_get_string (ps, "info.parent");
                          if (name_from_hal != NULL)
                            {
                              name = g_strdup (name_from_hal);
                            }
                          else if (parent_udi != NULL)
                            {
                              ps2 = libhal_device_get_all_properties (gphoto2_backend->hal_ctx, parent_udi, NULL);
                              if (ps2 != NULL)
                                {
                                  const char *vendor;
                                  const char *product;
                                  
                                  vendor = libhal_ps_get_string (ps2, "usb_device.vendor");
                                  product = libhal_ps_get_string (ps2, "usb_device.product");
                                  if (vendor == NULL)
                                    {
                                      if (product != NULL)
                                        name = g_strdup (product);
                                    }
                                  else
                                    {
                                      if (product != NULL)
                                        name = g_strdup_printf ("%s %s", vendor, product);
                                      else
                                        {
                                          if (m == 0)
                                            /* Translator: %s is the vendor name, e.g. Panasonic */
                                            name = g_strdup_printf (_("%s Camera"), vendor);
                                          else
                                            /* Translator: %s is the vendor name, e.g. Panasonic */
                                            name = g_strdup_printf (_("%s Audio Player"), vendor);
                                        }
                                    }
                                  libhal_free_property_set (ps2);
                                }
                            }
                          if (name == NULL)
                            {
                              if (m == 0)
                                name = g_strdup (_("Camera"));
                              else
                                name = g_strdup (_("Audio Player"));
                            }
                          
                          gphoto2_backend->hal_udi = g_strdup (udi);
                          gphoto2_backend->hal_name = name;
                          if (icon_from_hal != NULL)
                            {
                              gphoto2_backend->icon_name = g_strdup (icon_from_hal);
                            }
                          else
                            {
                              if (m == 1)
                                {
                                  gphoto2_backend->icon_name = g_strdup ("multimedia-player");
                                }
                              else
                                {
                                  gphoto2_backend->icon_name = g_strdup ("camera-photo");
                                }
                            }

                          /* TODO: should we sniff the files instead? */
                          if (m == 0)
                            {
                              g_vfs_backend_set_x_content_types (G_VFS_BACKEND (gphoto2_backend),
                                                                 camera_x_content_types);
                            }
                          else
                            {
                              g_vfs_backend_set_x_content_types (G_VFS_BACKEND (gphoto2_backend),
                                                                 music_player_x_content_types);
                            }

                        }
                      
                    }
                  
                  libhal_free_property_set (ps);
                }
            }
          libhal_free_string_array (devices);
        }
    }
}

/* ------------------------------------------------------------------------------------------------- */

static void
_hal_device_removed (LibHalContext *hal_ctx, const char *udi)
{
  GVfsBackendGphoto2 *gphoto2_backend;

  gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (libhal_ctx_get_user_data (hal_ctx));

  if (gphoto2_backend->hal_udi != NULL && strcmp (udi, gphoto2_backend->hal_udi) == 0)
    {
      DEBUG ("we have been removed!");

      /* nuke all caches so we're a bit more valgrind friendly */
      caches_invalidate_all (gphoto2_backend);

      /* TODO: need a cleaner way to force unmount ourselves */
      exit (1);
    }
}
#endif

/* ------------------------------------------------------------------------------------------------- */

static void
split_filename_with_ignore_prefix (GVfsBackendGphoto2 *gphoto2_backend, const char *filename, char **dir, char **name)
{
  char *s;

  s = g_path_get_dirname (filename);
  if (s[0] == '/')
    *dir = g_strconcat (gphoto2_backend->ignore_prefix, s + 1, NULL);
  else
    *dir = g_strconcat (gphoto2_backend->ignore_prefix, s, NULL);
  g_free (s);

  if (strcmp (filename, "/") == 0)
    *name = g_strdup ("");
  else
    *name = g_path_get_basename (filename);

  s = *dir;
  if (s[strlen(s)] == '/')
    s[strlen(s)] = '\0';

  /*DEBUG ("split_filename_with_ignore_prefix: '%s' -> '%s' '%s'", filename, *dir, *name);*/
}

/* ------------------------------------------------------------------------------------------------- */

static char *
add_ignore_prefix (GVfsBackendGphoto2 *gphoto2_backend, const char *filename)
{
  char *result;

  if (filename[0] == '/')
    result = g_strconcat (gphoto2_backend->ignore_prefix, filename + 1, NULL);
  else
    result = g_strconcat (gphoto2_backend->ignore_prefix, filename, NULL);

  /*DEBUG ("add_ignore_prefix: '%s' -> '%s'", filename, result);*/
  return result;
}

/* ------------------------------------------------------------------------------------------------- */

/* the passed 'dir' variable must contain ignore_prefix */
static gboolean
file_get_info (GVfsBackendGphoto2 *gphoto2_backend, 
               const char *dir, 
               const char *name, 
               GFileInfo *info, 
               GError **error,
               gboolean try_cache_only)
{
  int rc;
  gboolean ret;
  CameraFileInfo gp_info;
  char *full_path;
  GFileInfo *cached_info;
  GTimeVal mtime;
  char *mime_type;
  GIcon *icon;
  unsigned int n;

  ret = FALSE;

  full_path = g_build_filename (dir, name, NULL);
  DEBUG ("file_get_info() try_cache_only=%d dir='%s', name='%s'\n"
         "                full_path='%s'", 
         try_cache_only, dir, name, full_path, gphoto2_backend->ignore_prefix);


  /* first look up cache */
  g_mutex_lock (gphoto2_backend->lock);
  cached_info = g_hash_table_lookup (gphoto2_backend->info_cache, full_path);
  if (cached_info != NULL)
    {
      g_file_info_copy_into (cached_info, info);
      g_mutex_unlock (gphoto2_backend->lock);
      DEBUG ("  Using cached info %p for '%s'", cached_info, full_path);
      ret = TRUE;
      goto out;
    }
  g_mutex_unlock (gphoto2_backend->lock);

  if (try_cache_only)
    goto out;

  ensure_not_dirty (gphoto2_backend);

  DEBUG ("  No cached info for '%s'", full_path);

  /* Since we're caching stuff, make sure all information we store is set */
  g_file_info_unset_attribute_mask (info);

  /* handle root directory */
  if (strcmp (full_path, gphoto2_backend->ignore_prefix) == 0 || strcmp (full_path, "/") == 0)
    {
      char *display_name;
      display_name = compute_display_name (gphoto2_backend);
      g_file_info_set_display_name (info, display_name);
      g_file_info_set_name (info, display_name);
      g_free (display_name);
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_content_type (info, "inode/directory");
      g_file_info_set_size (info, 0);
      icon = g_themed_icon_new ("folder");
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, gphoto2_backend->can_write);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, gphoto2_backend->can_delete);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE); 
      ret = TRUE;
      DEBUG ("  Generating info (root folder) for '%s'", full_path);
      goto add_to_cache;
    }

  rc = gp_camera_file_get_info (gphoto2_backend->camera,
                                dir,
                                name,
                                &gp_info,
                                gphoto2_backend->context);
  if (rc != 0)
    {
      CameraList *list;
      gboolean is_folder;

      /* gphoto2 doesn't know about this file.. it may be a folder; try that */
      is_folder = FALSE;
      gp_list_new (&list);
      rc = gp_camera_folder_list_folders (gphoto2_backend->camera, 
                                        dir, 
                                        list, 
                                        gphoto2_backend->context);
      if (rc == 0)
        {
          for (n = 0; n < gp_list_count (list) && !is_folder; n++)
            {
              const char *folder_name;
              gp_list_get_name (list, n, &folder_name);
              if (strcmp (folder_name, name) == 0)
                {
                  is_folder = TRUE;
                }
            }
        }
      gp_list_free (list);

      if (is_folder)
        {
          g_file_info_set_name (info, name);
          g_file_info_set_display_name (info, name);
          icon = g_themed_icon_new ("folder");
          g_file_info_set_icon (info, icon);
          g_object_unref (icon);
          g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
          g_file_info_set_content_type (info, "inode/directory");
          g_file_info_set_size (info, 0);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, gphoto2_backend->can_write);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, gphoto2_backend->can_delete);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, gphoto2_backend->can_write); 
          g_file_info_set_is_hidden (info, name != NULL && name[0] == '.');
          ret = TRUE;
          DEBUG ("  Generating info (folder) for '%s'", full_path);
          goto add_to_cache;
        }

      /* nope, not a folder either.. error out.. */
      if (error != NULL)
        {
          *error = g_error_new (G_IO_ERROR,
                                G_IO_ERROR_NOT_FOUND,
                                _("No such file or directory"));
        }
      goto out;
    }

  g_file_info_set_name (info, name);
  g_file_info_set_display_name (info, name);
  g_file_info_set_file_type (info, G_FILE_TYPE_REGULAR);

  if (gp_info.file.fields & GP_FILE_INFO_SIZE)
    {
      g_file_info_set_size (info, gp_info.file.size);
    }
  else
    {
      /* not really sure this is the right thing to do... */
      g_file_info_set_size (info, 0);
    }

  /* TODO: We really should sniff the file / look at file extensions
   * instead of relying on gp_info.file.type... but sniffing the file
   * is no fun since we (currently) can't do partial reads with the
   * libgphoto2 API :-/
   */
  mime_type = NULL;
  if (gp_info.file.fields & GP_FILE_INFO_TYPE)
    {
      /* application/x-unknown is a bogus mime type return by some
       * devices (such as Sandisk Sansa music players) - ignore it.
       */
      if (strcmp (gp_info.file.type, "application/x-unknown") != 0)
        {
          mime_type = g_strdup (gp_info.file.type);
        }
    }
  if (mime_type == NULL)
    mime_type = g_content_type_guess (name, NULL, 0, NULL);
  if (mime_type == NULL)
    mime_type = g_strdup ("application/octet-stream");
  g_file_info_set_content_type (info, mime_type);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, mime_type);

  /* we offer thumbnails for both pics and video (see bgo #585853) */
  if (g_str_has_prefix (mime_type, "image") || g_str_has_prefix (mime_type, "video"))
    {
      char *icon_id;
      GIcon *icon;
      GMountSpec *mount_spec;

      mount_spec = g_vfs_backend_get_mount_spec (G_VFS_BACKEND (gphoto2_backend));
      icon_id = g_strdup_printf ("preview:%s/%s", dir + strlen (gphoto2_backend->ignore_prefix), name);
      icon = g_vfs_icon_new (mount_spec,
                             icon_id);
      g_file_info_set_attribute_object (info,
                                        G_FILE_ATTRIBUTE_PREVIEW_ICON,
                                        G_OBJECT (icon));
      g_object_unref (icon);
      g_free (icon_id);
    }

  icon = g_content_type_get_icon (mime_type);
  DEBUG ("  got icon %p for mime_type '%s'", icon, mime_type);
  if (icon != NULL)
    {
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
    }
  g_free (mime_type);

  if (gp_info.file.fields & GP_FILE_INFO_MTIME)
    mtime.tv_sec = gp_info.file.mtime;
  else
    mtime.tv_sec = 0;
  mtime.tv_usec = 0;
  g_file_info_set_modification_time (info, &mtime);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, gphoto2_backend->can_write);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, gphoto2_backend->can_delete);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, gphoto2_backend->can_write); 
  g_file_info_set_is_hidden (info, name != NULL && name[0] == '.');

  if (gp_info.file.fields & GP_FILE_INFO_PERMISSIONS) {
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, 
                                       gp_info.file.permissions & GP_FILE_PERM_DELETE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, 
                                       gp_info.file.permissions & GP_FILE_PERM_DELETE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, 
                                       gp_info.file.permissions & GP_FILE_PERM_DELETE);
  }

  ret = TRUE;
  DEBUG ("  Generating info (file) for '%s'", full_path);

 add_to_cache:
  /* add this sucker to the cache */
  if (ret == TRUE)
    {
#ifndef DEBUG_NO_CACHING
      cached_info = g_file_info_dup (info);
      DEBUG ("  Storing cached info %p for '%s'", cached_info, full_path);
      g_mutex_lock (gphoto2_backend->lock);
      g_hash_table_insert (gphoto2_backend->info_cache, g_strdup (full_path), cached_info);
      g_mutex_unlock (gphoto2_backend->lock);
#endif
    }

 out:
  g_free (full_path);
  return ret;
}

/* ------------------------------------------------------------------------------------------------- */

static gboolean
is_directory (GVfsBackendGphoto2 *gphoto2_backend, const char *dir, const char *name)
{
  GFileInfo *info;
  gboolean ret;

  ret = FALSE;

  info = g_file_info_new ();
  if (!file_get_info (gphoto2_backend, dir, name, info, NULL, FALSE))
    goto out;

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    ret = TRUE;

 out:
  g_object_unref (info);
  return ret;
}

/* ------------------------------------------------------------------------------------------------- */

static gboolean 
is_regular (GVfsBackendGphoto2 *gphoto2_backend, const char *dir, const char *name)
{
  GFileInfo *info;
  gboolean ret;

  ret = FALSE;

  info = g_file_info_new ();
  if (!file_get_info (gphoto2_backend, dir, name, info, NULL, FALSE))
    goto out;

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR)
    ret = TRUE;

 out:
  g_object_unref (info);
  return ret;
}

/* ------------------------------------------------------------------------------------------------- */

static gboolean
is_directory_empty (GVfsBackendGphoto2 *gphoto2_backend, const char *dir)
{
  CameraList *list;
  gboolean ret;
  int rc;
  int num_dirs;
  int num_files;
  
  DEBUG ("is_directory_empty begin (%s)", dir);
  
  /* TODO: use cache */
  
  ret = FALSE;
  num_dirs = 0;
  num_files = 0;
  
  gp_list_new (&list);
  rc = gp_camera_folder_list_files (gphoto2_backend->camera, 
                                    dir, 
                                    list, 
                                    gphoto2_backend->context);
  if (rc == 0)
    num_files = gp_list_count (list);
  gp_list_free (list);
  
  if (num_files > 0)
    goto out;
  
  gp_list_new (&list);
  rc = gp_camera_folder_list_folders (gphoto2_backend->camera, 
                                      dir, 
                                      list, 
                                      gphoto2_backend->context);
  if (rc == 0)
    num_dirs = gp_list_count (list);
  gp_list_free (list);
  
  if (num_dirs == 0 && num_files == 0)
    ret = TRUE;
  
 out:
  DEBUG ("  is_directory_empty (%s) -> %d", dir, ret);
  return ret;
}

/* ------------------------------------------------------------------------------------------------- */

/* If we only have a single storage head, the gphoto2 volume monitor
 * will not use activation roots into our mount. This is mainly to
 * work around buggy devices where the basedir of the storage head
 * changes on every camera initialization, e.g. the iPhone.
 *
 * So, if we have only one storage head, do use basedir of that
 * head as ignore_prefix.
 *
 * See also update_cameras() in ggphoto2volumemonitor.c.
 *
 * This function needs to be called from do_mount().
 */
static gboolean
ensure_ignore_prefix (GVfsBackendGphoto2 *gphoto2_backend, GVfsJob *job)
{
  gchar *prefix;
  CameraStorageInformation *storage_info, *head;
  int num_storage_info, i;

  /* already set */
  if (gphoto2_backend->ignore_prefix != NULL)
    return TRUE;

  prefix = NULL;

  if (gp_camera_get_storageinfo (gphoto2_backend->camera,
                                 &storage_info,
                                 &num_storage_info,
                                 gphoto2_backend->context) != 0)
    goto out;

  head = NULL;
  for (i = 0; i < num_storage_info; i++)
    {
      /* Ignore storage with no capacity (see bug 570888) */
      if ((storage_info[i].fields & GP_STORAGEINFO_MAXCAPACITY) &&
          storage_info[i].capacitykbytes == 0)
        continue;
      
      /* Multiple heads, don't ignore */
      if (head != NULL)
	goto out;
	
      head = &storage_info[i];
    }

  /* Some cameras, such as the Canon 5D, won't report the basedir */
  if (head->fields & GP_STORAGEINFO_BASE)
    prefix = g_strdup_printf ("%s/", head->basedir);

 out:

  if (prefix == NULL)
    gphoto2_backend->ignore_prefix = g_strdup ("/");
  else
    gphoto2_backend->ignore_prefix = prefix;

  DEBUG ("Using ignore_prefix='%s'", gphoto2_backend->ignore_prefix);

  return TRUE;
}

/* ------------------------------------------------------------------------------------------------- */

static void
do_mount (GVfsBackend *backend,
	  GVfsJobMount *job,
	  GMountSpec *mount_spec,
	  GMountSource *mount_source,
	  gboolean is_automount)
{
  char *fuse_name;
  char *display_name;
  char *icon_name;
  const char *host;
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  GError *error = NULL;
  GMountSpec *gphoto2_mount_spec;
  int rc;
  GPPortInfo info;
  GPPortInfoList *il = NULL;
  int n;
  CameraStorageInformation *storage_info;
  int num_storage_info;

  DEBUG ("do_mount %p", gphoto2_backend);

#ifdef HAVE_GUDEV
  /* setup gudev */
  const char *subsystems[] = {"usb", NULL};

  gphoto2_backend->gudev_client = g_udev_client_new (subsystems);
  if (gphoto2_backend->gudev_client == NULL)
    {
      release_device (gphoto2_backend);
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Cannot create gudev client"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  g_signal_connect (gphoto2_backend->gudev_client, "uevent", G_CALLBACK (on_uevent), gphoto2_backend);

#elif defined(HAVE_HAL)
  /* setup libhal */
  DBusError dbus_error;

  dbus_error_init (&dbus_error);
  gphoto2_backend->dbus_connection = dbus_bus_get_private (DBUS_BUS_SYSTEM, &dbus_error);
  if (dbus_error_is_set (&dbus_error))
    {
      release_device (gphoto2_backend);
      dbus_error_free (&dbus_error);
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Cannot connect to the system bus"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }
  
  dbus_connection_set_exit_on_disconnect (gphoto2_backend->dbus_connection, FALSE);

  gphoto2_backend->hal_ctx = libhal_ctx_new ();
  if (gphoto2_backend->hal_ctx == NULL)
    {
      release_device (gphoto2_backend);
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Cannot create libhal context"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  _g_dbus_connection_integrate_with_main (gphoto2_backend->dbus_connection);
  libhal_ctx_set_dbus_connection (gphoto2_backend->hal_ctx, gphoto2_backend->dbus_connection);
  
  if (!libhal_ctx_init (gphoto2_backend->hal_ctx, &dbus_error))
    {
      release_device (gphoto2_backend);
      dbus_error_free (&dbus_error);
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Cannot initialize libhal"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  libhal_ctx_set_device_removed (gphoto2_backend->hal_ctx, _hal_device_removed);
  libhal_ctx_set_user_data (gphoto2_backend->hal_ctx, gphoto2_backend);
#endif

  /* setup gphoto2 */

  host = g_mount_spec_get (mount_spec, "host");
  DEBUG ("  host='%s'", host);
  if (host == NULL || strlen (host) < 3 || host[0] != '[' || host[strlen (host) - 1] != ']')
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _("No device specified"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }

  gphoto2_backend->gphoto2_port = g_strdup (host + 1);
  gphoto2_backend->gphoto2_port[strlen (gphoto2_backend->gphoto2_port) - 1] = '\0';

  DEBUG ("  decoded host='%s'", gphoto2_backend->gphoto2_port);

#ifdef HAVE_GUDEV
  setup_for_device (gphoto2_backend);
#elif defined(HAVE_HAL)
  find_udi_for_device (gphoto2_backend);
#endif

  gphoto2_backend->context = gp_context_new ();
  if (gphoto2_backend->context == NULL)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Cannot create gphoto2 context"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }

  rc = gp_camera_new (&(gphoto2_backend->camera));
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_("Error creating camera"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }


  il = NULL;
  
  rc = gp_port_info_list_new (&il);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_("Error loading device information"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }

  rc = gp_port_info_list_load (il);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_("Error loading device information"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }

  DEBUG ("  gphoto2_port='%s'", gphoto2_backend->gphoto2_port);

  n = gp_port_info_list_lookup_path (il, gphoto2_backend->gphoto2_port);
  if (n == GP_ERROR_UNKNOWN_PORT)
    {
      error = get_error_from_gphoto2 (_("Error looking up device information"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }

  rc = gp_port_info_list_get_info (il, n, &info);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_("Error getting device information"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }

  DEBUG ("  '%s' '%s' '%s'",  info.name, info.path, info.library_filename);
  
  /* set port */
  rc = gp_camera_set_port_info (gphoto2_backend->camera, info);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_("Error setting up camera communications port"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }
  gp_port_info_list_free(il);

  rc = gp_camera_init (gphoto2_backend->camera, gphoto2_backend->context);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_("Error initializing camera"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }

  if (!ensure_ignore_prefix (gphoto2_backend, G_VFS_JOB (job)))
    {
      release_device (gphoto2_backend);
      return;
    }

  /* Translator: %s represents the device, e.g. usb:001,042  */
  fuse_name = g_strdup_printf (_("gphoto2 mount on %s"), gphoto2_backend->gphoto2_port);
  icon_name = compute_icon_name (gphoto2_backend);
  display_name = compute_display_name (gphoto2_backend);
  g_vfs_backend_set_stable_name (backend, fuse_name);
  g_vfs_backend_set_display_name (backend, display_name);
  g_vfs_backend_set_icon_name (backend, icon_name);
  g_free (display_name);
  g_free (icon_name);
  g_free (fuse_name);

  gphoto2_backend->can_write = FALSE;
  gphoto2_backend->can_delete = FALSE;
  rc = gp_camera_get_storageinfo (gphoto2_backend->camera, &storage_info, &num_storage_info, gphoto2_backend->context);
  if (rc == 0)
    {
      if (num_storage_info >= 1)
        {
          if (storage_info[0].fields & GP_STORAGEINFO_ACCESS && storage_info[0].access == GP_STORAGEINFO_AC_READWRITE)
            {
              gphoto2_backend->can_write = TRUE;
              gphoto2_backend->can_delete = TRUE;
            }
          if (storage_info[0].fields & GP_STORAGEINFO_ACCESS && storage_info[0].access == GP_STORAGEINFO_AC_READONLY_WITH_DELETE)
            {
              gphoto2_backend->can_delete = TRUE;
            }
        }
    }
  DEBUG ("  can_write = %d", gphoto2_backend->can_write);
  DEBUG ("  can_delete = %d", gphoto2_backend->can_delete);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  gphoto2_backend->free_space = -1;

  gphoto2_backend->lock = g_mutex_new ();

  gphoto2_mount_spec = g_mount_spec_new ("gphoto2");
  g_mount_spec_set (gphoto2_mount_spec, "host", host);
  g_vfs_backend_set_mount_spec (backend, gphoto2_mount_spec);
  g_mount_spec_unref (gphoto2_mount_spec);

  gphoto2_backend->info_cache = g_hash_table_new_full (g_str_hash,
                                                       g_str_equal,
                                                       g_free,
                                                       g_object_unref);

  gphoto2_backend->dir_name_cache = g_hash_table_new_full (g_str_hash,
                                                           g_str_equal,
                                                           g_free,
                                                           (GDestroyNotify) gp_list_unref);

  gphoto2_backend->file_name_cache = g_hash_table_new_full (g_str_hash,
                                                            g_str_equal,
                                                            g_free,
                                                            (GDestroyNotify) gp_list_unref);

  DEBUG ("  mounted %p", gphoto2_backend);
}

/* ------------------------------------------------------------------------------------------------- */

static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  const char *host;
  GError *error = NULL;
  GMountSpec *gphoto2_mount_spec;

  DEBUG ("try_mount %p", backend);

  /* TODO: Hmm.. apparently we have to set the mount spec in
   * try_mount(); doing it in mount() do_won't work.. 
   */
  host = g_mount_spec_get (mount_spec, "host");
  DEBUG ("  host=%s", host);
  if (host == NULL)
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _("No camera specified"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return TRUE;
    }

  gphoto2_mount_spec = g_mount_spec_new ("gphoto2");
  g_mount_spec_set (gphoto2_mount_spec, "host", host);
  g_vfs_backend_set_mount_spec (backend, gphoto2_mount_spec);
  g_mount_spec_unref (gphoto2_mount_spec);
  return FALSE;
}

/* ------------------------------------------------------------------------------------------------- */

static void
unmount_with_op_cb (GVfsBackend  *backend,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  GVfsJobUnmount *job = G_VFS_JOB_UNMOUNT (user_data);
  gboolean should_unmount;

  DEBUG ("In unmount_with_op_cb");

  should_unmount = g_vfs_backend_unmount_with_operation_finish (backend,
                                                                res);

  DEBUG ("should_unmount=%d", should_unmount);

  if (should_unmount)
    {

      DEBUG ("unmounted %p", backend);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      GError *error;
      error = g_error_new (G_IO_ERROR,
                           G_IO_ERROR_FAILED_HANDLED,
                           _("Filesystem is busy"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static gboolean
try_unmount (GVfsBackend        *backend,
             GVfsJobUnmount     *job,
             GMountUnmountFlags  flags,
             GMountSource       *mount_source)
{
  DEBUG ("In try_unmount, unmount_flags=%d", flags, mount_source);

  if (flags & G_MOUNT_UNMOUNT_FORCE)
    {
      DEBUG ("forcibly unmounted %p", backend);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else if (g_mount_source_is_dummy (mount_source))
    {
      if (g_vfs_backend_has_blocking_processes (backend))
        {
          GError *error;
          error = g_error_new (G_IO_ERROR,
                               G_IO_ERROR_BUSY,
                               _("Filesystem is busy"));
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
        }
      else
        {
          DEBUG ("unmounted %p", backend);
          g_vfs_job_succeeded (G_VFS_JOB (job));
        }
    }
  else
    {
      g_vfs_backend_unmount_with_operation (backend,
                                            mount_source,
                                            (GAsyncReadyCallback) unmount_with_op_cb,
                                            job);
    }

  return TRUE;
}

/* ------------------------------------------------------------------------------------------------- */

static void
free_read_handle (ReadHandle *read_handle)
{
  if (read_handle->file != NULL)
    {
      gp_file_unref (read_handle->file);
    }
  g_free (read_handle);
}

static void
do_open_for_read_real (GVfsBackend *backend,
                       GVfsJobOpenForRead *job,
                       const char *filename,
                       gboolean get_preview)
{
  int rc;
  GError *error;
  ReadHandle *read_handle;
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  char *dir;
  char *name;

  ensure_not_dirty (gphoto2_backend);

  split_filename_with_ignore_prefix (gphoto2_backend, filename, &dir, &name);

  if (is_directory (gphoto2_backend, dir, name))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_IS_DIRECTORY,
                        _("Can't open directory"));
      goto out;
    }

  if (!is_regular (gphoto2_backend, dir, name))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("No such file"));
      goto out;
    }

  read_handle = g_new0 (ReadHandle, 1);
  rc = gp_file_new (&read_handle->file);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_("Error creating file object"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      free_read_handle (read_handle);
      goto out;
    }

  rc = gp_camera_file_get (gphoto2_backend->camera,
                           dir,
                           name,
                           get_preview ? GP_FILE_TYPE_PREVIEW : GP_FILE_TYPE_NORMAL,
                           read_handle->file,
                           gphoto2_backend->context);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_("Error getting file"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      free_read_handle (read_handle);
      goto out;
    }

  rc = gp_file_get_data_and_size (read_handle->file, &read_handle->data, &read_handle->size);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_("Error getting data from file"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      free_read_handle (read_handle);
      goto out;
    }

  DEBUG ("  data=%p size=%ld handle=%p get_preview=%d",
         read_handle->data, read_handle->size, read_handle, get_preview);

  g_mutex_lock (gphoto2_backend->lock);
  gphoto2_backend->open_read_handles = g_list_prepend (gphoto2_backend->open_read_handles, read_handle);
  g_mutex_unlock (gphoto2_backend->lock);
      
  read_handle->cursor = 0;

  g_vfs_job_open_for_read_set_can_seek (job, TRUE);
  g_vfs_job_open_for_read_set_handle (job, GINT_TO_POINTER (read_handle));
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_free (name);
  g_free (dir);
}


static void
do_open_for_read (GVfsBackend *backend,
                  GVfsJobOpenForRead *job,
                  const char *filename)
{
  DEBUG ("open_for_read (%s)", filename);

  do_open_for_read_real (backend,
                         job,
                         filename,
                         FALSE);
}

static void
do_open_icon_for_read (GVfsBackend *backend,
                       GVfsJobOpenIconForRead *job,
                       const char *icon_id)
{
  DEBUG ("open_icon_for_read (%s)", icon_id);

  if (g_str_has_prefix (icon_id, "preview:"))
    {
      do_open_for_read_real (backend,
                             G_VFS_JOB_OPEN_FOR_READ (job),
                             icon_id + sizeof ("preview:") - 1,
                             TRUE);
    }
  else
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        _("Malformed icon identifier '%s'"),
                        icon_id);
    }
}

/* ------------------------------------------------------------------------------------------------- */

static gboolean
try_read (GVfsBackend *backend,
          GVfsJobRead *job,
          GVfsBackendHandle handle,
          char *buffer,
          gsize bytes_requested)
{
  //GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  ReadHandle *read_handle = (ReadHandle *) handle;
  gsize bytes_left;
  gsize bytes_to_copy;

  DEBUG ("do_read() %d @ %ld of %ld, handle=%p", bytes_requested, read_handle->cursor, read_handle->size, handle);

  if (read_handle->cursor >= read_handle->size)
    {
      bytes_to_copy = 0;
      goto out;
    }
  
  bytes_left = read_handle->size - read_handle->cursor;
  if (bytes_requested > bytes_left)
    bytes_to_copy = bytes_left;
  else
    bytes_to_copy = bytes_requested;

  memcpy (buffer, read_handle->data + read_handle->cursor, bytes_to_copy);
  read_handle->cursor += bytes_to_copy;

 out:
  
  g_vfs_job_read_set_size (job, bytes_to_copy);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
}

/* ------------------------------------------------------------------------------------------------- */

static gboolean
try_seek_on_read (GVfsBackend *backend,
                  GVfsJobSeekRead *job,
                  GVfsBackendHandle handle,
                  goffset    offset,
                  GSeekType  type)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  ReadHandle *read_handle = (ReadHandle *) handle;
  long new_offset;

  DEBUG ("seek_on_read() offset=%d, type=%d, handle=%p", (int)offset, type, handle);

  switch (type)
    {
    default:
    case G_SEEK_SET:
      new_offset = offset;
      break;
    case G_SEEK_CUR:
      new_offset = read_handle->cursor + offset;
      break;
    case G_SEEK_END:
      new_offset = read_handle->size + offset;
      break;
    }

  if (new_offset < 0 || new_offset > read_handle->size)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        _("Error seeking in stream on camera %s"), gphoto2_backend->gphoto2_port);
    }
  else
    {
      read_handle->cursor = new_offset;
      g_vfs_job_seek_read_set_offset (job, offset);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  return TRUE;
}

/* ------------------------------------------------------------------------------------------------- */

/* cannot be async because we unref the CameraFile */
static void
do_close_read (GVfsBackend *backend,
                GVfsJobCloseRead *job,
                GVfsBackendHandle handle)
{
  ReadHandle *read_handle = (ReadHandle *) handle;
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);

  DEBUG ("close_read() handle=%p", handle);

  g_mutex_lock (gphoto2_backend->lock);
  gphoto2_backend->open_read_handles = g_list_remove (gphoto2_backend->open_read_handles, read_handle);
  g_mutex_unlock (gphoto2_backend->lock);

  free_read_handle (read_handle);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

/* ------------------------------------------------------------------------------------------------- */

static void
do_query_info (GVfsBackend *backend,
	       GVfsJobQueryInfo *job,
	       const char *filename,
	       GFileQueryInfoFlags flags,
	       GFileInfo *info,
	       GFileAttributeMatcher *matcher)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  GError *error;
  char *dir;
  char *name;

  DEBUG ("query_info (%s)", filename);

  split_filename_with_ignore_prefix (gphoto2_backend, filename, &dir, &name);

  error = NULL;
  if (!file_get_info (gphoto2_backend, dir, name, info, &error, FALSE))
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
  else
    {
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  
  g_free (name);
  g_free (dir);
}

/* ------------------------------------------------------------------------------------------------- */

static gboolean
try_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  char *dir;
  char *name;
  gboolean ret;

  DEBUG ("try_query_info (%s)", filename);

  ret = FALSE;

  split_filename_with_ignore_prefix (gphoto2_backend, filename, &dir, &name);

  if (!file_get_info (gphoto2_backend, dir, name, info, NULL, TRUE))
    {
      DEBUG ("  BUU no info from cache for try_query_info (%s)", filename);
      goto out;
    }
  DEBUG ("  YAY got info from cache for try_query_info (%s)", filename);

  g_vfs_job_succeeded (G_VFS_JOB (job));
  ret = TRUE;
  
 out:
  g_free (name);
  g_free (dir);
  return ret;
}

/* ------------------------------------------------------------------------------------------------- */

static void
do_enumerate (GVfsBackend *backend,
              GVfsJobEnumerate *job,
              const char *given_filename,
              GFileAttributeMatcher *matcher,
              GFileQueryInfoFlags flags)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  GFileInfo *info;
  GList *l;
  GError *error;
  CameraList *list;
  int n;
  int rc;
  char *filename;
  gboolean using_cached_dir_list;
  gboolean using_cached_file_list;
  char *as_dir;
  char *as_name;

  l = NULL;
  using_cached_dir_list = FALSE;
  using_cached_file_list = FALSE;

  filename = add_ignore_prefix (gphoto2_backend, given_filename);
  DEBUG ("enumerate ('%s', with_prefix='%s')", given_filename, filename);

  split_filename_with_ignore_prefix (gphoto2_backend, given_filename, &as_dir, &as_name);
  if (!is_directory (gphoto2_backend, as_dir, as_name))
    {
      if (is_regular (gphoto2_backend, as_dir, as_name))
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_DIRECTORY,
                            _("Not a directory"));
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            _("No such file or directory"));
        }
      g_free (as_dir);
      g_free (as_name);
      return;
    }
  g_free (as_dir);
  g_free (as_name);

  /* first, list the folders */
  g_mutex_lock (gphoto2_backend->lock);
  list = g_hash_table_lookup (gphoto2_backend->dir_name_cache, filename);
  if (list == NULL)
    {
      g_mutex_unlock (gphoto2_backend->lock);

      ensure_not_dirty (gphoto2_backend);

      DEBUG ("  Generating dir list for dir '%s'", filename);

      gp_list_new (&list);
      rc = gp_camera_folder_list_folders (gphoto2_backend->camera, 
                                          filename, 
                                          list, 
                                          gphoto2_backend->context);
      if (rc != 0)
        {
          error = get_error_from_gphoto2 (_("Failed to get folder list"), rc);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          g_free (filename);
          return;
        }  
    }
  else
    {
      DEBUG ("  Using cached dir list for dir '%s'", filename);
      using_cached_dir_list = TRUE;
      gp_list_ref (list);
      g_mutex_unlock (gphoto2_backend->lock);
    }
  for (n = 0; n < gp_list_count (list); n++) 
    {
      const char *name;

      gp_list_get_name (list, n, &name);
      DEBUG ("  enum folder '%s'", name);
      info = g_file_info_new ();
      error = NULL;
      if (!file_get_info (gphoto2_backend, filename, name, info, &error, FALSE))
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          g_list_foreach (l, (GFunc) g_object_unref, NULL);
          g_list_free (l);
          gp_list_free (list);
          return;
        }
      l = g_list_append (l, info);
    }
  if (!using_cached_dir_list)
    {
#ifndef DEBUG_NO_CACHING
      g_mutex_lock (gphoto2_backend->lock);
      g_hash_table_insert (gphoto2_backend->dir_name_cache, g_strdup (filename), list);
      g_mutex_unlock (gphoto2_backend->lock);
#endif
    }
  else
    {
      g_mutex_lock (gphoto2_backend->lock);
      gp_list_unref (list);
      g_mutex_unlock (gphoto2_backend->lock);
    }


  /* then list the files in each folder */
  g_mutex_lock (gphoto2_backend->lock);
  list = g_hash_table_lookup (gphoto2_backend->file_name_cache, filename);
  if (list == NULL)
    {
      g_mutex_unlock (gphoto2_backend->lock);
      ensure_not_dirty (gphoto2_backend);

      DEBUG ("  Generating file list for dir '%s'", filename);

      gp_list_new (&list);
      rc = gp_camera_folder_list_files (gphoto2_backend->camera, 
                                        filename, 
                                        list, 
                                        gphoto2_backend->context);
      if (rc != 0)
        {
          error = get_error_from_gphoto2 (_("Failed to get file list"), rc);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          g_free (filename);
          return;
        }
    }
  else
    {
      DEBUG ("  Using cached file list for dir '%s'", filename);
      using_cached_file_list = TRUE;
      gp_list_ref (list);
      g_mutex_unlock (gphoto2_backend->lock);
    }
  for (n = 0; n < gp_list_count (list); n++) 
    {
      const char *name;

      gp_list_get_name (list, n, &name);
      DEBUG ("  enum file '%s'", name);

      info = g_file_info_new ();
      error = NULL;
      if (!file_get_info (gphoto2_backend, filename, name, info, &error, FALSE))
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          g_list_foreach (l, (GFunc) g_object_unref, NULL);
          g_list_free (l);
          gp_list_free (list);
          return;
        }
      l = g_list_append (l, info);
    }
  if (!using_cached_file_list)
    {
#ifndef DEBUG_NO_CACHING
      g_mutex_lock (gphoto2_backend->lock);
      g_hash_table_insert (gphoto2_backend->file_name_cache, g_strdup (filename), list);
      g_mutex_unlock (gphoto2_backend->lock);
#endif
    }
  else
    {
      g_mutex_lock (gphoto2_backend->lock);
      gp_list_unref (list);
      g_mutex_unlock (gphoto2_backend->lock);
    }

  /* and we're done */

  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_vfs_job_enumerate_add_infos (job, l);
  g_list_foreach (l, (GFunc) g_object_unref, NULL);
  g_list_free (l);
  g_vfs_job_enumerate_done (job);

  g_free (filename);
}

/* ------------------------------------------------------------------------------------------------- */

static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *given_filename,
               GFileAttributeMatcher *matcher,
               GFileQueryInfoFlags flags)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  GFileInfo *info;
  GList *l;
  GError *error;
  CameraList *list;
  int n;
  char *filename;
  const char *name;

  l = NULL;

  filename = add_ignore_prefix (gphoto2_backend, given_filename);
  DEBUG ("try_enumerate (%s)", given_filename);

  /* first, list the folders */
  g_mutex_lock (gphoto2_backend->lock);
  list = g_hash_table_lookup (gphoto2_backend->dir_name_cache, filename);
  if (list == NULL)
    {
      g_mutex_unlock (gphoto2_backend->lock);
      goto error_not_cached;
    }
  gp_list_ref (list);
  g_mutex_unlock (gphoto2_backend->lock);
  for (n = 0; n < gp_list_count (list); n++) 
    {
      gp_list_get_name (list, n, &name);
      DEBUG ("  try_enum folder '%s'", name);
      info = g_file_info_new ();
      if (!file_get_info (gphoto2_backend, filename, name, info, &error, TRUE))
        {
          g_mutex_lock (gphoto2_backend->lock);
          gp_list_unref (list);
          g_mutex_unlock (gphoto2_backend->lock);
          goto error_not_cached;
        }
      l = g_list_append (l, info);
    }
  g_mutex_lock (gphoto2_backend->lock);
  gp_list_unref (list);
  g_mutex_unlock (gphoto2_backend->lock);

  /* then list the files in each folder */
  g_mutex_lock (gphoto2_backend->lock);
  list = g_hash_table_lookup (gphoto2_backend->file_name_cache, filename);
  if (list == NULL)
    {
      g_mutex_unlock (gphoto2_backend->lock);
      goto error_not_cached;
    }
  gp_list_ref (list);
  g_mutex_unlock (gphoto2_backend->lock);
  for (n = 0; n < gp_list_count (list); n++) 
    {
      gp_list_get_name (list, n, &name);
      DEBUG ("  try_enum file '%s'", name);

      info = g_file_info_new ();
      if (!file_get_info (gphoto2_backend, filename, name, info, &error, TRUE))
        {
          g_mutex_lock (gphoto2_backend->lock);
          gp_list_unref (list);
          g_mutex_unlock (gphoto2_backend->lock);
          goto error_not_cached;
        }
      l = g_list_append (l, info);
    }
  g_mutex_lock (gphoto2_backend->lock);
  gp_list_unref (list);
  g_mutex_unlock (gphoto2_backend->lock);

  /* and we're done */

  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_vfs_job_enumerate_add_infos (job, l);
  g_list_foreach (l, (GFunc) g_object_unref, NULL);
  g_list_free (l);
  g_vfs_job_enumerate_done (job);

  g_free (filename);
  DEBUG ("  YAY got info from cache for try_enumerate (%s)", given_filename);
  return TRUE;

 error_not_cached:
  g_list_foreach (l, (GFunc) g_object_unref, NULL);
  g_list_free (l);

  g_free (filename);
  DEBUG ("  BUU no info from cache for try_enumerate (%s)", given_filename);
  return FALSE;
}

/* ------------------------------------------------------------------------------------------------- */

static void
do_query_fs_info (GVfsBackend *backend,
		  GVfsJobQueryFsInfo *job,
		  const char *filename,
		  GFileInfo *info,
		  GFileAttributeMatcher *attribute_matcher)
{
  int rc;
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  CameraStorageInformation *storage_info;
  int num_storage_info;

  DEBUG ("query_fs_info (%s)", filename);

  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "gphoto2");
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_IF_LOCAL);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, !gphoto2_backend->can_write);

  rc = gp_camera_get_storageinfo (gphoto2_backend->camera, &storage_info, &num_storage_info, gphoto2_backend->context);
  if (rc == 0)
    {
      if (num_storage_info >= 1)
        {
          /* for now we only support a single storage head */
          if (storage_info[0].fields & GP_STORAGEINFO_MAXCAPACITY)
            {
              g_mutex_lock (gphoto2_backend->lock);
              gphoto2_backend->capacity = storage_info[0].capacitykbytes * 1024;
              g_mutex_unlock (gphoto2_backend->lock);
              g_file_info_set_attribute_uint64 (info, 
                                                G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, 
                                                (guint64) gphoto2_backend->capacity);
            }

          if (storage_info[0].fields & GP_STORAGEINFO_FREESPACEKBYTES)
            {
              g_mutex_lock (gphoto2_backend->lock);
              gphoto2_backend->free_space = storage_info[0].freekbytes * 1024;
              g_mutex_unlock (gphoto2_backend->lock);
              g_file_info_set_attribute_uint64 (info, 
                                                G_FILE_ATTRIBUTE_FILESYSTEM_FREE, 
                                                (guint64) gphoto2_backend->free_space);
            }
        }
      DEBUG ("  got %d storage_info objects", num_storage_info);
    }
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

/* ------------------------------------------------------------------------------------------------- */

static gboolean
try_query_fs_info (GVfsBackend *backend,
		  GVfsJobQueryFsInfo *job,
		  const char *filename,
		  GFileInfo *info,
		  GFileAttributeMatcher *attribute_matcher)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  gboolean ret;
  gint64 free_space;
  gint64 capacity;

  DEBUG ("try_query_fs_info (%s)", filename);

  ret = FALSE;

  g_mutex_lock (gphoto2_backend->lock);
  free_space = gphoto2_backend->free_space;
  capacity = gphoto2_backend->capacity;
  g_mutex_unlock (gphoto2_backend->lock);

  if (free_space == -1 || capacity == -1)
    {
      DEBUG ("  BUU no info from cache for try_query_fs_info (%s)", filename);
      goto out;
    }
  DEBUG ("  YAY got info from cache for try_query_fs_info (%s)", filename);

  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "gphoto2");
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_IF_LOCAL);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, !gphoto2_backend->can_write);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, (guint64) capacity);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, (guint64) free_space);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  ret = TRUE;
 out:
  return ret;
}

/* ------------------------------------------------------------------------------------------------- */

static void
do_make_directory (GVfsBackend *backend,
                   GVfsJobMakeDirectory *job,
                   const char *filename)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  char *name;
  char *dir;
  int rc;
  GError *error;

  DEBUG ("make_directory (%s)", filename);

  ensure_not_dirty (gphoto2_backend);

  dir = NULL;
  name = NULL;
  error = NULL;

  if (!gphoto2_backend->can_write)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Not supported"));
      goto out;
    }

  split_filename_with_ignore_prefix (gphoto2_backend, filename, &dir, &name);

  rc = gp_camera_folder_make_dir (gphoto2_backend->camera,
                                  dir,
                                  name,
                                  gphoto2_backend->context);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_("Error creating directory"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  caches_invalidate_dir (gphoto2_backend, dir);
  caches_invalidate_free_space (gphoto2_backend);
  monitors_emit_created (gphoto2_backend, dir, name);

  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_free (dir);
  g_free (name);
}

/* ------------------------------------------------------------------------------------------------- */

static int
do_slow_file_rename_in_same_dir (GVfsBackendGphoto2 *gphoto2_backend,
                                 const char *dir,
                                 const char *name,
                                 const char *new_name,
                                 gboolean allow_overwrite)
{
  int rc;
  CameraFile *file;
  CameraFile *file_dest;
  const char *data;
  unsigned long int size;

  file = NULL;
  file_dest = NULL;

  DEBUG ("do_slow_file_rename_in_same_dir() '%s' '%s' -> '%s'", dir, name, new_name);

  rc = gp_file_new (&file);
  if (rc != 0)
    goto out;

  rc = gp_camera_file_get (gphoto2_backend->camera,
                           dir,
                           name,
                           GP_FILE_TYPE_NORMAL,
                           file,
                           gphoto2_backend->context);
  if (rc != 0)
    goto out;

  rc = gp_file_get_data_and_size (file, &data, &size);
  if (rc != 0)
    goto out;

  rc = gp_file_new (&file_dest);
  if (rc != 0)
    goto out;

  rc = gp_file_copy (file_dest, file);
  if (rc != 0)
    goto out;

  rc = gp_file_set_name (file_dest, new_name);
  if (rc != 0)
    goto out;

  if (allow_overwrite)
    {
      gp_camera_file_delete (gphoto2_backend->camera,
                             dir,
                             new_name,
                             gphoto2_backend->context);
      if (rc != 0)
        {
          DEBUG ("  file delete failed as part of slow rename rc=%d", rc);
          goto out;
        }
    }

  rc = gp_camera_folder_put_file (gphoto2_backend->camera, dir, file_dest, gphoto2_backend->context);
  if (rc != 0)
    goto out;

  rc = gp_camera_file_delete (gphoto2_backend->camera,
                              dir,
                              name,
                              gphoto2_backend->context);
  if (rc != 0)
    {
      /* at least try to clean up the newly created file... */
      gp_camera_file_delete (gphoto2_backend->camera,
                             dir,
                             new_name,
                             gphoto2_backend->context);
      goto out;
    }

 out:
  if (file != NULL)
    gp_file_unref (file);
  if (file_dest != NULL)
    gp_file_unref (file_dest);
  return rc;
}

/* ------------------------------------------------------------------------------------------------- */

static int
do_file_rename_in_same_dir (GVfsBackendGphoto2 *gphoto2_backend,
                            const char *dir,
                            const char *name,
                            const char *new_name,
                            gboolean allow_overwrite)
{
  /* TODO: The libgphoto2 API speaks of just using
   *       gp_camera_file_set_info() to achieve this. However this
   *       fails on the devices that I own. So fall back to the slow
   *       method for now. Patches welcome for someone with a device
   *       where the above mentioned trick works.
   */
  return do_slow_file_rename_in_same_dir (gphoto2_backend, dir, name, new_name, allow_overwrite);
}

/* ------------------------------------------------------------------------------------------------- */

static int
do_dir_rename_in_same_dir (GVfsBackendGphoto2 *gphoto2_backend,
                           const char *dir,
                           const char *name,
                           const char *new_name)
{
  int rc;
  char *dir_name;

  dir_name = g_build_filename (dir, name, NULL);

  DEBUG ("do_dir_rename_in_same_dir() '%s' '%s' -> '%s' ('%s')", dir, name, new_name, dir_name);

  /* TODO: Support non-empty folders by recursively renaming stuff.
   *       Or that might be too dangerous as it's not exactly atomic.
   *       And renaming files may be slow; see do_file_rename_in_same_dir() above.
   */
  if (is_directory_empty (gphoto2_backend, dir_name))
    {
      rc = gp_camera_folder_make_dir (gphoto2_backend->camera,
                                      dir,
                                      new_name,
                                      gphoto2_backend->context);
      if (rc != 0)
        goto out;
      
      rc = gp_camera_folder_remove_dir (gphoto2_backend->camera,
                                        dir,
                                        name,
                                        gphoto2_backend->context);
      if (rc != 0)
        goto out;
    }
  else
    {
      rc = GP_ERROR_NOT_SUPPORTED;
    }
  
 out:
  g_free (dir_name);
  return rc;
}

/* ------------------------------------------------------------------------------------------------- */

static void
do_set_display_name (GVfsBackend *backend,
                     GVfsJobSetDisplayName *job,
                     const char *filename,
                     const char *display_name)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  char *name;
  char *dir;
  int rc;
  char *dir_name;
  GError *error;
  char *new_name;

  ensure_not_dirty (gphoto2_backend);

  DEBUG ("set_display_name() '%s' -> '%s'", filename, display_name);

  dir = NULL;
  name = NULL;
  dir_name = NULL;
  new_name = NULL;

  if (!gphoto2_backend->can_write)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Not supported"));
      goto out;
    }

  split_filename_with_ignore_prefix (gphoto2_backend, filename, &dir, &name);

  /* refuse is desired name is already taken */
  if (is_directory (gphoto2_backend, dir, display_name) ||
      is_regular (gphoto2_backend, dir, display_name))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_EXISTS,
                        _("Name already exists"));
      goto out;      
    }

  /* ensure name is not too long - otherwise it might screw up enumerating
   * the folder on some devices 
   */
  if (strlen (display_name) > 63)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("New name too long"));
      goto out;
    }

  if (is_directory (gphoto2_backend, dir, name))
    {
      /* dir renaming */
      rc = do_dir_rename_in_same_dir (gphoto2_backend, dir, name, display_name);
      if (rc != 0)
        {
          error = get_error_from_gphoto2 (_("Error renaming dir"), rc);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }
      caches_invalidate_file (gphoto2_backend, dir, name);
    }
  else
    {
      /* file renaming */
      rc = do_file_rename_in_same_dir (gphoto2_backend, dir, name, display_name, FALSE);
      if (rc != 0)
        {
          error = get_error_from_gphoto2 (_("Error renaming file"), rc);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }
      caches_invalidate_file (gphoto2_backend, dir, name);
    }


  /* emit on monitor */
  monitors_emit_deleted (gphoto2_backend, dir, name);
  monitors_emit_created (gphoto2_backend, dir, display_name);

  new_name = g_build_filename (dir + strlen (gphoto2_backend->ignore_prefix), display_name, NULL);
  g_vfs_job_set_display_name_set_new_path (job, new_name);

  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_free (dir);
  g_free (name);
  g_free (dir_name);
  g_free (new_name);
}

/* ------------------------------------------------------------------------------------------------- */

static void
do_delete (GVfsBackend *backend,
           GVfsJobDelete *job,
           const char *filename)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  char *name;
  char *dir;
  int rc;
  GError *error;
  char *dir_name;

  ensure_not_dirty (gphoto2_backend);

  DEBUG ("delete() '%s'", filename);

  dir = NULL;
  name = NULL;
  dir_name = NULL;

  if (!gphoto2_backend->can_delete)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Not supported"));
      goto out;
    }

  split_filename_with_ignore_prefix (gphoto2_backend, filename, &dir, &name);

  if (is_directory (gphoto2_backend, dir, name))
    {
      dir_name = add_ignore_prefix (gphoto2_backend, filename);
      if (!is_directory_empty (gphoto2_backend, dir_name))
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_EMPTY,
                            _("Directory '%s' is not empty"), filename);
          goto out;
        }
      else
        {
          rc = gp_camera_folder_remove_dir (gphoto2_backend->camera,
                                            dir,
                                            name,
                                            gphoto2_backend->context);
          if (rc != 0)
            {
              error = get_error_from_gphoto2 (_("Error deleting directory"), rc);
              g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
              g_error_free (error);
              goto out;
            }
          caches_invalidate_file (gphoto2_backend, dir, name);
          caches_invalidate_free_space (gphoto2_backend);
          monitors_emit_deleted (gphoto2_backend, dir, name);
        }
    }
  else
    {
      if (!is_regular (gphoto2_backend, dir, name))
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            _("No such file or directory"));
          goto out;
        }

      rc = gp_camera_file_delete (gphoto2_backend->camera,
                                  dir,
                                  name,
                                  gphoto2_backend->context);
      if (rc != 0)
        {
          error = get_error_from_gphoto2 (_("Error deleting file"), rc);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }

      caches_invalidate_file (gphoto2_backend, dir, name);
      caches_invalidate_free_space (gphoto2_backend);
      monitors_emit_deleted (gphoto2_backend, dir, name);
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_free (dir);
  g_free (name);
  g_free (dir_name);
}

/* ------------------------------------------------------------------------------------------------- */

static void 
do_create_internal (GVfsBackend *backend,
                    GVfsJobOpenForWrite *job,
                    const char *filename,
                    GFileCreateFlags flags,
                    gboolean job_is_replace,
                    gboolean job_is_append_to)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  WriteHandle *handle;
  char *dir;
  char *name;

  ensure_not_dirty (gphoto2_backend);

  dir = NULL;
  name = NULL;
 
  if (!gphoto2_backend->can_write)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Not supported"));
      goto out;
    }

  split_filename_with_ignore_prefix (gphoto2_backend, filename, &dir, &name);

  if (is_directory (gphoto2_backend, dir, name))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_IS_DIRECTORY,
                        _("Can't write to directory"));
      goto out;
    }

  /* unless we're replacing or appending.. error out if file already exists */
  if (is_regular (gphoto2_backend, dir, name))
    {
      if (! (job_is_replace || job_is_append_to))
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_EXISTS,
                            _("File exists"));
          goto out;
        }
    }
  else
    {
      if (job_is_replace || job_is_append_to)
        {
          /* so we're not really replacing or appending; dont fail these
           * operations; just turn them into create instead...
           */
          job_is_replace = FALSE;
          job_is_append_to = FALSE;
        }
    }      

  handle = g_new0 (WriteHandle, 1);
  handle->filename = g_strdup (filename);
  handle->dir = g_strdup (dir);
  handle->name = g_strdup (name);
  handle->job_is_replace = job_is_replace;
  handle->job_is_append_to = job_is_append_to;
  handle->is_dirty = TRUE;

  /* if we're appending to a file read in all of the file to memory */
  if (job_is_append_to)
    {
      int rc;
      GError *error;
      CameraFile *file;
      const char *data;
      unsigned long int size;
      
      rc = gp_file_new (&file);
      if (rc != 0)
        {
          error = get_error_from_gphoto2 (_("Cannot allocate new file to append to"), rc);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          write_handle_free (handle);
          goto out;
        }

      rc = gp_camera_file_get (gphoto2_backend->camera,
                               dir,
                               name,
                               GP_FILE_TYPE_NORMAL,
                               file,
                               gphoto2_backend->context);
      if (rc != 0)
        {
          error = get_error_from_gphoto2 (_("Cannot read file to append to"), rc);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          write_handle_free (handle);
          gp_file_unref (file);
          goto out;
        }

      rc = gp_file_get_data_and_size (file, &data, &size);
      if (rc != 0)
        {
          error = get_error_from_gphoto2 (_("Cannot get data of file to append to"), rc);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          write_handle_free (handle);
          gp_file_unref (file);
          goto out;
        }

      handle->data = g_malloc (size + WRITE_INCREMENT);
      handle->allocated_size = size + WRITE_INCREMENT;
      handle->size = size;
      handle->cursor = size;
      memcpy (handle->data, data, size);
      gp_file_unref (file);
      
    }
  else
    {
      handle->data = g_malloc (WRITE_INCREMENT);
      handle->allocated_size = WRITE_INCREMENT;
    }

  g_vfs_job_open_for_write_set_handle (job, handle);
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);

  gphoto2_backend->open_write_handles = g_list_prepend (gphoto2_backend->open_write_handles, handle);

  DEBUG ("  handle=%p", handle);

  /* make sure we invalidate the dir and the file */
  caches_invalidate_file (gphoto2_backend, dir, name);

  /* emit on the monitor - hopefully some client won't need info 
   * about this (to avoid committing dirty bits midwrite) before
   * the write is done...
   */
  if (job_is_replace || job_is_append_to)
    monitors_emit_changed (gphoto2_backend, dir, name);
  else
    monitors_emit_created (gphoto2_backend, dir, name);

  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_free (dir);
  g_free (name);
}

/* ------------------------------------------------------------------------------------------------- */

static void 
do_create (GVfsBackend *backend,
           GVfsJobOpenForWrite *job,
           const char *filename,
           GFileCreateFlags flags)
{
  DEBUG ("create() '%s' flags=0x%04x", filename, flags);

  return do_create_internal (backend, job, filename, flags, FALSE, FALSE);
}

/* ------------------------------------------------------------------------------------------------- */

static void
do_replace (GVfsBackend *backend,
            GVfsJobOpenForWrite *job,
            const char *filename,
            const char *etag,
            gboolean make_backup,
            GFileCreateFlags flags)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  char *dir;
  char *name;

  DEBUG ("replace() '%s' etag='%s' make_backup=%d flags=0x%04x", filename, etag, make_backup, flags);

  dir = NULL;
  name = NULL;
  split_filename_with_ignore_prefix (gphoto2_backend, filename, &dir, &name);
  
  /* write a new file
   * - will delete the existing one when done in do_close_write() 
   */
  do_create_internal (backend, job, filename, flags, TRUE, FALSE);

  g_free (dir);
  g_free (name);
}

/* ------------------------------------------------------------------------------------------------- */

static void
do_append_to (GVfsBackend *backend,
              GVfsJobOpenForWrite *job,
              const char *filename,
              GFileCreateFlags flags)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  char *dir;
  char *name;

  DEBUG ("append_to() '%s' flags=0x%04x", filename, flags);

  dir = NULL;
  name = NULL;
  split_filename_with_ignore_prefix (gphoto2_backend, filename, &dir, &name);
  
  /* write a new file
   * - will read existing data in do_create_internal
   * - will delete the existing one when done in do_close_write() 
   */
  do_create_internal (backend, job, filename, flags, FALSE, TRUE);

  g_free (dir);
  g_free (name);
}

/* ------------------------------------------------------------------------------------------------- */

static void
do_write (GVfsBackend *backend,
          GVfsJobWrite *job,
          GVfsBackendHandle _handle,
          char *buffer,
          gsize buffer_size)
{
  WriteHandle *handle = _handle;

  DEBUG ("write() %p, '%s', %d bytes", handle, handle->filename, buffer_size);

  /* ensure we have enough room */
  if (handle->cursor + buffer_size > handle->allocated_size)
    {
      unsigned long int new_allocated_size;
      new_allocated_size = ((handle->cursor + buffer_size) / WRITE_INCREMENT + 1) * WRITE_INCREMENT;
      handle->data = g_realloc (handle->data, new_allocated_size);
      handle->allocated_size = new_allocated_size;
      DEBUG ("    allocated_size is now %ld bytes)", handle->allocated_size);
    }


  memcpy (handle->data + handle->cursor, buffer, buffer_size);
  handle->cursor += buffer_size;

  if (handle->cursor > handle->size)
    handle->size = handle->cursor;

  /* this will make us dirty */
  handle->is_dirty = TRUE;

  g_vfs_job_write_set_written_size (job, buffer_size);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

/* ------------------------------------------------------------------------------------------------- */

static void
do_seek_on_write (GVfsBackend *backend,
                  GVfsJobSeekWrite *job,
                  GVfsBackendHandle handle,
                  goffset    offset,
                  GSeekType  type)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  WriteHandle *write_handle = handle;
  long new_offset;

  DEBUG ("seek_on_write() %p '%s' offset=%d type=%d cursor=%ld size=%ld", write_handle, write_handle->filename, (int)offset, type, write_handle->cursor, write_handle->size);

  switch (type)
    {
    default:
    case G_SEEK_SET:
      new_offset = offset;
      break;
    case G_SEEK_CUR:
      new_offset = write_handle->cursor + offset;
      break;
    case G_SEEK_END:
      new_offset = write_handle->size + offset;
      break;
    }

  if (new_offset < 0 || new_offset > write_handle->size)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			G_IO_ERROR_FAILED,
			_("Error seeking in stream on camera %s"), gphoto2_backend->gphoto2_port);
    }
  else
    {
      write_handle->cursor = new_offset;      
      g_vfs_job_seek_write_set_offset (job, offset);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
}

/* ------------------------------------------------------------------------------------------------- */

/* this functions updates the device with the data currently in write_handle */
static int
commit_write_handle (GVfsBackendGphoto2 *gphoto2_backend, WriteHandle *write_handle)
{
  int rc;
  CameraFile *file;

  DEBUG ("commit_write_handle() '%s' of size %ld", write_handle->filename, write_handle->size);

  /* no need to write as we're not dirty */
  if (!write_handle->is_dirty)
    {
      DEBUG ("  not dirty => not writing");
      return 0;
    }

  if (write_handle->delete_before || 
      (write_handle->job_is_replace || write_handle->job_is_append_to))
    {
      /* OK, so this is not atomic. But there's no way we can make it
       * atomic until rename works properly - see comments in
       * do_set_display_name() and why have do_slow_rename()...
       *
       * So first delete the existing file...
       */
      rc = gp_camera_file_delete (gphoto2_backend->camera,
                                  write_handle->dir,
                                  write_handle->name,
                                  gphoto2_backend->context);
      if (rc != 0)
        goto out;

      DEBUG ("  deleted '%s' '%s' for delete_before=%d, job_is_replace=%d, job_is_append_to=%d", 
             write_handle->dir, write_handle->name, 
             write_handle->delete_before, write_handle->job_is_replace, write_handle->job_is_append_to);
    }

  rc = gp_file_new (&file);
  if (rc != 0)
    goto out;

  gp_file_set_type (file, GP_FILE_TYPE_NORMAL);
  gp_file_set_name (file, write_handle->name);
  gp_file_set_mtime (file, time (NULL));
  gp_file_set_data_and_size (file, 
                             dup_for_gphoto2 (write_handle->data, write_handle->size), 
                             write_handle->size);
  
  rc = gp_camera_folder_put_file (gphoto2_backend->camera, write_handle->dir, file, gphoto2_backend->context);
  if (rc != 0)
    {
      gp_file_unref (file);
      goto out;
    }

  DEBUG ("  successfully wrote '%s' of %ld bytes", write_handle->filename, write_handle->size);
  monitors_emit_changed (gphoto2_backend, write_handle->dir, write_handle->name);

  gp_file_unref (file);

 out:
  write_handle->is_dirty = FALSE;
  write_handle->delete_before = TRUE;

  caches_invalidate_file (gphoto2_backend, write_handle->dir, write_handle->name);
  caches_invalidate_free_space (gphoto2_backend);

  return rc;
}

/* ------------------------------------------------------------------------------------------------- */

static void 
do_close_write (GVfsBackend *backend,
                GVfsJobCloseWrite *job,
                GVfsBackendHandle handle)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  WriteHandle *write_handle = handle;
  GError *error;
  int rc;

  DEBUG ("close_write() %p '%s' %ld bytes total", write_handle, write_handle->filename, write_handle->size);

  rc = commit_write_handle (gphoto2_backend, write_handle);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_("Error writing file"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);      
      g_error_free (error);
      goto out;
    }

  monitors_emit_changed (gphoto2_backend, write_handle->dir, write_handle->name);

  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  write_handle_free (write_handle);
  gphoto2_backend->open_write_handles = g_list_remove (gphoto2_backend->open_write_handles, write_handle);
}

/* ------------------------------------------------------------------------------------------------- */

static void
do_move (GVfsBackend *backend,
         GVfsJobMove *job,
         const char *source,
         const char *destination,
         GFileCopyFlags flags,
         GFileProgressCallback progress_callback,
         gpointer progress_callback_data)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  char *src_dir;
  char *src_name;
  char *dst_dir;
  char *dst_name;
  int rc;
  GError *error;
  gboolean mv_dir;

  DEBUG ("move() '%s' -> '%s' %04x)", source, destination, flags);

  ensure_not_dirty (gphoto2_backend);

  split_filename_with_ignore_prefix (gphoto2_backend, source, &src_dir, &src_name);
  split_filename_with_ignore_prefix (gphoto2_backend, destination, &dst_dir, &dst_name);

  /* this is an limited implementation that can only move files / folders in the same directory */
  if (strcmp (src_dir, dst_dir) != 0)
    {
      DEBUG ("  not supported (not same directory)");
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Not supported (not same directory)"));
      goto out;
    }

  mv_dir = FALSE;
  if (is_directory (gphoto2_backend, src_dir, src_name))
    {
      if (is_directory (gphoto2_backend, dst_dir, dst_name))
        {
          DEBUG ("  not supported (src is dir; dst is dir)");
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Not supported (src is dir, dst is dir)"));
          goto out;
        }
      else if (is_regular (gphoto2_backend, dst_dir, dst_name))
        {
          DEBUG ("  not supported (src is dir; dst is existing file)");
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Not supported (src is dir, dst is existing file)"));
          goto out;
        }
      mv_dir = TRUE;
    }
  else
    {
      if (is_directory (gphoto2_backend, dst_dir, dst_name))
        {
          DEBUG ("  not supported (src is file; dst is dir)");
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Not supported (src is file, dst is dir)"));
          goto out;
        }
    }

  /* ensure name is not too long - otherwise it might screw up enumerating
   * the folder on some devices 
   */
  if (strlen (dst_name) > 63)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("New name too long"));
      goto out;
    }

  if (mv_dir)
    {
      DEBUG ("  renaming dir");
      rc = do_dir_rename_in_same_dir (gphoto2_backend, src_dir, src_name, dst_name);
      if (rc != 0)
        {
          DEBUG ("  error renaming dir");
          error = get_error_from_gphoto2 (_("Error renaming dir"), rc);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }
    }
  else
    {
      DEBUG ("  renaming file");
      rc = do_file_rename_in_same_dir (gphoto2_backend, src_dir, src_name, dst_name, flags & G_FILE_COPY_OVERWRITE);
      if (rc != 0)
        {
          DEBUG ("  error renaming file");
          error = get_error_from_gphoto2 (_("Error renaming file"), rc);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }
    }

  caches_invalidate_file (gphoto2_backend, src_dir, src_name);
  monitors_emit_deleted (gphoto2_backend, src_dir, src_name);
  monitors_emit_created (gphoto2_backend, src_dir, dst_name);

  DEBUG ("  success");

  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_free (src_dir);
  g_free (src_name);
  g_free (dst_dir);
  g_free (dst_name);
}

/* ------------------------------------------------------------------------------------------------- */

static void
vfs_dir_monitor_destroyed (gpointer user_data, GObject *where_the_object_was)
{
  GList *l;
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (user_data);
  
  DEBUG ("vfs_dir_monitor_destroyed()");

  for (l = gphoto2_backend->dir_monitor_proxies; l != NULL; l = l->next)
    {
      MonitorProxy *proxy = l->data;
      if (G_OBJECT (proxy->vfs_monitor) == where_the_object_was)
        {
          gphoto2_backend->dir_monitor_proxies = g_list_remove (gphoto2_backend->dir_monitor_proxies, proxy);
          DEBUG ("  Removed dead dir monitor for '%s'", proxy->path);
          monitor_proxy_free (proxy);
          break;
        }
    }  
}

static void
do_create_dir_monitor (GVfsBackend *backend,
                       GVfsJobCreateMonitor *job,
                       const char *filename,
                       GFileMonitorFlags flags)
{
  char *dir;
  char *name;
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  MonitorProxy *proxy;

  DEBUG ("create_dir_monitor (%s)", filename);

  split_filename_with_ignore_prefix (gphoto2_backend, filename, &dir, &name);

  proxy = g_new0 (MonitorProxy, 1);
  proxy->path = add_ignore_prefix (gphoto2_backend, filename);
  proxy->vfs_monitor = g_vfs_monitor_new (backend);

  gphoto2_backend->dir_monitor_proxies = g_list_prepend (gphoto2_backend->dir_monitor_proxies, proxy);

  g_vfs_job_create_monitor_set_monitor (job, proxy->vfs_monitor);
  g_object_weak_ref (G_OBJECT (proxy->vfs_monitor), vfs_dir_monitor_destroyed, gphoto2_backend);
  g_object_unref (proxy->vfs_monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

/* ------------------------------------------------------------------------------------------------- */

static void
vfs_file_monitor_destroyed (gpointer user_data, GObject *where_the_object_was)
{
  GList *l;
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (user_data);
  
  DEBUG ("vfs_file_monitor_destroyed()");

  for (l = gphoto2_backend->file_monitor_proxies; l != NULL; l = l->next)
    {
      MonitorProxy *proxy = l->data;
      if (G_OBJECT (proxy->vfs_monitor) == where_the_object_was)
        {
          gphoto2_backend->dir_monitor_proxies = g_list_remove (gphoto2_backend->dir_monitor_proxies, proxy);
          DEBUG ("  Removed dead file monitor for '%s'", proxy->path);
          monitor_proxy_free (proxy);
          break;
        }
    }  
}

static void
do_create_file_monitor (GVfsBackend *backend,
                        GVfsJobCreateMonitor *job,
                        const char *filename,
                        GFileMonitorFlags flags)
{
  char *dir;
  char *name;
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  MonitorProxy *proxy;

  DEBUG ("create_file_monitor (%s)", filename);

  split_filename_with_ignore_prefix (gphoto2_backend, filename, &dir, &name);

  proxy = g_new0 (MonitorProxy, 1);
  proxy->path = add_ignore_prefix (gphoto2_backend, filename);
  proxy->vfs_monitor = g_vfs_monitor_new (backend);

  gphoto2_backend->file_monitor_proxies = g_list_prepend (gphoto2_backend->file_monitor_proxies, proxy);

  g_vfs_job_create_monitor_set_monitor (job, proxy->vfs_monitor);
  g_object_weak_ref (G_OBJECT (proxy->vfs_monitor), vfs_file_monitor_destroyed, gphoto2_backend);
  g_object_unref (proxy->vfs_monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

/* ------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_gphoto2_class_init (GVfsBackendGphoto2Class *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_gphoto2_finalize;

  backend_class->try_mount = try_mount;
  backend_class->mount = do_mount;
  backend_class->try_unmount = try_unmount;
  backend_class->open_icon_for_read = do_open_icon_for_read;
  backend_class->open_for_read = do_open_for_read;
  backend_class->try_read = try_read;
  backend_class->try_seek_on_read = try_seek_on_read;
  backend_class->close_read = do_close_read;
  backend_class->query_info = do_query_info;
  backend_class->enumerate = do_enumerate;
  backend_class->query_fs_info = do_query_fs_info;
  backend_class->make_directory = do_make_directory;
  backend_class->set_display_name = do_set_display_name;
  backend_class->delete = do_delete;
  backend_class->create = do_create;
  backend_class->replace = do_replace;
  backend_class->append_to = do_append_to;
  backend_class->write = do_write;
  backend_class->close_write = do_close_write;
  backend_class->seek_on_write = do_seek_on_write;
  backend_class->move = do_move;
  backend_class->create_dir_monitor = do_create_dir_monitor;
  backend_class->create_file_monitor = do_create_file_monitor;

  /* fast sync versions that only succeed if info is in the cache */
  backend_class->try_query_info = try_query_info;
  backend_class->try_enumerate = try_enumerate;
  backend_class->try_query_fs_info = try_query_fs_info;
}
