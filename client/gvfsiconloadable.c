/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2008 Red Hat, Inc.
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
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>
#include <string.h>
#include <glib/gi18n-lib.h>
#include <gio/gunixfdlist.h>

#include "gvfsicon.h"
#include "gvfsiconloadable.h"
#include "gmounttracker.h"
#include "gvfsdaemondbus.h"
#include "gdaemonvfs.h"
#include "gdaemonfileinputstream.h"
#include <gvfsdbus.h>

/* see comment in common/giconvfs.c for why the GLoadableIcon interface is here */


static GVfsDBusMount *
create_proxy_for_icon (GVfsIcon *vfs_icon,
                       GCancellable *cancellable,
                       GError **error)
{
  GVfsDBusMount *proxy;
  GMountInfo *mount_info;
  GDBusConnection *connection;

  proxy = NULL;
  
  mount_info = _g_daemon_vfs_get_mount_info_sync (vfs_icon->mount_spec,
                                                  "/",
                                                  cancellable,
                                                  error);
  
  if (mount_info == NULL)
    goto out;

  connection = _g_dbus_connection_get_sync (mount_info->dbus_id, cancellable, error);
  if (connection == NULL)
    goto out;

  proxy = gvfs_dbus_mount_proxy_new_sync (connection,
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                          mount_info->dbus_id,
                                          mount_info->object_path,
                                          cancellable,
                                          error);
  
  if (proxy == NULL)
    goto out;
  
 out:
  if (mount_info)
    g_mount_info_unref (mount_info);
  if (error && *error)
    g_dbus_error_strip_remote_error (*error);

  return proxy;
}

static GInputStream *
g_vfs_icon_load (GLoadableIcon  *icon,
                 int            size,
                 char          **type,
                 GCancellable   *cancellable,
                 GError        **error)
{
  GVfsIcon *vfs_icon = G_VFS_ICON (icon);
  GVfsDBusMount *proxy;
  gboolean res;
  gboolean can_seek;
  GUnixFDList *fd_list;
  int fd;
  GVariant *fd_id_val = NULL;
  GError *local_error = NULL;

  proxy = create_proxy_for_icon (vfs_icon, cancellable, error);
  if (proxy == NULL)
    return NULL;

  res = gvfs_dbus_mount_call_open_icon_for_read_sync (proxy,
                                                      vfs_icon->icon_id,
                                                      NULL,
                                                      &fd_id_val,
                                                      &can_seek,
                                                      &fd_list,
                                                      cancellable,
                                                      &local_error);
  
  if (! res)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        _g_dbus_send_cancelled_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (proxy)));
      _g_propagate_error_stripped (error, local_error);
    }

  g_object_unref (proxy);

  if (! res)
    return NULL;
  
  if (fd_list == NULL || fd_id_val == NULL ||
      g_unix_fd_list_get_length (fd_list) != 1 ||
      (fd = g_unix_fd_list_get (fd_list, g_variant_get_handle (fd_id_val), NULL)) == -1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Didn't get stream file descriptor"));
      return NULL;
    }

  g_variant_unref (fd_id_val);
  g_object_unref (fd_list);
  
  return G_INPUT_STREAM (g_daemon_file_input_stream_new (fd, can_seek));
}


typedef void (*CreateProxyAsyncCallback) (GVfsDBusMount *proxy,
                                          GSimpleAsyncResult *result,
                                          GCancellable *cancellable,
                                          gpointer callback_data);

typedef struct {
  GSimpleAsyncResult *result;
  GVfsIcon *vfs_icon;
  GMountInfo *mount_info;
  GDBusConnection *connection;
  GVfsDBusMount *proxy;
  GCancellable *cancellable;
  CreateProxyAsyncCallback callback;
  gpointer callback_data;
  gulong cancelled_tag;
} AsyncPathCall;


static void
async_path_call_free (AsyncPathCall *data)
{
  g_clear_object (&data->connection);
  if (data->mount_info)
    g_mount_info_unref (data->mount_info);
  g_clear_object (&data->result);
  g_object_unref (data->vfs_icon);
  g_clear_object (&data->cancellable);
  g_clear_object (&data->proxy);
  g_free (data);
}

static void
async_proxy_new_cb (GObject *source_object,
                    GAsyncResult *res,
                    gpointer user_data)
{
  AsyncPathCall *data = user_data;
  GVfsDBusMount *proxy;
  GError *error = NULL;
  
  proxy = gvfs_dbus_mount_proxy_new_finish (res, &error);
  if (proxy == NULL)
    {
      _g_simple_async_result_take_error_stripped (data->result, error);      
      _g_simple_async_result_complete_with_cancellable (data->result, data->cancellable);
      async_path_call_free (data);
      return;
    }
  
  data->proxy = proxy;

  data->callback (proxy,
                  data->result,
                  data->cancellable,
                  data->callback_data);
}

static void
async_got_connection_cb (GDBusConnection *connection,
                         GError *io_error,
                         gpointer callback_data)
{
  AsyncPathCall *data = callback_data;
  
  if (connection == NULL)
    {
      g_dbus_error_strip_remote_error (io_error);
      g_simple_async_result_set_from_error (data->result, io_error);
      _g_simple_async_result_complete_with_cancellable (data->result, data->cancellable);
      async_path_call_free (data);
      return;
    }
  
  data->connection = g_object_ref (connection);
  gvfs_dbus_mount_proxy_new (connection,
                             G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                             data->mount_info->dbus_id,
                             data->mount_info->object_path,
                             data->cancellable,
                             async_proxy_new_cb,
                             data);
}

static void
async_got_mount_info (GMountInfo *mount_info,
                      gpointer _data,
                      GError *error)
{
  AsyncPathCall *data = _data;

  if (error != NULL)
    {
      g_dbus_error_strip_remote_error (error);
      g_simple_async_result_set_from_error (data->result, error);      
      _g_simple_async_result_complete_with_cancellable (data->result, data->cancellable);
      async_path_call_free (data);
      return;
    }

  data->mount_info = g_mount_info_ref (mount_info);

  _g_dbus_connection_get_for_async (mount_info->dbus_id,
                                    async_got_connection_cb,
                                    data,
                                    data->cancellable);
}

static void
create_proxy_for_icon_async (GVfsIcon *vfs_icon,
		             GCancellable *cancellable,
		             GAsyncReadyCallback op_callback,
		             gpointer op_callback_data,
		             CreateProxyAsyncCallback callback,
		             gpointer callback_data)
{
  AsyncPathCall *data;

  data = g_new0 (AsyncPathCall, 1);

  data->result = g_simple_async_result_new (G_OBJECT (vfs_icon),
					    op_callback, op_callback_data,
					    NULL);

  data->vfs_icon = g_object_ref (vfs_icon);
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);
  data->callback = callback;
  data->callback_data = callback_data;

  _g_daemon_vfs_get_mount_info_async (vfs_icon->mount_spec,
                                      "/",
                                      async_got_mount_info,
                                      data);
}

static void
open_icon_read_cb (GVfsDBusMount *proxy,
                   GAsyncResult *res,
                   gpointer user_data)
{
  AsyncPathCall *data = user_data;
  GError *error = NULL;
  gboolean can_seek;
  GUnixFDList *fd_list;
  int fd;
  GVariant *fd_id_val;
  guint fd_id;
  GFileInputStream *stream;

  if (! gvfs_dbus_mount_call_open_icon_for_read_finish (proxy, &fd_id_val, &can_seek, &fd_list, res, &error))
    {
      _g_simple_async_result_take_error_stripped (data->result, error);
      goto out;
    }

  fd_id = g_variant_get_handle (fd_id_val);
  g_variant_unref (fd_id_val);

  if (fd_list == NULL || g_unix_fd_list_get_length (fd_list) != 1 ||
      (fd = g_unix_fd_list_get (fd_list, fd_id, NULL)) == -1)
    {
      g_simple_async_result_set_error (data->result,
                                       G_IO_ERROR, G_IO_ERROR_FAILED,
                                       _("Couldn't get stream file descriptor"));
    }
  else
    {
      stream = g_daemon_file_input_stream_new (fd, can_seek);
      g_simple_async_result_set_op_res_gpointer (data->result, stream, g_object_unref);
      g_object_unref (fd_list);
    }

out:
  _g_simple_async_result_complete_with_cancellable (data->result, data->cancellable);
  _g_dbus_async_unsubscribe_cancellable (data->cancellable, data->cancelled_tag);
  async_path_call_free (data);
}

static void
load_async_cb (GVfsDBusMount *proxy,
               GSimpleAsyncResult *result,
               GCancellable *cancellable,
               gpointer callback_data)
{
  AsyncPathCall *data = callback_data;

  gvfs_dbus_mount_call_open_icon_for_read (proxy,
                                           data->vfs_icon->icon_id,
                                           NULL,
                                           cancellable,
                                           (GAsyncReadyCallback) open_icon_read_cb,
                                           callback_data);
  data->cancelled_tag = _g_dbus_async_subscribe_cancellable (data->connection, cancellable);
}

static void
g_vfs_icon_load_async (GLoadableIcon       *icon,
                       int                  size,
                       GCancellable        *cancellable,
                       GAsyncReadyCallback  callback,
                       gpointer             user_data)
{
  create_proxy_for_icon_async (G_VFS_ICON (icon),
		               cancellable,
		               callback, user_data,
		               load_async_cb, NULL);
}

static GInputStream *
g_vfs_icon_load_finish (GLoadableIcon  *icon,
                         GAsyncResult   *res,
                         char          **type,
                         GError        **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  gpointer op;

  op = g_simple_async_result_get_op_res_gpointer (simple);
  if (op)
    return g_object_ref (op);

  return NULL;
}

static void
g_vfs_icon_loadable_icon_iface_init (GLoadableIconIface *iface)
{
  iface->load = g_vfs_icon_load;
  iface->load_async = g_vfs_icon_load_async;
  iface->load_finish = g_vfs_icon_load_finish;
}

void
_g_vfs_icon_add_loadable_interface (void)
{
  static const GInterfaceInfo g_implement_interface_info = {
    (GInterfaceInitFunc) g_vfs_icon_loadable_icon_iface_init
  };

  g_type_add_interface_static (G_VFS_TYPE_ICON, G_TYPE_LOADABLE_ICON, &g_implement_interface_info);
}
