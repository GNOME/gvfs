/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

/* GVFS gphoto2 file system driver
 * 
 * Copyright (C) 2007 Red Hat, Inc.
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

/* NOTE: since we link the libcdio libs (GPLv2) into our process space
 * the combined work is GPLv2. This source file, however, is LGPLv2+.
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
#include <libhal.h>
#include <dbus/dbus.h>

#include "gvfsbackendgphoto2.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobenumerate.h"

/* see this bug http://bugzilla.gnome.org/show_bug.cgi?id=518284 */
#define _I18N_LATER(x) x

/*--------------------------------------------------------------------------------------------------------------*/

/* TODO:
 *
 *  - write support
 *    - be CAREFUL when adding this; need to invalidate the caches below
 *    - also need to check CameraStorageAccessType in CameraStorageInformation for
 *      whether the device supports it
 *
 *  - support for multiple storage heads
 *    - need a device that supports this
 *    - should be different mounts so need to infect GHalVolumeMonitor with libgphoto2
 *    - probably not a huge priority to add
 *    - might help properly resolve the hack we're doing in ensure_ignore_prefix()
 *
 *  - add payload cache
 *    - to help alleviate the fact that libgphoto2 doesn't allow partial downloads :-/
 *    - use max 25% of physical memory or at least 40MB
 *    - max file size 10% of cache or at least 20MB
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

  int num_open_files;

  DBusConnection *dbus_connection;
  LibHalContext *hal_ctx;
  char *hal_udi;
  char *hal_name;
  char *hal_icon_name;

  /* CACHES */

  /* fully qualified path -> GFileInfo */
  GHashTable *info_cache;

  /* dir name -> CameraList of (sub-) directory names in given directory */
  GHashTable *dir_name_cache;

  /* dir name -> CameraList of file names in given directory */
  GHashTable *file_name_cache;
};

G_DEFINE_TYPE (GVfsBackendGphoto2, g_vfs_backend_gphoto2, G_VFS_TYPE_BACKEND);

static GError *
get_error_from_gphoto2 (const char *message, int gphoto2_error_code)
{
  GError *error;
  /* TODO: properly map error number */
  error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %d: %s",
                       message,
                       gphoto2_error_code, 
                       gp_result_as_string (gphoto2_error_code));
  return error;
}

static void
release_device (GVfsBackendGphoto2 *gphoto2_backend)
{
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
  g_free (gphoto2_backend->hal_icon_name);
  gphoto2_backend->hal_icon_name = NULL;

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
}

static void
g_vfs_backend_gphoto2_finalize (GObject *object)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (object);

  /*g_warning ("finalizing %p", object);*/

  release_device (gphoto2_backend);

  if (G_OBJECT_CLASS (g_vfs_backend_gphoto2_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_gphoto2_parent_class)->finalize) (object);
}

static void
g_vfs_backend_gphoto2_init (GVfsBackendGphoto2 *gphoto2_backend)
{
  GVfsBackend *backend = G_VFS_BACKEND (gphoto2_backend);
  GMountSpec *mount_spec;

  /*g_warning ("initing %p", gphoto2_backend);*/

  g_vfs_backend_set_display_name (backend, "gphoto2");

  mount_spec = g_mount_spec_new ("gphoto2");
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  g_mount_spec_unref (mount_spec);
}

static char *
compute_icon_name (GVfsBackendGphoto2 *gphoto2_backend)
{
  char *result;

  if (gphoto2_backend->hal_icon_name == NULL)
    {
      result = g_strdup_printf ("camera");
    }
  else
    {
      result = g_strdup (gphoto2_backend->hal_icon_name);
    }

  return result;
}

static char *
compute_display_name (GVfsBackendGphoto2 *gphoto2_backend)
{
  char *result;

  if (gphoto2_backend->hal_name == NULL)
    {
      /* Translator: %s represents the device, e.g. usb:001,042  */
      result = g_strdup_printf (_("Digital Camera (%s)"), gphoto2_backend->gphoto2_port);
    }
  else
    {
      result = g_strdup (gphoto2_backend->hal_name);
    }

  return result;
}


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

  /*g_warning ("Parsed '%s' into bus=%d device=%d", gphoto2_backend->gphoto2_port, usb_bus_num, usb_device_num);*/

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
                      
                      /*g_warning ("looking at usb device '%s' with bus=%d, device=%d", 
                        udi, device_usb_bus_num, device_usb_device_num);*/
                      
                      if (device_usb_bus_num == usb_bus_num && 
                          device_usb_device_num == usb_device_num)
                        {
                          char *name;
                          const char *parent_udi;
                          LibHalPropertySet *ps2;

                          /*g_warning ("udi '%s' is the one!", gphoto2_backend->hal_udi);*/
                          
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
                              gphoto2_backend->hal_icon_name = g_strdup (icon_from_hal);
                            }
                          else
                            {
                              if (m == 1)
                                {
                                  gphoto2_backend->hal_icon_name = g_strdup ("multimedia-player");
                                }
                              else
                                {
                                  gphoto2_backend->hal_icon_name = g_strdup ("camera");
                                }
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

static void
_hal_device_removed (LibHalContext *hal_ctx, const char *udi)
{
  GVfsBackendGphoto2 *gphoto2_backend;

  gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (libhal_ctx_get_user_data (hal_ctx));

  if (gphoto2_backend->hal_udi != NULL && strcmp (udi, gphoto2_backend->hal_udi) == 0)
    {
      /*g_warning ("we have been removed!");*/

      /* TODO: need a cleaner way to force unmount ourselves */
      exit (1);
    }
}

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
  DBusError dbus_error;

  /*g_warning ("do_mount %p", gphoto2_backend);*/

  /* setup libhal */

  dbus_error_init (&dbus_error);
  gphoto2_backend->dbus_connection = dbus_bus_get_private (DBUS_BUS_SYSTEM, &dbus_error);
  if (dbus_error_is_set (&dbus_error))
    {
      release_device (gphoto2_backend);
      dbus_error_free (&dbus_error);
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _I18N_LATER("Cannot connect to the system bus"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }
  
  gphoto2_backend->hal_ctx = libhal_ctx_new ();
  if (gphoto2_backend->hal_ctx == NULL)
    {
      release_device (gphoto2_backend);
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _I18N_LATER("Cannot create libhal context"));
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
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _I18N_LATER("Cannot initialize libhal"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  libhal_ctx_set_device_removed (gphoto2_backend->hal_ctx, _hal_device_removed);
  libhal_ctx_set_user_data (gphoto2_backend->hal_ctx, gphoto2_backend);

  /* setup gphoto2 */

  host = g_mount_spec_get (mount_spec, "host");
  /*g_warning ("host='%s'", host);*/
  if (host == NULL || strlen (host) < 3 || host[0] != '[' || host[strlen (host) - 1] != ']')
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _I18N_LATER("No device specified"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }

  gphoto2_backend->gphoto2_port = g_strdup (host + 1);
  gphoto2_backend->gphoto2_port[strlen (gphoto2_backend->gphoto2_port) - 1] = '\0';

  /*g_warning ("decoded host='%s'", gphoto2_backend->gphoto2_port);*/

  find_udi_for_device (gphoto2_backend);

  gphoto2_backend->context = gp_context_new ();
  if (gphoto2_backend->context == NULL)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _I18N_LATER("Cannot create gphoto2 context"));
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }

  rc = gp_camera_new (&(gphoto2_backend->camera));
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_I18N_LATER("Error creating camera"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }


  il = NULL;
  
  rc = gp_port_info_list_new (&il);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_I18N_LATER("Error creating port info list"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }

  rc = gp_port_info_list_load (il);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_I18N_LATER("Error loading info list"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }

  /*g_warning ("gphoto2_port='%s'", gphoto2_backend->gphoto2_port);*/

  n = gp_port_info_list_lookup_path (il, gphoto2_backend->gphoto2_port);
  if (n == GP_ERROR_UNKNOWN_PORT)
    {
      error = get_error_from_gphoto2 (_I18N_LATER("Error looking up port info from port info list"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }

  rc = gp_port_info_list_get_info (il, n, &info);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_I18N_LATER("Error getting port info from port info list"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }

  /*g_warning ("'%s' '%s' '%s'",  info.name, info.path, info.library_filename);*/
  
  /* set port */
  rc = gp_camera_set_port_info (gphoto2_backend->camera, info);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_I18N_LATER("Error setting port info"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      release_device (gphoto2_backend);
      return;
    }
  gp_port_info_list_free(il);

  rc = gp_camera_init (gphoto2_backend->camera, gphoto2_backend->context);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_I18N_LATER("Error initializing camera"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
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

  g_vfs_job_succeeded (G_VFS_JOB (job));

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

  /*g_warning ("mounted %p", gphoto2_backend);*/
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
  GMountSpec *gphoto2_mount_spec;

  /*g_warning ("try_mount %p", backend);*/

  /* TODO: Hmm.. apparently we have to set the mount spec in
   * try_mount(); doing it in mount() do_won't work.. 
   */
  host = g_mount_spec_get (mount_spec, "host");
  /*g_warning ("tm host=%s", host);*/
  if (host == NULL)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED, _I18N_LATER("No camera specified"));
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


static void
do_unmount (GVfsBackend *backend,
            GVfsJobUnmount *job)
{
  GError *error;
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);

  if (gphoto2_backend->num_open_files > 0)
    {
      error = g_error_new (G_IO_ERROR, G_IO_ERROR_BUSY, 
                           _I18N_LATER("File system is busy: %d open files"), gphoto2_backend->num_open_files);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      return;
    }

  release_device (gphoto2_backend);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  //g_warning ("unmounted %p", backend);
}

/* The PTP gphoto2 backend puts an annoying virtual store_00010001
 * directory in the root (in fact 00010001 can be any hexedecimal
 * digit).
 *
 * We want to skip that as the x-content detection expects to find the
 * DCIM/ folder. As such, this function tries to detect the presence
 * of such a folder in the root and, if found, sets a variable that is
 * prepended to any path passed to libgphoto2. This is cached for as
 * long as we got a connection to libgphoto2. If this operation fails
 * then the passed job will be cancelled and this function will return
 * FALSE.
 *
 * IMPORTANT: *ANY* method called by the gvfs core needs to call this
 * function before doing anything. If FALSE is returned the function
 * just needs to return to the gvfs core.
 */
static gboolean
ensure_ignore_prefix (GVfsBackendGphoto2 *gphoto2_backend, GVfsJob *job)
{
  int rc;
  char *prefix;
  GError *error;
  CameraList *list;

  /* already set */
  if (gphoto2_backend->ignore_prefix != NULL)
    return TRUE;

  /* check folders in / - if there is exactly one folder of the form "store_" followed by eight
   * hexadecimal digits.. then use that as ignore_prefix.. otherwise don't use anything 
   */

  gp_list_new (&list);
  rc = gp_camera_folder_list_folders (gphoto2_backend->camera, 
                                      "/", 
                                      list, 
                                      gphoto2_backend->context);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_I18N_LATER("Error listing folders to figure out ignore prefix"), rc);
      g_vfs_job_failed_from_error (job, error);
      return FALSE;
  }  

  prefix = NULL;

  if (gp_list_count (list) == 1)
    {
      char *name;
      const char *s;
      unsigned int n;

      gp_list_get_name (list, 0, &s);

      name = g_ascii_strdown (s, -1);
      if (g_str_has_prefix (name, "store_") && strlen (name) == 14)
        {
          for (n = 6; n < 14; n++)
            {
              if (!g_ascii_isxdigit (name[n]))
                {
                  break;
                }
            }
          if (n == 14)
            {
              prefix = g_strconcat ("/", name, NULL);
            }
        }

      g_free (name);
    }
  gp_list_free (list);

  if (prefix == NULL)
    gphoto2_backend->ignore_prefix = g_strdup ("");
  else
    gphoto2_backend->ignore_prefix = prefix;

  return TRUE;
}

typedef struct {
  CameraFile *file;
  const char *data;
  unsigned long int size;
  unsigned long int cursor;
} ReadHandle;

static void
free_read_handle (ReadHandle *read_handle)
{
  if (read_handle->file != NULL)
    gp_file_unref (read_handle->file);
  g_free (read_handle);
}

static void
do_open_for_read (GVfsBackend *backend,
                  GVfsJobOpenForRead *job,
                  const char *filename)
{
  int rc;
  GError *error;
  ReadHandle *read_handle;
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  char *s;
  char *dir;
  char *name;

  if (!ensure_ignore_prefix (gphoto2_backend, G_VFS_JOB (job)))
    return;

  s = g_path_get_dirname (filename);
  dir = g_strconcat (gphoto2_backend->ignore_prefix, s, NULL);
  g_free (s);
  name = g_path_get_basename (filename);

  /*g_warning ("open_for_read (%s)", filename);*/

  read_handle = g_new0 (ReadHandle, 1);
  rc = gp_file_new (&read_handle->file);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_I18N_LATER("Error creating file object"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      free_read_handle (read_handle);
      goto out;
    }

  rc = gp_camera_file_get (gphoto2_backend->camera,
                           dir,
                           name,
                           GP_FILE_TYPE_NORMAL,
                           read_handle->file,
                           gphoto2_backend->context);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_I18N_LATER("Error getting file"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      free_read_handle (read_handle);
      goto out;
    }

  rc = gp_file_get_data_and_size (read_handle->file, &read_handle->data, &read_handle->size);
  if (rc != 0)
    {
      error = get_error_from_gphoto2 (_I18N_LATER("Error getting data from file"), rc);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      free_read_handle (read_handle);
      goto out;
    }

  /*g_warning ("data=%p size=%ld", read_handle->data, read_handle->size);*/

  gphoto2_backend->num_open_files++;

  read_handle->cursor = 0;

  g_vfs_job_open_for_read_set_can_seek (job, TRUE);
  g_vfs_job_open_for_read_set_handle (job, GINT_TO_POINTER (read_handle));
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_free (name);
  g_free (dir);
}

static void
do_read (GVfsBackend *backend,
         GVfsJobRead *job,
         GVfsBackendHandle handle,
         char *buffer,
         gsize bytes_requested)
{
  //GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  ReadHandle *read_handle = (ReadHandle *) handle;
  gsize bytes_left;
  gsize bytes_to_copy;

  /*g_warning ("do_read (%d @ %ld of %ld)", bytes_requested, read_handle->cursor, read_handle->size);*/

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
}

static void
do_seek_on_read (GVfsBackend *backend,
		 GVfsJobSeekRead *job,
		 GVfsBackendHandle handle,
		 goffset    offset,
		 GSeekType  type)
{
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);
  ReadHandle *read_handle = (ReadHandle *) handle;
  long new_offset;

  /*g_warning ("seek_on_read (%d, %d)", (int)offset, type);*/

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

  if (new_offset < 0 || new_offset >= read_handle->size)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			G_IO_ERROR_FAILED,
			_I18N_LATER("Error seeking in stream on camera %s"), gphoto2_backend->gphoto2_port);
    }
  else
    {
      read_handle->cursor = new_offset;
      
      g_vfs_job_seek_read_set_offset (job, offset);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
}

static void
do_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  ReadHandle *read_handle = (ReadHandle *) handle;
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);

  /*g_warning ("close ()");*/

  free_read_handle (read_handle);

  gphoto2_backend->num_open_files--;
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

/* the passed 'dir' variable contains ignore_prefix */
static gboolean
_set_info (GVfsBackendGphoto2 *gphoto2_backend, const char *dir, const char *name, GFileInfo *info, GError **error)
{
  int rc;
  gboolean ret;
  CameraFileInfo gp_info;
  char *full_path;
  GFileInfo *cached_info;
  GTimeVal mtime;
  char *mime_type;
  GIcon *icon;

  /*g_warning ("_set_info(); dir='%s', name='%s'", dir, name);*/

  ret = FALSE;

  /* look up cache */
  full_path = g_strconcat (dir, "/", name, NULL);
  cached_info = g_hash_table_lookup (gphoto2_backend->info_cache, full_path);
  if (cached_info != NULL)
    {
      /*g_warning ("Using cached info for '%s'", full_path);*/
      g_file_info_copy_into (cached_info, info);
      ret = TRUE;
      goto out;
    }

  rc = gp_camera_file_get_info (gphoto2_backend->camera,
                                dir,
                                name,
                                &gp_info,
                                gphoto2_backend->context);
  if (rc != 0)
    {
      CameraList *list;
      unsigned int n;

      /* gphoto2 doesn't know about this file.. it may be a folder; try that */

      gp_list_new (&list);
      rc = gp_camera_folder_list_folders (gphoto2_backend->camera, 
                                          dir, 
                                          list, 
                                          gphoto2_backend->context);
      if (rc != 0)
        {
          *error = get_error_from_gphoto2 (_I18N_LATER("Error listing folders"), rc);
          goto out;
        }  
      
      for (n = 0; n < gp_list_count (list); n++) 
        {
          const char *folder_name;

          gp_list_get_name (list, n, &folder_name);
          
          /*g_warning ("Looking at folder_name='%s' for dir='%s'", folder_name, dir);*/
          
          if (strcmp (name, folder_name) != 0)
            continue;
          
          /*g_warning ("Got it");*/
          
          g_file_info_set_name (info, name);
          g_file_info_set_display_name (info, name);
          icon = g_themed_icon_new ("folder");
          g_file_info_set_icon (info, icon);
          g_object_unref (icon);
          g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
          g_file_info_set_content_type (info, "inode/directory");
          g_file_info_set_size (info, 0);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE); 
          
          gp_list_free (list);
          ret = TRUE;
          goto add_to_cache;
        }
      gp_list_free (list);

      /* nope, not a folder either.. error out.. */
      
      *error = get_error_from_gphoto2 (_I18N_LATER("Error getting file info"), rc);
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

  icon = g_content_type_get_icon (mime_type);
  /*g_warning ("got icon %p for mime_type '%s'", icon, mime_type);*/
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
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE); 

  ret = TRUE;

 add_to_cache:
  /* add this sucker to the cache */
  if (ret == TRUE)
    {
      g_hash_table_insert (gphoto2_backend->info_cache, g_strdup (full_path), g_file_info_dup (info));
    }

 out:
  g_free (full_path);
  return ret;
}

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

  if (!ensure_ignore_prefix (gphoto2_backend, G_VFS_JOB (job)))
    return;

  /*g_warning ("get_file_info (%s)", filename);*/

  if (strcmp (filename, "/") == 0)
    {
      GIcon *icon;
      char *display_name;
      display_name = compute_display_name (gphoto2_backend);
      g_file_info_set_display_name (info, display_name);
      g_free (display_name);
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_content_type (info, "inode/directory");
      g_file_info_set_size (info, 0);
      icon = g_themed_icon_new ("folder");
      g_file_info_set_icon (info, icon);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE); 
      g_object_unref (icon);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      char *s;
      char *dir;
      char *name;

      s = g_path_get_dirname (filename);
      dir = g_strconcat (gphoto2_backend->ignore_prefix, s, NULL);
      g_free (s);
      name = g_path_get_basename (filename);

      if (!_set_info (gphoto2_backend, dir, name, info, &error))
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
        }
      else
        {
          g_vfs_job_succeeded (G_VFS_JOB (job));
        }

      g_free (name);
      g_free (dir);
    }
  
}

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

  if (!ensure_ignore_prefix (gphoto2_backend, G_VFS_JOB (job)))
    return;

  l = NULL;
  using_cached_dir_list = FALSE;
  using_cached_file_list = FALSE;

  filename = g_strconcat (gphoto2_backend->ignore_prefix, given_filename, NULL);
  /*g_warning ("enumerate (%s) (%s)", given_filename, filename);*/

  /* first, list the folders */
  list = g_hash_table_lookup (gphoto2_backend->dir_name_cache, filename);
  if (list == NULL)
    {
      gp_list_new (&list);
      rc = gp_camera_folder_list_folders (gphoto2_backend->camera, 
                                          filename, 
                                          list, 
                                          gphoto2_backend->context);
      if (rc != 0)
        {
          error = get_error_from_gphoto2 (_I18N_LATER("Error listing folders"), rc);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_free (filename);
          return;
        }  
    }
  else
    {
      /*g_warning ("Using cached dirlist for dir '%s'", filename);*/
      gp_list_ref (list);
      using_cached_dir_list = TRUE;
    }
  for (n = 0; n < gp_list_count (list); n++) 
    {
      const char *name;
      GIcon *icon;

      gp_list_get_name (list, n, &name);

      /*g_warning ("enum '%s'", name);*/

      info = g_file_info_new ();
      g_file_info_set_name (info, name);
      g_file_info_set_display_name (info, name);

      icon = g_themed_icon_new ("folder");
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);

      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_content_type (info, "inode/directory");
      g_file_info_set_size (info, 0);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE); 

      l = g_list_append (l, info);
    }
  if (!using_cached_dir_list)
    {
      gp_list_ref (list);
      g_hash_table_insert (gphoto2_backend->dir_name_cache, g_strdup (filename), list);
    }
  gp_list_unref (list);


  /* then list the files in each folder */
  list = g_hash_table_lookup (gphoto2_backend->file_name_cache, filename);
  if (list == NULL)
    {
      gp_list_new (&list);
      rc = gp_camera_folder_list_files (gphoto2_backend->camera, 
                                        filename, 
                                        list, 
                                        gphoto2_backend->context);
      if (rc != 0)
        {
          error = get_error_from_gphoto2 (_I18N_LATER("Error listing files in folder"), rc);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_free (filename);
          return;
        }
    }
  else
    {
      /*g_warning ("Using cached file list for dir '%s'", filename);*/
      gp_list_ref (list);
      using_cached_file_list = TRUE;
    }
  for (n = 0; n < gp_list_count (list); n++) 
    {
      const char *name;

      gp_list_get_name (list, n, &name);

      info = g_file_info_new ();
      if (!_set_info (gphoto2_backend, filename, name, info, &error))
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_list_foreach (l, (GFunc) g_object_unref, NULL);
          g_list_free (l);
          gp_list_free (list);
          return;
        }
      l = g_list_append (l, info);
    }
  if (!using_cached_file_list)
    {
      gp_list_ref (list);
      g_hash_table_insert (gphoto2_backend->file_name_cache, g_strdup (filename), list);
    }
  gp_list_unref (list);

  /* and we're done */

  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_vfs_job_enumerate_add_infos (job, l);
  g_list_foreach (l, (GFunc) g_object_unref, NULL);
  g_list_free (l);
  g_vfs_job_enumerate_done (job);

  g_free (filename);
}

static void
do_query_fs_info (GVfsBackend *backend,
		  GVfsJobQueryFsInfo *job,
		  const char *filename,
		  GFileInfo *info,
		  GFileAttributeMatcher *attribute_matcher)
{
  int rc;
  GVfsBackendGphoto2 *gphoto2_backend = G_VFS_BACKEND_GPHOTO2 (backend);

  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "gphoto2");
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, TRUE);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_IF_LOCAL);

  int num_storage_info;
  CameraStorageInformation *storage_info;

  rc = gp_camera_get_storageinfo (gphoto2_backend->camera, &storage_info, &num_storage_info, gphoto2_backend->context);
  if (rc == 0)
    {
      if (num_storage_info >= 1)
        {

          /*g_warning ("capacity = %ld", storage_info[0].capacitykbytes);*/
          /*g_warning ("free = %ld", storage_info[0].freekbytes);*/

          /* for now we only support a single storage head */
          if (storage_info[0].fields & GP_STORAGEINFO_MAXCAPACITY)
            {
              g_file_info_set_attribute_uint64 (info, 
                                                G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, 
                                                storage_info[0].capacitykbytes * 1024);
            }
          if (storage_info[0].fields & GP_STORAGEINFO_FREESPACEKBYTES)
            {
              g_file_info_set_attribute_uint64 (info, 
                                                G_FILE_ATTRIBUTE_FILESYSTEM_FREE, 
                                                storage_info[0].freekbytes * 1024);
            }
          
        }
      /*g_warning ("got %d storage_info objects", num_storage_info);*/
    }

  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
g_vfs_backend_gphoto2_class_init (GVfsBackendGphoto2Class *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_gphoto2_finalize;

  backend_class->try_mount = try_mount;
  backend_class->mount = do_mount;
  backend_class->unmount = do_unmount;
  backend_class->open_for_read = do_open_for_read;
  backend_class->read = do_read;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->close_read = do_close_read;
  backend_class->query_info = do_query_info;
  backend_class->enumerate = do_enumerate;
  backend_class->query_fs_info = do_query_fs_info;
}
