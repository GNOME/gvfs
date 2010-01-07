/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2008 Red Hat, Inc.
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
 * Authors: Bastien Nocera <hadess@hadess.net>
 *          Cosimo Cecchi  <cosimoc@gnome.org>
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
#if defined(HAVE_HAL)
  #include <libhal.h>
#endif	
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-bindings.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <bluetooth/bluetooth.h>

#include "gvfsbackendobexftp.h"
#include "gvfsbackendobexftp-fl-parser.h"
#include "gvfsbackendobexftp-cap-parser.h"
#include "obexftp-marshal.h"

#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobmount.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"

#define BDADDR_LEN 17

#define ASYNC_SUCCESS 2
#define ASYNC_RUNNING 1
#define ASYNC_PENDING 0
#define ASYNC_ERROR -1

#define CACHE_LIFESPAN 3

struct _GVfsBackendObexftp
{
  GVfsBackend parent_instance;

  char *display_name;
  char *bdaddr;
  char *icon_name;
  gint usbintfnum;

  DBusGConnection *connection;
  DBusGProxy *manager_proxy;
  DBusGProxy *session_proxy;

  /* Use for the async notifications and errors */
  GCond *cond;
  GMutex *mutex;
  int status;
  gboolean doing_io;
  GError *error;

  /* Folders listing cache */
  char *files_listing;
  char *directory;
  time_t time_captured;
};

typedef struct {
    char *source;
    goffset size;
    int fd;
} ObexFTPOpenHandle;

G_DEFINE_TYPE (GVfsBackendObexftp, g_vfs_backend_obexftp, G_VFS_TYPE_BACKEND);

static void session_connect_error_cb (DBusGProxy *proxy,
                                      const char *session_object,
                                      const gchar *error_name,
                                      const gchar *error_message,
                                      gpointer user_data);
static void session_connected_cb (DBusGProxy *proxy,
                                  const char *session_object,
                                  gpointer user_data);

/* Parse USB paths from do_mount(), or obex-data-server */
static gboolean
_get_numbers_from_usb_path (const char *path, int *usb_bus_num, int *usb_device_num, int *usb_intf_num)
{
  char **tokens;
  char *endp;
  gboolean has_brackets = FALSE;

  if (path == NULL)
    return FALSE;
  if (*path == '[')
    {
      path++;
      has_brackets = TRUE;
    }

  tokens = g_strsplit (path + 4, ",", 0);
  if (g_strv_length (tokens) != 3)
    {
      g_strfreev (tokens);
      return FALSE;
    }

 *usb_bus_num = strtol (tokens[0], &endp, 10);
  if (*endp != '\0')
    {
      g_strfreev (tokens);
      return FALSE;
    }

  *usb_device_num = strtol (tokens[1], &endp, 10);
  if (*endp != '\0')
    {
      g_strfreev (tokens);
      return FALSE;
    }

  *usb_intf_num = strtol (tokens[2], &endp, 10);
  if ((has_brackets && *endp != ']') || (!has_brackets && *endp != '\0'))
    {
      g_strfreev (tokens);
      return FALSE;
    }

  g_strfreev (tokens);

  return TRUE;
}

/* Used to detect broken listings from
 * old Nokia 3650s */
static gboolean
_is_nokia_3650 (const char *bdaddr)
{
  if (!bdaddr)
    return FALSE;

  /* Don't ask, Nokia seem to use a Bluetooth
   * HCI from Murata */
  return g_str_has_prefix(bdaddr, "00:60:57");
}

static char *
_get_bluetooth_name_and_icon (DBusGProxy *device, char **icon_name)
{
  GHashTable *hash;

  if (dbus_g_proxy_call (device, "GetProperties", NULL,
                         G_TYPE_INVALID, dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_VALUE),
                         &hash, G_TYPE_INVALID) != FALSE)
    {
      GValue *value;
      char *name;

      value = g_hash_table_lookup (hash, "Alias");
      name = value ? g_value_dup_string(value) : NULL;

      value = g_hash_table_lookup (hash, "Icon");
      if (value)
        {
          *icon_name = g_value_dup_string (value);
        }
      else
        {
          *icon_name = g_strdup ("bluetooth");
        }
      g_hash_table_destroy (hash);
      return name;
    }

  return NULL;
}


#define _DBUS_POINTER_SHIFT(p)   ((void*) (((char*)p) + sizeof (void*)))
#define DBUS_G_CONNECTION_FROM_CONNECTION(x)     ((DBusGConnection*) _DBUS_POINTER_SHIFT(x))

static gchar *
_get_bluetooth_device_properties (const char *bdaddr, char **icon_name)
{
  DBusConnection *conn;
  DBusGProxy *manager;
  GPtrArray *adapters;
  gchar *name;
  guint i;

  name = NULL;

  conn = dbus_bus_get_private (DBUS_BUS_SYSTEM, NULL);
  if (conn == NULL)
        return name;

  manager = dbus_g_proxy_new_for_name (DBUS_G_CONNECTION_FROM_CONNECTION(conn), "org.bluez",
                                       "/", "org.bluez.Manager");
  if (manager == NULL)
    {
      dbus_connection_close (conn);
      dbus_connection_unref (conn);
      return name;
    }

  if (dbus_g_proxy_call (manager, "ListAdapters", NULL, G_TYPE_INVALID, dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_OBJECT_PATH), &adapters, G_TYPE_INVALID) == FALSE)
    {
      g_object_unref (manager);
      dbus_connection_close (conn);
      dbus_connection_unref (conn);
      return name;
    }

  for (i = 0; i < adapters->len && name == NULL; i++)
    {
      DBusGProxy *adapter;
      char *device_path;

      adapter = dbus_g_proxy_new_for_name (DBUS_G_CONNECTION_FROM_CONNECTION(conn), "org.bluez",
                                           g_ptr_array_index (adapters, i), "org.bluez.Adapter");
      if (dbus_g_proxy_call (adapter, "FindDevice", NULL,
                             G_TYPE_STRING, bdaddr, G_TYPE_INVALID,
                             DBUS_TYPE_G_OBJECT_PATH, &device_path, G_TYPE_INVALID) != FALSE)
        {
          DBusGProxy *device;
          device = dbus_g_proxy_new_for_name (DBUS_G_CONNECTION_FROM_CONNECTION(conn), "org.bluez", device_path, "org.bluez.Device");
          name = _get_bluetooth_name_and_icon (device, icon_name);
          g_object_unref (device);
        }
      g_object_unref (adapter);
    }

  g_ptr_array_free (adapters, TRUE);
  g_object_unref (manager);
  dbus_connection_close (conn);
  dbus_connection_unref (conn);

  return name;
}

static gboolean
_is_same_path (const char *path, int usb_bus_num, int usb_device_num, int usb_intf_num)
{
  int bus, dev, intf;

  if (!_get_numbers_from_usb_path (path, &bus, &dev, &intf))
    return FALSE;

  if (bus == usb_bus_num &&
      dev == usb_device_num &&
      intf == usb_intf_num)
        return TRUE;

  return FALSE;
}

static int
_find_ods_usb_intfnum (DBusGProxy *obex_manager, int device_usb_bus_num, int device_usb_device_num, int device_usb_interface_num)
{
  int i, n;
  GHashTable *hash;

  if (obex_manager == NULL)
    return -1;

  if (dbus_g_proxy_call (obex_manager, "GetUsbInterfacesNum", NULL, G_TYPE_INVALID, G_TYPE_UINT, &n, G_TYPE_INVALID) == FALSE)
    return -1;

  for (i = 0; i < n; i++) 
    {
      if (dbus_g_proxy_call (obex_manager, "GetUsbInterfaceInfo", NULL,
                             G_TYPE_UINT, i, G_TYPE_INVALID,
                             DBUS_TYPE_G_STRING_STRING_HASHTABLE, &hash, 
                             G_TYPE_INVALID) != FALSE)
        {
          const char* ods_path = g_hash_table_lookup (hash, "Path");

          if (ods_path == NULL)
            {
              g_hash_table_destroy (hash);
              return -2;
            }

          if (_is_same_path (ods_path, device_usb_bus_num, device_usb_device_num, device_usb_interface_num))
            {
              g_hash_table_destroy (hash);
              return i;
            }
          g_hash_table_destroy (hash);
        }
    }
  return -1;
}

static gint
_get_usb_intfnum_and_properties (DBusGProxy *obex_manager, const char *device, char **display_name, char **icon_name)
{
  char **obex_devices;
  int num_obex_devices;
  int usb_bus_num;
  int usb_device_num;
  int usb_intf_num;
#ifdef HAVE_HAL
  DBusError dbus_error;
  DBusConnection *dbus_connection;
  LibHalContext *hal_ctx;
#endif
  int ods_intf_num = 1;
  int n;

  /* Parse the [usb:1,41,3] string */
  if (!g_str_has_prefix (device, "[usb:"))
    {
      return -1;
    }

  if (!_get_numbers_from_usb_path (device, &usb_bus_num, &usb_device_num, &usb_intf_num))
    return -1;

  g_message ("Parsed '%s' into bus=%d device=%d interface=%d", device, usb_bus_num, usb_device_num, usb_intf_num);

#ifdef HAVE_HAL
  dbus_error_init (&dbus_error);
  dbus_connection = dbus_bus_get_private (DBUS_BUS_SYSTEM, &dbus_error);
  if (dbus_error_is_set (&dbus_error))
    {
      dbus_error_free (&dbus_error);
      return -1;
    }
  dbus_connection_set_exit_on_disconnect (dbus_connection, FALSE);

  hal_ctx = libhal_ctx_new ();
  if (hal_ctx == NULL)
    {
      dbus_connection_close (dbus_connection);
      dbus_connection_unref (dbus_connection);
      dbus_error_free (&dbus_error);
      return -1;
    }

  libhal_ctx_set_dbus_connection (hal_ctx, dbus_connection);
  if (!libhal_ctx_init (hal_ctx, &dbus_error))
    {
      libhal_ctx_free (hal_ctx);
      dbus_connection_close (dbus_connection);
      dbus_connection_unref (dbus_connection);
      dbus_error_free (&dbus_error);
      return -1;
    }
  obex_devices = libhal_find_device_by_capability (hal_ctx, "obex", &num_obex_devices, NULL);

  for (n = 0; n < num_obex_devices; n++)
    {
      char *udi = obex_devices[n];
      LibHalPropertySet *ps;

      ps = libhal_device_get_all_properties (hal_ctx, udi, NULL);
      if (ps != NULL)
        {
          const char *subsystem;

          subsystem = libhal_ps_get_string (ps, "info.subsystem");
          if (subsystem != NULL && strcmp (subsystem, "usb") == 0)
            {
              int device_usb_bus_num;
              int device_usb_device_num;
              int device_usb_interface_num;
              const char *icon_from_hal;
              const char *name_from_hal;

              device_usb_bus_num = libhal_ps_get_int32 (ps, "usb.bus_number");
              device_usb_device_num = libhal_ps_get_int32 (ps, "usb.linux.device_number");
              device_usb_interface_num = libhal_ps_get_int32 (ps, "usb.interface.number");
              icon_from_hal = libhal_ps_get_string (ps, "info.desktop.icon");
              name_from_hal = libhal_ps_get_string (ps, "info.desktop.name");

              g_message ("looking at usb device '%s' with bus=%d, device=%d interface=%d", 
                     udi, device_usb_bus_num, device_usb_device_num, device_usb_interface_num);

              if (device_usb_bus_num == usb_bus_num &&
                  device_usb_device_num == usb_device_num &&
                  device_usb_interface_num == usb_intf_num)
                {
                  const char *parent_udi;

                  g_message ("udi '%s' matches %s", udi, device);
                  parent_udi = libhal_ps_get_string (ps, "info.parent");

                  if (parent_udi != NULL)
                    {
                      LibHalPropertySet *ps2;

                      ps2 = libhal_device_get_all_properties (hal_ctx, parent_udi, NULL);
                      if (ps2 != NULL)
                        {
                          ods_intf_num = _find_ods_usb_intfnum (obex_manager,
                                                                device_usb_bus_num,
                                                                device_usb_device_num,
                                                                device_usb_interface_num);

                          if (ods_intf_num >= 0)
                            {
                              if (icon_from_hal != NULL)
                                *icon_name = g_strdup (icon_from_hal);
                              else
                                *icon_name = "drive-removable-media-usb";

                              if (name_from_hal != NULL)
                                *display_name = g_strdup (name_from_hal);
                              else
                                *display_name = g_strdup_printf ("%s - %s",
                                                                 libhal_ps_get_string (ps2, "usb_device.vendor"),
                                                                 libhal_ps_get_string (ps2, "usb_device.product"));

                              libhal_free_property_set (ps2);
                              libhal_free_property_set (ps);

                              goto end;
                            }
                          else if (ods_intf_num == -2)
                            {
                              libhal_free_property_set (ps2);
                              libhal_free_property_set (ps);

                              goto end;
                            }
                          libhal_free_property_set (ps2);
                        }
                    }
                }
            }
          libhal_free_property_set (ps);
        }
    }

end:
  libhal_free_string_array (obex_devices);
  libhal_ctx_free (hal_ctx);

  dbus_connection_close (dbus_connection);
  dbus_connection_unref (dbus_connection);
  dbus_error_free (&dbus_error);
#endif /* HAVE_HAL */

  return ods_intf_num;
}

static void
g_vfs_backend_obexftp_finalize (GObject *object)
{
  GVfsBackendObexftp *backend;

  backend = G_VFS_BACKEND_OBEXFTP (object);

  g_free (backend->display_name);
  g_free (backend->bdaddr);
  g_free (backend->icon_name);
  g_free (backend->files_listing);
  g_free (backend->directory);

  if (backend->session_proxy != NULL)
        g_object_unref (backend->session_proxy);
  g_mutex_free (backend->mutex);
  g_cond_free (backend->cond);

  if (G_OBJECT_CLASS (g_vfs_backend_obexftp_parent_class)->finalize)
        (*G_OBJECT_CLASS (g_vfs_backend_obexftp_parent_class)->finalize) (object);
}

static void
g_vfs_backend_obexftp_init (GVfsBackendObexftp *backend)
{
  DBusConnection *conn;
  DBusGConnection *connection;
  DBusError error;
  GError *err = NULL;

  /* Otherwise dbus-glib doesn't setup it value types */
  connection = dbus_g_bus_get (DBUS_BUS_SESSION, NULL);

  if (connection != NULL)
        dbus_g_connection_unref (connection);

  /* Connect to the session bus via a private connection */
  dbus_error_init (&error);
  conn = dbus_bus_get_private (DBUS_BUS_SESSION, &error);
  if (conn == NULL) {
      g_printerr ("Connecting to session bus failed: %s\n", err->message);
      dbus_error_free (&error);
      return;
  }
  dbus_connection_setup_with_g_main (conn, NULL);

  backend->connection = dbus_connection_get_g_connection (conn);
  if (backend->connection == NULL) {
      g_printerr ("Connecting to session bus failed: %s\n", err->message);
      g_error_free (err);
      return;
  }

  backend->mutex = g_mutex_new ();
  backend->cond = g_cond_new ();
  backend->manager_proxy = dbus_g_proxy_new_for_name (backend->connection,
                                                      "org.openobex",
                                                      "/org/openobex",
                                                      "org.openobex.Manager");

  dbus_g_proxy_add_signal(backend->manager_proxy, "SessionConnectError",
                          DBUS_TYPE_G_OBJECT_PATH, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INVALID);
  dbus_g_proxy_connect_signal(backend->manager_proxy, "SessionConnectError",
                              G_CALLBACK(session_connect_error_cb), backend, NULL);
  dbus_g_proxy_add_signal(backend->manager_proxy, "SessionConnected",
                          DBUS_TYPE_G_OBJECT_PATH, G_TYPE_INVALID);
  dbus_g_proxy_connect_signal(backend->manager_proxy, "SessionConnected",
                              G_CALLBACK(session_connected_cb), backend, NULL);
}

static gboolean
_change_directory (GVfsBackendObexftp *op_backend,
                     const char *filename,
                     GError **error)
{
  char *current_path, **req_components;
  guint i;

  if (dbus_g_proxy_call (op_backend->session_proxy, "GetCurrentPath", error,
                         G_TYPE_INVALID,
                         G_TYPE_STRING, &current_path, G_TYPE_INVALID) == FALSE)
    {
      g_message ("GetCurrentPath failed");
      return FALSE;
    }

  if (strcmp (filename, current_path) == 0)
    {
      g_free (current_path);
      return TRUE;
    }

  /* Are we already at the root? */
  if (strcmp (current_path, "/") != 0)
    {
      if (dbus_g_proxy_call (op_backend->session_proxy, "ChangeCurrentFolderToRoot", error,
                             G_TYPE_INVALID,
                             G_TYPE_INVALID) == FALSE)
        {
          g_message ("ChangeCurrentFolderToRoot failed");
          //FIXME change the retval from org.openobex.Error.NotAuthorized to
          //no such file or directory
          return FALSE;
        }
    }
  g_free (current_path);

  /* If we asked for the root, we're done */
  if (strcmp (filename, "/") == 0)
        return TRUE;

  req_components = g_strsplit (filename, "/", -1);
  for (i = 0; req_components[i] != NULL; i++)
    {
      if (*req_components[i] == '\0')
            continue;
      if (dbus_g_proxy_call (op_backend->session_proxy, "ChangeCurrentFolder", error,
                             G_TYPE_STRING, req_components[i], G_TYPE_INVALID,
                             G_TYPE_INVALID) == FALSE)
        {
          g_message ("ChangeCurrentFolder failed");
          g_strfreev (req_components);
          return FALSE;
        }
    }

  g_strfreev (req_components);

  return TRUE;
}

static gboolean
_retrieve_folder_listing (GVfsBackend *backend,
                          const char *filename,
                          char **files,
                          GError **error)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (backend);
  time_t current;

  current = time (NULL);

  if (op_backend->directory != NULL &&
      strcmp (op_backend->directory, filename) == 0 &&
      op_backend->time_captured > current - CACHE_LIFESPAN)
    {
      *files = g_strdup (op_backend->files_listing);
      return TRUE;
    }
  else
    {
      g_free (op_backend->directory);
      op_backend->directory = NULL;
      g_free (op_backend->files_listing);
      op_backend->files_listing = NULL;
    }

  if (dbus_g_proxy_call (op_backend->session_proxy, "RetrieveFolderListing", error,
                         G_TYPE_INVALID,
                         G_TYPE_STRING, files, G_TYPE_INVALID) == FALSE)
    {
      return FALSE;
    }

  op_backend->directory = g_strdup (filename);
  op_backend->files_listing = g_strdup (*files);
  op_backend->time_captured = time (NULL);

  return TRUE;
}

static gboolean
_query_file_info_helper (GVfsBackend *backend,
                         const char *filename,
                         GFileInfo *info,
                         GError **error)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (backend);
  char *parent, *basename, *files;
  GList *elements, *l;
  gboolean found;

  g_debug ("+ _query_file_info_helper, filename: %s\n", filename);

  if (strcmp (filename, "/") == 0)
    {
      char *display;

      /* That happens when you want '/'
       * and we don't have any info about it :( */
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_content_type (info, "inode/directory");
      g_file_info_set_name (info, "/");
      if (op_backend->icon_name) {
          GIcon *icon;

          g_vfs_backend_set_icon_name (backend, op_backend->icon_name);
          icon = g_themed_icon_new (op_backend->icon_name);
          g_file_info_set_icon (info, icon);
          g_object_unref (icon);
      }
      display = g_strdup_printf (_("%s on %s"), "/", op_backend->display_name);
      g_file_info_set_display_name (info, display);
      g_free (display);
      return TRUE;
    }

  parent = g_path_get_dirname (filename);
  if (_change_directory (op_backend, parent, error) == FALSE)
    {
      g_free (parent);
      return FALSE;
    }

  files = NULL;
  if (_retrieve_folder_listing (backend, parent, &files, error) == FALSE)
    {
      g_free (parent);
      return FALSE;
    }

  g_free (parent);

  if (gvfsbackendobexftp_fl_parser_parse (files, strlen (files), &elements, error) == FALSE)
    {
      g_free (files);
      return FALSE;
    }
  g_free (files);

  basename = g_path_get_basename (filename);
  found = FALSE;

  for (l = elements; l != NULL; l = l->next)
    {
      if (strcmp (basename, g_file_info_get_name (l->data)) == 0)
        {
          g_file_info_copy_into (l->data, info);
          found = TRUE;
          break;
        }
    }

  if (found == FALSE)
    {
      g_set_error_literal (error, G_IO_ERROR,
	                   G_IO_ERROR_NOT_FOUND,
        	           g_strerror (ENOENT));
    }

  g_free (basename);
  g_list_foreach (elements, (GFunc)g_object_unref, NULL);
  g_list_free (elements);

  g_debug ("- _query_file_info_helper\n");

  return found;
}

static void
_invalidate_cache_helper (GVfsBackendObexftp *op_backend)
{
  g_free (op_backend->directory);
  op_backend->directory = NULL;
}

static void
error_occurred_cb (DBusGProxy *proxy, const gchar *error_name, const gchar *error_message, gpointer user_data)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (user_data);

  g_message("ErrorOccurred");
  g_message("Error name: %s", error_name);
  g_message("Error message: %s", error_message);

  if (strcmp (error_name, "org.openobex.Error.LinkError") == 0)
    {
      g_message ("link lost to remote device");
      _exit (1);
    }

  /* Something is waiting on us */
  g_mutex_lock (op_backend->mutex);
  if (op_backend->doing_io)
    {
      op_backend->status = ASYNC_ERROR;
      op_backend->error = g_error_new_literal (DBUS_GERROR,
                                               DBUS_GERROR_REMOTE_EXCEPTION,
                                               error_message);
      g_cond_signal (op_backend->cond);
      g_mutex_unlock (op_backend->mutex);
      return;
    }
  g_mutex_unlock (op_backend->mutex);

  g_message ("Unhandled error, file a bug");
  _exit (1);
}

static void
session_connect_error_cb (DBusGProxy *proxy,
                          const char *session_object,
                          const gchar *error_name,
                          const gchar *error_message,
                          gpointer user_data)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (user_data);

  g_mutex_lock (op_backend->mutex);
  op_backend->status = ASYNC_ERROR;
  op_backend->error = g_error_new_literal (DBUS_GERROR,
                                           DBUS_GERROR_REMOTE_EXCEPTION,
                                           error_message);
  g_cond_signal (op_backend->cond);
  g_mutex_unlock (op_backend->mutex);
}

static void
session_connected_cb (DBusGProxy *proxy,
                      const char *session_object,
                      gpointer user_data)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (user_data);

  g_mutex_lock (op_backend->mutex);
  op_backend->status = ASYNC_SUCCESS;
  g_cond_signal (op_backend->cond);
  g_mutex_unlock (op_backend->mutex);
}

static void
cancelled_cb (DBusGProxy *proxy, gpointer user_data)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (user_data);

  g_message ("transfer got cancelled");

  g_mutex_lock (op_backend->mutex);
  op_backend->status = ASYNC_ERROR;
  g_cond_signal (op_backend->cond);
  g_mutex_unlock (op_backend->mutex);
}

static void
disconnected_cb (DBusGProxy *proxy, gpointer user_data)
{
  g_message ("disconnected_cb");

  _exit (1);
}

static void
closed_cb (DBusGProxy *proxy, gpointer user_data)
{
  g_message ("closed_cb");

  _exit (1);
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (backend);
  const char *device;
  GError *error = NULL;
  const gchar *path = NULL;
  GMountSpec *obexftp_mount_spec;
  guint count;

  g_debug ("+ do_mount\n");

  device = g_mount_spec_get (mount_spec, "host");

  if (device == NULL || (strlen (device) != BDADDR_LEN + 2 && !g_str_has_prefix(device, "[usb:")) )
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Invalid mount spec"));
      return;
    }

  op_backend->bdaddr = NULL;
  op_backend->usbintfnum = -1;

  if (!g_str_has_prefix(device, "[usb:"))
    {
      /* Strip the brackets */
      op_backend->bdaddr = g_strndup (device + 1, BDADDR_LEN);
      if (bachk (op_backend->bdaddr) < 0)
        {
          g_free (op_backend->bdaddr);
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            _("Invalid mount spec"));
          return;
        }
    }
  else
    {
      op_backend->usbintfnum = _get_usb_intfnum_and_properties (op_backend->manager_proxy, device, &op_backend->display_name, &op_backend->icon_name);
      if (op_backend->usbintfnum < 0)
        {
          if (op_backend->usbintfnum == -2)
            {
              g_vfs_job_failed (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                _("USB support missing. Please contact your software vendor"));
            }
          else
            {
              g_vfs_job_failed (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                _("Invalid mount spec"));
            }
          return;
        }
    }

  /* FIXME, Have a way for the mount to be cancelled, see:
   * Use CancelSessionConnect */
  op_backend->status = ASYNC_PENDING;

  if (op_backend->bdaddr)
    {
      if (dbus_g_proxy_call (op_backend->manager_proxy, "CreateBluetoothSession", &error,
                             G_TYPE_STRING, op_backend->bdaddr, G_TYPE_STRING, "00:00:00:00:00:00", G_TYPE_STRING, "ftp", G_TYPE_INVALID,
                             DBUS_TYPE_G_OBJECT_PATH, &path, G_TYPE_INVALID) == FALSE)
        {
          g_free (op_backend->bdaddr);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          return;
        }
      op_backend->display_name = _get_bluetooth_device_properties (op_backend->bdaddr, &op_backend->icon_name);
      if (!op_backend->display_name)
        op_backend->display_name = g_strdelimit (g_strdup (op_backend->bdaddr), ":", '-');
      if (!op_backend->icon_name)
        op_backend->icon_name = g_strdup ("bluetooth");
      g_debug ("  do_mount: %s (%s) mounted\n", op_backend->display_name, op_backend->bdaddr);
    }
  else
    {
      if (dbus_g_proxy_call (op_backend->manager_proxy, "CreateUsbSession", &error,
                             G_TYPE_UINT, op_backend->usbintfnum, G_TYPE_STRING, "ftp", G_TYPE_INVALID,
                             DBUS_TYPE_G_OBJECT_PATH, &path, G_TYPE_INVALID) == FALSE)
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          return;
        }
      g_debug ("  do_mount: usb interface %d mounted\n", op_backend->usbintfnum);
    }

  g_vfs_job_set_backend_data (G_VFS_JOB (job), backend, NULL);

  op_backend->session_proxy = dbus_g_proxy_new_for_name (op_backend->connection,
                                                         "org.openobex",
                                                         path,
                                                         "org.openobex.Session");

  g_vfs_backend_set_display_name (G_VFS_BACKEND  (op_backend),
                                  op_backend->display_name);
  g_vfs_backend_set_icon_name (G_VFS_BACKEND (op_backend), op_backend->icon_name);

  obexftp_mount_spec = g_mount_spec_new ("obex");
  g_mount_spec_set (obexftp_mount_spec, "host", device);
  g_vfs_backend_set_mount_spec (G_VFS_BACKEND (op_backend), obexftp_mount_spec);
  g_mount_spec_unref (obexftp_mount_spec);

  dbus_g_proxy_add_signal(op_backend->session_proxy, "ErrorOccurred",
                          G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INVALID);
  dbus_g_proxy_connect_signal(op_backend->session_proxy, "ErrorOccurred",
                              G_CALLBACK(error_occurred_cb), op_backend, NULL);

  dbus_g_proxy_add_signal(op_backend->session_proxy, "Cancelled",
                          G_TYPE_INVALID);
  dbus_g_proxy_connect_signal(op_backend->session_proxy, "Cancelled",
                              G_CALLBACK(cancelled_cb), op_backend, NULL);

  dbus_g_proxy_add_signal(op_backend->session_proxy, "Disconnected",
                          G_TYPE_INVALID);
  dbus_g_proxy_connect_signal(op_backend->session_proxy, "Disconnected",
                              G_CALLBACK(disconnected_cb), op_backend, NULL);

  dbus_g_proxy_add_signal(op_backend->session_proxy, "Closed",
                          G_TYPE_INVALID);
  dbus_g_proxy_connect_signal(op_backend->session_proxy, "Closed",
                              G_CALLBACK(closed_cb), op_backend, NULL);

  dbus_g_proxy_add_signal(op_backend->session_proxy, "TransferStarted",
                          G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64, G_TYPE_INVALID);
  dbus_g_proxy_add_signal(op_backend->session_proxy, "TransferCompleted",
                          G_TYPE_INVALID);
  dbus_g_proxy_add_signal(op_backend->session_proxy, "TransferProgress",
                          G_TYPE_UINT64, G_TYPE_INVALID);

  /* Now wait until the device is connected */
  count = 0;
  g_mutex_lock (op_backend->mutex);

  while (op_backend->status == ASYNC_PENDING && count < 100) {
      GTimeVal val;
      g_get_current_time (&val);
      g_time_val_add (&val, 100000);
      count++;
      if (g_cond_timed_wait (op_backend->cond, op_backend->mutex, &val) != FALSE)
            break;
  }
  g_mutex_unlock (op_backend->mutex);

  if (op_backend->status == ASYNC_ERROR || op_backend->status == ASYNC_PENDING)
    {
      g_message ("mount failed, didn't connect");

      g_free (op_backend->display_name);
      op_backend->display_name = NULL;
      g_free (op_backend->bdaddr);
      op_backend->bdaddr = NULL;
      g_object_unref (op_backend->session_proxy);
      op_backend->session_proxy = NULL;

      if (op_backend->status != ASYNC_PENDING)
            g_vfs_job_failed_from_error (G_VFS_JOB (job), op_backend->error);
      else
            g_vfs_job_failed (G_VFS_JOB (job),
                              G_IO_ERROR, G_IO_ERROR_BUSY,
                              _("Connection to the device lost"));
      return;
    }

  op_backend->status = ASYNC_PENDING;

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_debug ("- do_mount\n");
}

static void
transfer_started_cb (DBusGProxy *proxy, const gchar *filename,
                     const gchar *local_path, guint64 byte_count, gpointer user_data)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (user_data);

  g_message ("transfer of %s to %s started", filename, local_path);

  g_mutex_lock (op_backend->mutex);
  op_backend->status = ASYNC_SUCCESS;
  g_cond_signal (op_backend->cond);
  g_mutex_unlock (op_backend->mutex);
}

static void
do_open_for_read (GVfsBackend *backend,
                  GVfsJobOpenForRead *job,
                  const char *filename)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (backend);
  GError *error = NULL;
  ObexFTPOpenHandle *handle;
  char *target, *basename;
  GFileInfo *info;
  goffset size;
  int fd, success;

  g_debug ("+ do_open_for_read, filename: %s\n", filename);

  g_mutex_lock (op_backend->mutex);
  op_backend->doing_io = TRUE;

  /* Change into the directory and cache the file size */
  info = g_file_info_new ();
  if (_query_file_info_helper (backend, filename, info, &error) == FALSE)
    {
      op_backend->doing_io = FALSE;
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      g_object_unref (info);
      return;
    }
  /* If we're trying to open a directory for reading, exit out */
  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
      op_backend->doing_io = FALSE;
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_IS_DIRECTORY,
                        _("Can't open directory"));
      g_object_unref (info);
      return;
    }

  size = g_file_info_get_size (info);
  g_object_unref (info);

  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
    {
      op_backend->doing_io = FALSE;
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_CANCELLED,
                        _("Operation was cancelled"));
      return;
    }

  fd = g_file_open_tmp ("gvfsobexftp-tmp-XXXXXX",
                        &target, &error);
  if (fd < 0)
    {
      op_backend->doing_io = FALSE;
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
    {
      op_backend->doing_io = FALSE;
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                                   G_IO_ERROR_CANCELLED,
                                   _("Operation was cancelled"));
      return;
    }

  op_backend->status = ASYNC_PENDING;

  dbus_g_proxy_connect_signal(op_backend->session_proxy, "TransferStarted",
                              G_CALLBACK(transfer_started_cb), op_backend, NULL);

  basename = g_path_get_basename (filename);
  if (dbus_g_proxy_call (op_backend->session_proxy, "CopyRemoteFile", &error,
                         G_TYPE_STRING, basename,
                         G_TYPE_STRING, target,
                         G_TYPE_INVALID,
                         G_TYPE_INVALID) == FALSE)
    {
      g_message ("CopyRemoteFile failed");

      g_free (basename);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);

      dbus_g_proxy_disconnect_signal(op_backend->session_proxy, "TransferStarted",
                                     G_CALLBACK(transfer_started_cb), op_backend);

      /* Close the target */
      g_unlink (target);
      g_free (target);
      close (fd);

      op_backend->doing_io = FALSE;
      g_mutex_unlock (op_backend->mutex);
      return;
    }

  /* Wait for TransferStarted or ErrorOccurred to have happened */
  while (op_backend->status == ASYNC_PENDING)
        g_cond_wait (op_backend->cond, op_backend->mutex);
  success = op_backend->status;
  dbus_g_proxy_disconnect_signal(op_backend->session_proxy, "TransferStarted",
                                 G_CALLBACK(transfer_started_cb), op_backend);

  /* We either got success or an async error */
  g_assert (success != ASYNC_PENDING);

  g_message ("filename: %s (%s) copying to %s (retval %d)", filename, basename, target, success);
  g_free (basename);

  g_unlink (target);
  g_free (target);
  op_backend->status = ASYNC_PENDING;

  if (success == ASYNC_ERROR)
    {
      op_backend->doing_io = FALSE;
      g_mutex_unlock (op_backend->mutex);
      close (fd);
      g_vfs_job_failed_from_error (G_VFS_JOB (job),
                                   op_backend->error);
      g_error_free (op_backend->error);
      op_backend->error = NULL;
      return;
    }

  handle = g_new0 (ObexFTPOpenHandle, 1);
  handle->source = g_strdup (filename);
  handle->fd = fd;
  handle->size = size;
  g_vfs_job_open_for_read_set_handle (job, handle);

  g_debug ("- do_open_for_read, filename: %s\n", filename);

  g_vfs_job_open_for_read_set_can_seek (G_VFS_JOB_OPEN_FOR_READ (job), FALSE);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  op_backend->doing_io = FALSE;
  g_mutex_unlock (op_backend->mutex);
}

static int
is_busy (DBusGProxy *session_proxy, GVfsJob *job)
{
  GError *error = NULL;
  gboolean busy;

  if (dbus_g_proxy_call (session_proxy, "IsBusy", &error,
                         G_TYPE_INVALID,
                         G_TYPE_BOOLEAN, &busy, G_TYPE_INVALID) == FALSE)
    {
      g_vfs_job_failed_from_error (job, error);
      g_error_free (error);
      return -1;
    }

  return busy;
}

static void
do_read (GVfsBackend *backend,
         GVfsJobRead *job,
         GVfsBackendHandle handle,
         char *buffer,
         gsize bytes_requested)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (backend);
  ObexFTPOpenHandle *backend_handle = (ObexFTPOpenHandle *) handle;
  ssize_t bytes_read = 0;
  gboolean busy = TRUE;

  while (bytes_read == 0 && busy != FALSE)
    {
      bytes_read = read (backend_handle->fd, buffer, bytes_requested);
      if (bytes_read != 0)
            break;

      if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_CANCELLED,
                            _("Operation was cancelled"));
          return;
        }

      busy = is_busy (op_backend->session_proxy, G_VFS_JOB (job));
      if (busy < 0)
            return;

      g_usleep (G_USEC_PER_SEC / 100);
    }

  if (bytes_read < 0)
    {
      g_vfs_job_failed_from_errno (G_VFS_JOB (job), errno);
    }
  else if (bytes_read == 0)
    {
      g_vfs_job_read_set_size (job, 0);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_read_set_size (job, bytes_read);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
}

static void
do_close_read (GVfsBackend *backend,
               GVfsJobCloseRead *job,
               GVfsBackendHandle handle)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (backend);
  ObexFTPOpenHandle *backend_handle = (ObexFTPOpenHandle *) handle;
  int busy;

  g_debug ("+ do_close_read\n");

  busy = is_busy (op_backend->session_proxy, G_VFS_JOB (job));
  if (busy < 0) {
      g_message ("busy error");
        return;
  }

  g_mutex_lock (op_backend->mutex);

  if (busy > 0)
    {
      op_backend->status = ASYNC_PENDING;

      if (dbus_g_proxy_call (op_backend->session_proxy, "Cancel", NULL,
                         G_TYPE_INVALID, G_TYPE_INVALID) != FALSE)
        {
          while (op_backend->status == ASYNC_PENDING)
                g_cond_wait (op_backend->cond, op_backend->mutex);
        }
    }

  g_mutex_unlock (op_backend->mutex);

  close (backend_handle->fd);
  g_free (backend_handle->source);
  g_free (backend_handle);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_debug ("- do_close_read\n");
}

static void
do_query_info (GVfsBackend *backend,
               GVfsJobQueryInfo *job,
               const char *filename,
               GFileQueryInfoFlags flags,
               GFileInfo *info,
               GFileAttributeMatcher *matcher)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (backend);
  GError *error = NULL;

  g_debug ("+ do_query_info, filename: %s\n", filename);

  g_mutex_lock (op_backend->mutex);

  if (_query_file_info_helper (backend, filename, info, &error) == FALSE)
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  g_mutex_unlock (op_backend->mutex);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_debug ("- do_query_info\n");

  return;
}

static void
do_query_fs_info (GVfsBackend *backend,
                  GVfsJobQueryFsInfo *job,
                  const char *filename,
                  GFileInfo *info,
                  GFileAttributeMatcher *attribute_matcher)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (backend);
  OvuCaps *caps;
  OvuCapsMemory *memory;
  GError *error = NULL;
  char *caps_str;
  const char *mem_type;
  GList *l;
  gboolean has_free_memory;

  g_debug ("+ do_query_fs_info, filename: %s\n", filename);

  g_mutex_lock (op_backend->mutex);

  /* Get the capabilities */
  if (dbus_g_proxy_call (op_backend->session_proxy, "GetCapability", &error,
                         G_TYPE_INVALID,
                         G_TYPE_STRING, &caps_str, G_TYPE_INVALID) == FALSE)
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_CANCELLED,
                        _("Operation was cancelled"));
      g_free (caps_str);
      return;
    }

  /* No caps from the server? */
  if (caps_str == NULL)
    {
      /* Best effort, don't error out */
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_succeeded (G_VFS_JOB (job));
      return;
    }

  caps = ovu_caps_parser_parse (caps_str, strlen (caps_str), &error);
  g_free (caps_str);
  if (caps == NULL)
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  /* Check whether we have no free space available */
  has_free_memory = FALSE;
  for (l = ovu_caps_get_memory_entries (caps); l != NULL; l = l->next)
    {
      if (ovu_caps_memory_has_free (l->data) != FALSE)
        {
          has_free_memory = TRUE;
          break;
        }
    }
  if (has_free_memory == FALSE)
    {
      g_mutex_unlock (op_backend->mutex);
      /* Best effort, don't error out */
      g_vfs_job_succeeded (G_VFS_JOB (job));
      return;
    }

  /* Check whether we have only one memory type */
  l = ovu_caps_get_memory_entries (caps);
  if (g_list_length (l) == 1)
    {
      memory = l->data;
      goto set_info_from_memory;
    }

  if (_query_file_info_helper (backend, filename, info, &error) == FALSE)
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      ovu_caps_free (caps);
      return;
    }

  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_CANCELLED,
                        _("Operation was cancelled"));
      ovu_caps_free (caps);
      return;
    }

  mem_type = NULL;
  if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_RDEV) != FALSE)
    {
      guint rdev;
      rdev = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_RDEV);
      mem_type = om_mem_type_id_to_string (rdev);
    }
  memory = ovu_caps_get_memory_type (caps, mem_type);

set_info_from_memory:
  if (memory != NULL && ovu_caps_memory_has_free (memory) != FALSE)
    {
      g_file_info_set_attribute_uint64 (info,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
                                        ovu_caps_memory_get_free (memory));
      if (ovu_caps_memory_has_used (memory) != FALSE)
        {
          g_file_info_set_attribute_uint64 (info,
                                            G_FILE_ATTRIBUTE_FILESYSTEM_SIZE,
                                            ovu_caps_memory_get_free (memory)
                                            + ovu_caps_memory_get_used (memory));
        }
    }
  ovu_caps_free (caps);

  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "obexftp");

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_mutex_unlock (op_backend->mutex);

  g_debug ("- do_query_fs_info\n");
}

static void
do_enumerate (GVfsBackend *backend,
              GVfsJobEnumerate *job,
              const char *filename,
              GFileAttributeMatcher *matcher,
              GFileQueryInfoFlags flags)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (backend);
  GError *error = NULL;
  char *files;
  GList *elements = NULL;

  g_debug ("+ do_enumerate, filename: %s\n", filename);

  g_mutex_lock (op_backend->mutex);

  if (_change_directory (op_backend, filename, &error) == FALSE)
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  files = NULL;
  if (_retrieve_folder_listing (backend, filename, &files, &error) == FALSE)
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  if (gvfsbackendobexftp_fl_parser_parse (files, strlen (files), &elements, &error) == FALSE)
    {
      g_mutex_unlock (op_backend->mutex);
      /* See http://web.archive.org/web/20070826221251/http://docs.kde.org/development/en/extragear-pim/kdebluetooth/components.kio_obex.html#devices
       * for the reasoning */
      if (strstr (files, "SYSTEM\"obex-folder-listing.dtd") != NULL && _is_nokia_3650 (op_backend->bdaddr))
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Device requires a software update"));
        }
      else
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
        }
      g_message ("gvfsbackendobexftp_fl_parser_parse failed");
      g_free (files);
      g_error_free (error);
      return;
    }
  g_free (files);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_vfs_job_enumerate_add_infos (job, elements);

  g_list_foreach (elements, (GFunc)g_object_unref, NULL);
  g_list_free (elements);
  g_vfs_job_enumerate_done (job);

  g_mutex_unlock (op_backend->mutex);

  g_debug ("- do_enumerate\n");
}

typedef struct {
  GVfsBackendObexftp *op_backend;
  GFileProgressCallback progress_callback;
  gpointer progress_callback_data;
  goffset total_bytes;
} PushData;

static void
push_transfer_started_cb (DBusGProxy *proxy,
                          const gchar *filename,
                          const gchar *local_path,
                          guint64 total_bytes,
                          gpointer user_data)
{
  PushData *job_data = (PushData *) user_data;
  GVfsBackendObexftp *op_backend = job_data->op_backend;

  g_message ("transfer of %s to %s started", filename, local_path);

  g_mutex_lock (op_backend->mutex);

  op_backend->status = ASYNC_RUNNING;
  job_data->total_bytes = (goffset) total_bytes;
  if (job_data->progress_callback)
    job_data->progress_callback (0, job_data->total_bytes,
                                 job_data->progress_callback_data);

  g_cond_signal (op_backend->cond);
  g_mutex_unlock (op_backend->mutex);
}

static void
push_transfer_completed_cb (DBusGProxy *proxy,
                            gpointer user_data)
{
  PushData *job_data = (PushData *) user_data;
  GVfsBackendObexftp *op_backend = job_data->op_backend;

  g_message ("transfer completed");

  g_mutex_lock (op_backend->mutex);

  op_backend->status = ASYNC_SUCCESS;

  g_cond_signal (op_backend->cond);
  g_mutex_unlock (op_backend->mutex);
}

static void
push_transfer_progress_cb (DBusGProxy *proxy,
                           guint64 bytes_transferred,
                           gpointer user_data)
{
  PushData *job_data = (PushData *) user_data;

  g_message ("transfer progress");

  if (job_data->progress_callback)
    job_data->progress_callback ((goffset) bytes_transferred,
                                 job_data->total_bytes,
                                 job_data->progress_callback_data);
}

static void
push_data_free (PushData *job_data)
{
  g_object_unref (job_data->op_backend);
  g_slice_free (PushData, job_data);
}

static gboolean
_push_single_file_helper (GVfsBackendObexftp *op_backend,
                          GVfsJobPush *job,
                          const char *local_path,
                          const char *destination,
                          GError **error,
                          PushData *job_data)
{
  char *dirname;
  int success;

  dirname = g_path_get_dirname (destination);

  if (_change_directory (op_backend, dirname, error) == FALSE)
    {
      g_free (dirname);
      return FALSE;
    }

  g_free (dirname);

  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
    {
      g_set_error_literal (error, G_IO_ERROR,
                           G_IO_ERROR_CANCELLED,
                           _("Operation was cancelled"));
      return FALSE;
    }

  op_backend->status = ASYNC_PENDING;

  /* connect to the transfer signals */
  dbus_g_proxy_connect_signal (op_backend->session_proxy, "TransferStarted",
                               G_CALLBACK (push_transfer_started_cb), job_data,
                               NULL);
  dbus_g_proxy_connect_signal (op_backend->session_proxy, "TransferCompleted",
                               G_CALLBACK (push_transfer_completed_cb), job_data,
                               NULL);
  dbus_g_proxy_connect_signal (op_backend->session_proxy, "TransferProgress",
                               G_CALLBACK (push_transfer_progress_cb), job_data,
                               NULL);

  if (dbus_g_proxy_call (op_backend->session_proxy, "SendFile", error,
                         G_TYPE_STRING, local_path, G_TYPE_INVALID,
                         G_TYPE_INVALID) == FALSE)
    {
      dbus_g_proxy_disconnect_signal (op_backend->session_proxy, "TransferStarted", 
                                      G_CALLBACK (push_transfer_started_cb), job_data);
      dbus_g_proxy_disconnect_signal (op_backend->session_proxy, "TransferCompleted", 
                                      G_CALLBACK (push_transfer_completed_cb), job_data);
      dbus_g_proxy_disconnect_signal (op_backend->session_proxy, "TransferProgress", 
                                      G_CALLBACK (push_transfer_progress_cb), job_data);
      return FALSE;
    }

  /* wait for the TransferStarted or ErrorOccurred signal */
  while (op_backend->status == ASYNC_PENDING)
    g_cond_wait (op_backend->cond, op_backend->mutex);

  dbus_g_proxy_disconnect_signal (op_backend->session_proxy, "TransferStarted",
                                  G_CALLBACK (push_transfer_started_cb), job_data);
  success = op_backend->status;

  /* we either got the operation running or an error */
  if (success == ASYNC_ERROR)
    {
      dbus_g_proxy_disconnect_signal (op_backend->session_proxy, "TransferCompleted", 
                                      G_CALLBACK (push_transfer_completed_cb), job_data);
      dbus_g_proxy_disconnect_signal (op_backend->session_proxy, "TransferProgress", 
                                      G_CALLBACK (push_transfer_progress_cb), job_data);

      *error = g_error_copy (op_backend->error);
      g_error_free (op_backend->error);
      op_backend->error = NULL;
      return FALSE;
    }

  while (op_backend->status == ASYNC_RUNNING)
    g_cond_wait (op_backend->cond, op_backend->mutex);

  dbus_g_proxy_disconnect_signal (op_backend->session_proxy, "TransferCompleted", 
                                  G_CALLBACK (push_transfer_completed_cb), job_data);
  dbus_g_proxy_disconnect_signal (op_backend->session_proxy, "TransferProgress", 
                                  G_CALLBACK (push_transfer_progress_cb), job_data);
  success = op_backend->status;

  /* same as before, either we have success or an error */
  if (success == ASYNC_ERROR)
    {
      *error = g_error_copy (op_backend->error);
      g_error_free (op_backend->error);
      op_backend->error = NULL;
      return FALSE;
    }

  return TRUE;
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
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (backend);
  GError *error = NULL;
  gboolean is_dir, overwrite, res;
  GFileType target_type;
  PushData *job_data;
  GFileInfo *info;

  g_debug ("+ do_push, destination: %s, local_path: %s\n", destination, local_path);

  g_mutex_lock (op_backend->mutex);
  op_backend->doing_io = TRUE;

  overwrite = (flags & G_FILE_COPY_OVERWRITE);
  is_dir = g_file_test (local_path, G_FILE_TEST_IS_DIR);
  info = g_file_info_new ();
  target_type = 0;

  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
    {
      op_backend->doing_io = FALSE;
      g_mutex_unlock (op_backend->mutex);

      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_CANCELLED,
                        _("Operation was cancelled"));
      return;
    }

  res = _query_file_info_helper (backend, destination, info, &error);

  if (error != NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          op_backend->doing_io = FALSE;
          g_mutex_unlock (op_backend->mutex);

          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);

          return;
        }
      else
        {
          g_clear_error (&error);
          res = TRUE;
        }
    }

  if (res)
    target_type = g_file_info_get_file_type (info);

  g_object_unref (info);

  /* error handling */
  if (is_dir)
    {
      op_backend->doing_io = FALSE;
      g_mutex_unlock (op_backend->mutex);

      if (target_type != 0)
        {
          if (overwrite)
            {
              if (target_type == G_FILE_TYPE_DIRECTORY)
                {
                  g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                                    G_IO_ERROR_WOULD_MERGE,
                                   _("Can't copy directory over directory"));
                  return;
                }
            }
          else
            {
              g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                                G_IO_ERROR_EXISTS,
                                _("Target file exists"));
              return;
            }
        }

      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_WOULD_RECURSE,
                        _("Can't recursively copy directory"));
      return;
    }
  else
    {
      if (target_type != 0)
        {
          op_backend->doing_io = FALSE;
          g_mutex_unlock (op_backend->mutex);

          if (overwrite)
            {
              if (target_type == G_FILE_TYPE_DIRECTORY)
                {
                  g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                                    G_IO_ERROR_IS_DIRECTORY,
                                    _("Can't copy file over directory"));
                  return;
                }
            }
          else
            {
              g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                                G_IO_ERROR_EXISTS,
                                _("Target file exists"));
              return;
            }
        }
    }

  job_data = g_slice_new0 (PushData);
  job_data->op_backend = g_object_ref (op_backend);
  job_data->progress_callback = progress_callback;
  job_data->progress_callback_data = progress_callback_data;

  /* start the actual transfer operation */
  if (_push_single_file_helper (op_backend, job, local_path, destination,
                                &error, job_data) == FALSE)
    {
      op_backend->doing_io = FALSE;
      g_mutex_unlock (op_backend->mutex);
      push_data_free (job_data);

      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      return;
    }

  push_data_free (job_data);

  /* we called _query_file_info_helper (), so we need to invalidate the
   * cache, as a query_info () will be called on us after we return.
   */
  _invalidate_cache_helper (op_backend);

  if (remove_source && g_unlink (local_path) == -1)
    {
      int errsv = errno;

      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        g_io_error_from_errno (errsv),
                        _("Error deleting file: %s"),
                        g_strerror (errsv));
    }
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));

  op_backend->doing_io = FALSE;
  g_mutex_unlock (op_backend->mutex);

  g_debug ("- do_push\n");
}

static void
do_delete (GVfsBackend *backend,
           GVfsJobDelete *job,
           const char *filename)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (backend);
  char *basename, *parent;
  GError *error = NULL;
  GFileInfo *info;

  g_debug ("+ do_delete, filename: %s\n", filename);

  g_mutex_lock (op_backend->mutex);

  /* Check whether we have a directory */
  info = g_file_info_new ();
  if (_query_file_info_helper (backend, filename, info, &error) == FALSE)
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      g_object_unref (info);
      return;
    }

  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_CANCELLED,
                        _("Operation was cancelled"));
      g_object_unref (info);
      return;
    }

  /* Get the listing of the directory, and abort if it's not empty */
  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
    {
      GList *elements;
      char *files;
      guint len;

      g_object_unref (info);

      if (_change_directory (op_backend, filename, &error) == FALSE)
        {
          g_mutex_unlock (op_backend->mutex);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          return;
        }

      if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
        {
          g_mutex_unlock (op_backend->mutex);
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_CANCELLED,
                            _("Operation was cancelled"));
          return;
        }

      files = NULL;
      if (_retrieve_folder_listing (backend, filename, &files, &error) == FALSE)
        {
          g_mutex_unlock (op_backend->mutex);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          return;
        }

      if (gvfsbackendobexftp_fl_parser_parse (files, strlen (files), &elements, &error) == FALSE)
        {
          g_mutex_unlock (op_backend->mutex);
          g_message ("gvfsbackendobexftp_fl_parser_parse failed");
          g_free (files);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          return;
        }
      g_free (files);

      len = g_list_length (elements);
      g_list_foreach (elements, (GFunc)g_object_unref, NULL);
      g_list_free (elements);

      if (len != 0)
        {
          g_mutex_unlock (op_backend->mutex);
          g_set_error_literal (&error, G_IO_ERROR,
	                       G_IO_ERROR_NOT_EMPTY,
        	               g_strerror (ENOTEMPTY));
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          return;
        }
    }
  else
    {
      g_object_unref (info);
    }

  basename = g_path_get_basename (filename);
  if (strcmp (basename, G_DIR_SEPARATOR_S) == 0
      || strcmp (basename, ".") == 0)
    {
      g_mutex_unlock (op_backend->mutex);
      g_free (basename);
      g_vfs_job_failed_from_errno (G_VFS_JOB (job), EPERM);
      return;
    }

  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_CANCELLED,
                        _("Operation was cancelled"));
      g_free (basename);
      return;
    }

  parent = g_path_get_dirname (filename);
  if (_change_directory (op_backend, parent, &error) == FALSE)
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_free (basename);
      g_free (parent);
      g_error_free (error);
      return;
    }
  g_free (parent);

  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_CANCELLED,
                        _("Operation was cancelled"));
      g_free (basename);
      return;
    }

  if (dbus_g_proxy_call (op_backend->session_proxy, "DeleteRemoteFile", &error,
                         G_TYPE_STRING, basename, G_TYPE_INVALID,
                         G_TYPE_INVALID) == FALSE)
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }
  g_free (basename);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_mutex_unlock (op_backend->mutex);

  g_debug ("- do_delete\n");
}

static void
do_make_directory (GVfsBackend *backend,
                   GVfsJobMakeDirectory *job,
                   const char *filename)
{
  GVfsBackendObexftp *op_backend = G_VFS_BACKEND_OBEXFTP (backend);
  char *basename, *parent;
  GError *error = NULL;
  GFileInfo *info;

  g_debug ("+ do_make_directory, filename: %s\n", filename);

  g_mutex_lock (op_backend->mutex);

  /* Check if the folder already exists */
  info = g_file_info_new ();
  if (_query_file_info_helper (backend, filename, info, &error) != FALSE)
    {
      g_mutex_unlock (op_backend->mutex);
      g_object_unref (info);
      g_vfs_job_failed_from_errno (G_VFS_JOB (job), EEXIST);
      return;
    }
  g_object_unref (info);
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) == FALSE)
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }

  if (error)
    g_clear_error (&error);

  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_CANCELLED,
                        _("Operation was cancelled"));
      return;
    }

  parent = g_path_get_dirname (filename);
  if (_change_directory (op_backend, parent, &error) == FALSE)
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }
  g_free (parent);

  if (g_vfs_job_is_cancelled (G_VFS_JOB (job)))
    {
      g_mutex_unlock (op_backend->mutex);
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_CANCELLED,
                        _("Operation was cancelled"));
      return;
    }

  basename = g_path_get_basename (filename);
  if (dbus_g_proxy_call (op_backend->session_proxy, "CreateFolder", &error,
                         G_TYPE_STRING, basename, G_TYPE_INVALID,
                         G_TYPE_INVALID) == FALSE)
    {
      g_mutex_unlock (op_backend->mutex);
      g_free (basename);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      return;
    }
  g_free (basename);

  /* reset the current directory so that we won't cache when querying
   * info after this has succeeded.
   */
  _invalidate_cache_helper (op_backend);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_mutex_unlock (op_backend->mutex);

  g_debug ("- do_make_directory\n");
}

static void
g_vfs_backend_obexftp_class_init (GVfsBackendObexftpClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

  gobject_class->finalize = g_vfs_backend_obexftp_finalize;

  backend_class->mount = do_mount;
  backend_class->open_for_read = do_open_for_read;
  backend_class->read = do_read;
  backend_class->close_read = do_close_read;
  backend_class->query_info = do_query_info;
  backend_class->query_fs_info = do_query_fs_info;
  backend_class->enumerate = do_enumerate;
  backend_class->delete = do_delete;
  backend_class->make_directory = do_make_directory;
  backend_class->push = do_push;

  /* ErrorOccurred */
  dbus_g_object_register_marshaller (obexftp_marshal_VOID__STRING_STRING,
                                     G_TYPE_NONE, G_TYPE_STRING,
                                     G_TYPE_STRING, G_TYPE_INVALID);
  /* TransferStarted */
  dbus_g_object_register_marshaller(obexftp_marshal_VOID__STRING_STRING_UINT64,
                                    G_TYPE_NONE, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64, G_TYPE_INVALID);
  /* TransferProgress */
  dbus_g_object_register_marshaller(obexftp_marshal_VOID__UINT64,
                                    G_TYPE_NONE, G_TYPE_UINT64, G_TYPE_INVALID);
  /* SessionConnected */
  dbus_g_object_register_marshaller(obexftp_marshal_VOID__STRING,
                                    G_TYPE_NONE, DBUS_TYPE_G_OBJECT_PATH, G_TYPE_INVALID);
  /* SessionConnectError */
  dbus_g_object_register_marshaller (obexftp_marshal_VOID__STRING_STRING_STRING,
                                     G_TYPE_NONE, DBUS_TYPE_G_OBJECT_PATH, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INVALID);
}

/*
 * vim: sw=2 ts=8 cindent expandtab cinoptions=f0,>4,n2,{2,(0,^-2,t0 ai
 */
