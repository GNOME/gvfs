/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
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
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>
#include "gdaemonvolumemonitor.h"
#include "gdaemonmount.h"
#include "gvfsdaemondbus.h"
#include "gdaemonfile.h"
#include "gmountsource.h"
#include "gmountoperationdbus.h"
#include <gvfsdbus.h>
#include <gvfsdaemonprotocol.h>

/* Protects all fields of GDaemonMount that can change
   which at this point is just foreign_volume */
G_LOCK_DEFINE_STATIC(daemon_mount);

struct _GDaemonMount {
  GObject     parent;

  GMountInfo *mount_info;

  GVolumeMonitor *volume_monitor;
};

static void g_daemon_mount_mount_iface_init (GMountIface *iface);

G_DEFINE_TYPE_WITH_CODE (GDaemonMount, g_daemon_mount, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT,
						g_daemon_mount_mount_iface_init))

static void
g_daemon_mount_finalize (GObject *object)
{
  GDaemonMount *mount;
  
  mount = G_DAEMON_MOUNT (object);

  if (mount->volume_monitor != NULL)
    g_object_remove_weak_pointer (G_OBJECT (mount->volume_monitor), (gpointer) &(mount->volume_monitor));

  g_mount_info_unref (mount->mount_info);
  
  if (G_OBJECT_CLASS (g_daemon_mount_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_mount_parent_class)->finalize) (object);
}

static void
g_daemon_mount_class_init (GDaemonMountClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_daemon_mount_finalize;
}

static void
g_daemon_mount_init (GDaemonMount *daemon_mount)
{
}

GDaemonMount *
g_daemon_mount_new (GMountInfo     *mount_info,
                    GVolumeMonitor *volume_monitor)
{
  GDaemonMount *mount;

  mount = g_object_new (G_TYPE_DAEMON_MOUNT, NULL);
  mount->mount_info = g_mount_info_ref (mount_info);
  mount->volume_monitor = volume_monitor;
  g_object_set_data (G_OBJECT (mount), "g-stable-name", (gpointer) mount_info->stable_name);
  if (mount->volume_monitor != NULL)
    g_object_add_weak_pointer (G_OBJECT (volume_monitor), (gpointer) &(mount->volume_monitor));

  return mount;
}

GMountInfo *
g_daemon_mount_get_mount_info (GDaemonMount *mount)
{
  return mount->mount_info;
}

static GFile *
g_daemon_mount_get_root (GMount *mount)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);

  return g_daemon_file_new (daemon_mount->mount_info->mount_spec, 
        daemon_mount->mount_info->mount_spec->mount_prefix);
}

static GIcon *
g_daemon_mount_get_icon (GMount *mount)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);

  return g_object_ref (daemon_mount->mount_info->icon);
}

static GIcon *
g_daemon_mount_get_symbolic_icon (GMount *mount)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);

  return g_object_ref (daemon_mount->mount_info->symbolic_icon);
}

static char *
g_daemon_mount_get_name (GMount *mount)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);

  return g_strdup (daemon_mount->mount_info->display_name);
}

static GFile *
g_daemon_mount_get_default_location (GMount *mount)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);
  const char *location = daemon_mount->mount_info->default_location;

  if (location == NULL || location[0] == '\0')
    location = daemon_mount->mount_info->mount_spec->mount_prefix;

  return g_daemon_file_new (daemon_mount->mount_info->mount_spec,
                            location);
}

static char *
g_daemon_mount_get_uuid (GMount *mount)
{
  return NULL;
}

static GVolume *
g_daemon_mount_get_volume (GMount *mount)
{
  return NULL;
}

static GDrive *
g_daemon_mount_get_drive (GMount *mount)
{
  return NULL;
}

static gboolean
g_daemon_mount_can_unmount (GMount *mount)
{
  return TRUE;
}

static gboolean
g_daemon_mount_can_eject (GMount *mount)
{
  return FALSE;
}


typedef struct {
  GMountInfo *mount_info;
  GMountOperation *mount_operation;
  GMountUnmountFlags flags;
  GDBusConnection *connection;
  GVfsDBusMount *proxy;
  gulong cancelled_tag;
} AsyncProxyCreate;

static void
async_proxy_create_free (AsyncProxyCreate *data)
{
  g_clear_object (&data->mount_operation);
  g_clear_object (&data->connection);
  g_clear_object (&data->proxy);
  g_free (data);
}

static void
unmount_reply (GVfsDBusMount *proxy,
               GAsyncResult *res,
               gpointer user_data)
{
  GDBusProxy *base_proxy = G_DBUS_PROXY (proxy);
  GTask *task = G_TASK (user_data);
  AsyncProxyCreate *data = g_task_get_task_data (task);
  GError *error = NULL;

  _g_daemon_vfs_invalidate (g_dbus_proxy_get_name (base_proxy),
                            g_dbus_proxy_get_object_path (base_proxy));

  if (! gvfs_dbus_mount_call_unmount_finish (proxy, res, &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_task_return_error (task, error);
    }
  else
    {
      g_task_return_boolean (task, TRUE);
    }

  _g_dbus_async_unsubscribe_cancellable (g_task_get_cancellable (task), data->cancelled_tag);
  g_object_unref (task);
}

static void
async_proxy_new_cb (GObject *source_object,
                    GAsyncResult *res,
                    gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  AsyncProxyCreate *data = g_task_get_task_data (task);
  GVfsDBusMount *proxy;
  GError *error = NULL;
  GMountSource *mount_source;

  proxy = gvfs_dbus_mount_proxy_new_finish (res, &error);
  if (proxy == NULL)
    {
      g_dbus_error_strip_remote_error (error);
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  data->proxy = proxy;

  mount_source = g_mount_operation_dbus_wrap (data->mount_operation, _g_daemon_vfs_get_async_bus ());

  /* 30 minute timeout */
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), G_VFS_DBUS_MOUNT_TIMEOUT_MSECS);

  gvfs_dbus_mount_call_unmount (proxy,
                                g_mount_source_get_dbus_id (mount_source),
                                g_mount_source_get_obj_path (mount_source),
                                data->flags,
                                g_task_get_cancellable (task),
                                (GAsyncReadyCallback) unmount_reply,
                                task);
  data->cancelled_tag = _g_dbus_async_subscribe_cancellable (data->connection,
                                                             g_task_get_cancellable (task));
  
  g_object_unref (mount_source);
}

static void
async_construct_proxy (GDBusConnection *connection,
                       GTask *task)
{
  AsyncProxyCreate *data = g_task_get_task_data (task);

  data->connection = g_object_ref (connection);
  gvfs_dbus_mount_proxy_new (connection,
                             G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                             data->mount_info->dbus_id,
                             data->mount_info->object_path,
                             g_task_get_cancellable (task),
                             async_proxy_new_cb,
                             task);
}

static void
bus_get_cb (GObject *source_object,
            GAsyncResult *res,
            gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  GDBusConnection *connection;
  GError *error = NULL;
  
  connection = g_bus_get_finish (res, &error);
  
  if (connection == NULL)
    {
      g_dbus_error_strip_remote_error (error);
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  async_construct_proxy (connection, task);
  g_object_unref (connection);
}

static void
async_got_connection_cb (GDBusConnection *connection,
                         GError *io_error,
                         gpointer callback_data)
{
  GTask *task = G_TASK (callback_data);

  if (connection == NULL)
    {
      g_dbus_error_strip_remote_error (io_error);

      if (g_error_matches (io_error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
          g_error_matches (io_error, G_VFS_ERROR, G_VFS_ERROR_RETRY))
        {
          g_task_return_error (task, g_error_copy (io_error));
          g_object_unref (task);
          return;
        }

      g_warning ("The peer-to-peer connection failed: %s. Falling back to the "
                 "session bus. Your application is probably missing "
                 "--filesystem=xdg-run/gvfsd privileges.", io_error->message);

      g_bus_get (G_BUS_TYPE_SESSION,
                 g_task_get_cancellable (task),
                 bus_get_cb,
                 task);
      return;
    }
  
  async_construct_proxy (connection, task);
}

static void
g_daemon_mount_unmount_with_operation (GMount *mount,
                                       GMountUnmountFlags flags,
                                       GMountOperation *mount_operation,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer         user_data)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);
  GTask *task;
  AsyncProxyCreate *data;

  task = g_task_new (mount, cancellable, callback, user_data);
  g_task_set_source_tag (task, g_daemon_mount_unmount_with_operation);

  data = g_new0 (AsyncProxyCreate, 1);
  data->mount_info = daemon_mount->mount_info;
  data->flags = flags;
  if (mount_operation)
    data->mount_operation = g_object_ref (mount_operation);

  g_task_set_task_data (task, data, (GDestroyNotify) async_proxy_create_free);

  _g_dbus_connection_get_for_async (data->mount_info->dbus_id,
                                    async_got_connection_cb,
                                    task,
                                    cancellable);
}

static gboolean
g_daemon_mount_unmount_with_operation_finish (GMount *mount,
                                              GAsyncResult *result,
                                              GError **error)
{
  g_return_val_if_fail (g_task_is_valid (result, mount), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_daemon_mount_unmount_with_operation), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
g_daemon_mount_unmount (GMount *mount,
			GMountUnmountFlags flags,
			GCancellable *cancellable,
			GAsyncReadyCallback callback,
			gpointer         user_data)
{
  g_daemon_mount_unmount_with_operation (mount, flags, NULL, cancellable, callback, user_data);
}

static gboolean
g_daemon_mount_unmount_finish (GMount *mount,
				GAsyncResult *result,
				GError **error)
{
  return g_daemon_mount_unmount_with_operation_finish (mount, result, error);
}

static char **
g_daemon_mount_guess_content_type_sync (GMount              *mount,
                                        gboolean             force_rescan,
                                        GCancellable        *cancellable,
                                        GError             **error)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);
  char **result;

  G_LOCK (daemon_mount);
  if (daemon_mount->mount_info->x_content_types != NULL &&
      strlen (daemon_mount->mount_info->x_content_types) > 0)
    result = g_strsplit (daemon_mount->mount_info->x_content_types, " ", 0);
  else
    result = g_new0 (char *, 1);
  G_UNLOCK (daemon_mount);

  return result;
}

static void
g_daemon_mount_guess_content_type (GMount              *mount,
                                   gboolean             force_rescan,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
  GTask *task;
  char **type;
  GError *error = NULL;

  task = g_task_new (mount, cancellable, callback, user_data);
  g_task_set_source_tag (task, g_daemon_mount_guess_content_type);

  type = g_daemon_mount_guess_content_type_sync (mount, FALSE, cancellable, &error);
  if (error != NULL)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, type, (GDestroyNotify) g_strfreev);

  g_object_unref (task);
}

static char **
g_daemon_mount_guess_content_type_finish (GMount              *mount,
                                          GAsyncResult        *result,
                                          GError             **error)
{
  g_return_val_if_fail (g_task_is_valid (result, mount), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_daemon_mount_guess_content_type), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
g_daemon_mount_mount_iface_init (GMountIface *iface)
{
  iface->get_root = g_daemon_mount_get_root;
  iface->get_name = g_daemon_mount_get_name;
  iface->get_icon = g_daemon_mount_get_icon;
  iface->get_symbolic_icon = g_daemon_mount_get_symbolic_icon;
  iface->get_uuid = g_daemon_mount_get_uuid;
  iface->get_volume = g_daemon_mount_get_volume;
  iface->get_drive = g_daemon_mount_get_drive;
  iface->get_default_location = g_daemon_mount_get_default_location;
  iface->can_unmount = g_daemon_mount_can_unmount;
  iface->can_eject = g_daemon_mount_can_eject;
  iface->unmount = g_daemon_mount_unmount;
  iface->unmount_finish = g_daemon_mount_unmount_finish;
  iface->unmount_with_operation = g_daemon_mount_unmount_with_operation;
  iface->unmount_with_operation_finish = g_daemon_mount_unmount_with_operation_finish;
  iface->guess_content_type = g_daemon_mount_guess_content_type;
  iface->guess_content_type_finish = g_daemon_mount_guess_content_type_finish;
  iface->guess_content_type_sync = g_daemon_mount_guess_content_type_sync;
}
