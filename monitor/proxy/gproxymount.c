/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
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
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include <gvfsdbusutils.h>

#include "gproxyvolumemonitor.h"
#include "gproxymount.h"
#include "gproxyvolume.h"
#include "gproxymountoperation.h"

/* Protects all fields of GProxyMount that can change */
G_LOCK_DEFINE_STATIC(proxy_mount);

struct _GProxyMount {
  GObject parent;

  GProxyVolumeMonitor *volume_monitor;

  char *id;
  char *name;
  char *uuid;
  char *volume_id;
  gboolean can_unmount;
  char **x_content_types;
  GFile *root;
  GIcon *icon;
};

static void g_proxy_mount_mount_iface_init (GMountIface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (GProxyMount, g_proxy_mount, G_TYPE_OBJECT, 0,
                                G_IMPLEMENT_INTERFACE_DYNAMIC (G_TYPE_MOUNT,
                                                               g_proxy_mount_mount_iface_init))

static void
g_proxy_mount_finalize (GObject *object)
{
  GProxyMount *mount;

  mount = G_PROXY_MOUNT (object);

  g_free (mount->id);
  g_free (mount->name);
  g_free (mount->uuid);
  g_free (mount->volume_id);
  g_strfreev (mount->x_content_types);
  if (mount->icon != NULL)
    g_object_unref (mount->icon);
  if (mount->root != NULL)
    g_object_unref (mount->root);

  if (mount->volume_monitor != NULL)
    g_object_unref (mount->volume_monitor);

  if (G_OBJECT_CLASS (g_proxy_mount_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_proxy_mount_parent_class)->finalize) (object);
}

static void
g_proxy_mount_class_init (GProxyMountClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_proxy_mount_finalize;
}

static void
g_proxy_mount_class_finalize (GProxyMountClass *klass)
{
}

static void
g_proxy_mount_init (GProxyMount *proxy_mount)
{
}

GProxyMount *
g_proxy_mount_new (GProxyVolumeMonitor *volume_monitor)
{
  GProxyMount *mount;
  mount = g_object_new (G_TYPE_PROXY_MOUNT, NULL);
  mount->volume_monitor = g_object_ref (volume_monitor);
  g_object_set_data (G_OBJECT (mount),
                     "g-proxy-mount-volume-monitor-name",
                     (gpointer) g_type_name (G_TYPE_FROM_INSTANCE (volume_monitor)));
  return mount;
}

gboolean
g_proxy_mount_has_mount_path (GProxyMount *mount, const char *mount_path)
{
  char *path;
  gboolean result;
  result = FALSE;
  path = g_file_get_path (mount->root);
  if (path != NULL)
    {
      if (strcmp (path, mount_path) == 0)
        result = TRUE;
      g_free (path);
    }
  return result;
}

/* string               id
 * string               name
 * string               gicon_data
 * string               uuid
 * string               root_uri
 * boolean              can-unmount
 * string               volume-id
 * array:string         x-content-types
 */

void
g_proxy_mount_update (GProxyMount         *mount,
                      DBusMessageIter     *iter)
{
  DBusMessageIter iter_struct;
  DBusMessageIter iter_x_content_types;
  const char *id;
  const char *name;
  const char *gicon_data;
  const char *uuid;
  const char *root_uri;
  dbus_bool_t can_unmount;
  const char *volume_id;
  GPtrArray *x_content_types;

  dbus_message_iter_recurse (iter, &iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &id);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &name);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &gicon_data);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &uuid);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &root_uri);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &can_unmount);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &volume_id);
  dbus_message_iter_next (&iter_struct);

  x_content_types = g_ptr_array_new ();
  dbus_message_iter_recurse (&iter_struct, &iter_x_content_types);
  while (dbus_message_iter_get_arg_type (&iter_x_content_types) != DBUS_TYPE_INVALID)
    {
      const char *x_content_type;
      dbus_message_iter_get_basic (&iter_x_content_types, &x_content_type);
      dbus_message_iter_next (&iter_x_content_types);
      g_ptr_array_add (x_content_types, (gpointer) x_content_type);
    }
  g_ptr_array_add (x_content_types, NULL);
  dbus_message_iter_next (&iter_struct);

  if (mount->id != NULL && strcmp (mount->id, id) != 0)
    {
      g_warning ("id mismatch during update of mount");
      goto out;
    }

  if (strlen (name) == 0)
    name = NULL;
  if (strlen (uuid) == 0)
    uuid = NULL;

  /* out with the old */
  g_free (mount->id);
  g_free (mount->name);
  g_free (mount->uuid);
  g_free (mount->volume_id);
  if (mount->icon != NULL)
    g_object_unref (mount->icon);
  g_strfreev (mount->x_content_types);
  if (mount->root != NULL)
    g_object_unref (mount->root);

  /* in with the new */
  mount->id = g_strdup (id);
  mount->name = g_strdup (name);
  if (*gicon_data == 0)
    mount->icon = NULL;
  else
    mount->icon = g_icon_new_for_string (gicon_data, NULL);
  mount->uuid = g_strdup (uuid);
  mount->root = g_file_new_for_uri (root_uri);
  mount->can_unmount = can_unmount;
  mount->volume_id = g_strdup (volume_id);
  mount->x_content_types = g_strdupv ((char **) x_content_types->pdata);

 out:
  g_ptr_array_free (x_content_types, TRUE);
}

const char *
g_proxy_mount_get_id (GProxyMount *mount)
{
  return mount->id;
}

static GFile *
g_proxy_mount_get_root (GMount *mount)
{
  GProxyMount *proxy_mount = G_PROXY_MOUNT (mount);
  GFile *root;

  G_LOCK (proxy_mount);
  root = proxy_mount->root != NULL ? g_object_ref (proxy_mount->root) : NULL;
  G_UNLOCK (proxy_mount);
  return root;
}

static GIcon *
g_proxy_mount_get_icon (GMount *mount)
{
  GProxyMount *proxy_mount = G_PROXY_MOUNT (mount);
  GIcon *icon;

  G_LOCK (proxy_mount);
  icon = proxy_mount->icon != NULL ? g_object_ref (proxy_mount->icon) : NULL;
  G_UNLOCK (proxy_mount);
  return icon;
}

static char *
g_proxy_mount_get_uuid (GMount *mount)
{
  GProxyMount *proxy_mount = G_PROXY_MOUNT (mount);
  char *uuid;

  G_LOCK (proxy_mount);
  uuid = g_strdup (proxy_mount->uuid);
  G_UNLOCK (proxy_mount);
  return uuid;
}

static char *
g_proxy_mount_get_name (GMount *mount)
{
  GProxyMount *proxy_mount = G_PROXY_MOUNT (mount);
  char *name;

  G_LOCK (proxy_mount);
  name = g_strdup (proxy_mount->name);
  G_UNLOCK (proxy_mount);

  return name;
}

static GDrive *
g_proxy_mount_get_drive (GMount *mount)
{
  GProxyMount *proxy_mount = G_PROXY_MOUNT (mount);
  GProxyVolume *volume;
  GDrive *drive;

  G_LOCK (proxy_mount);
  volume = NULL;
  if (proxy_mount->volume_id != NULL && strlen (proxy_mount->volume_id) > 0)
    volume = g_proxy_volume_monitor_get_volume_for_id (proxy_mount->volume_monitor,
                                                       proxy_mount->volume_id);
  G_UNLOCK (proxy_mount);

  drive = NULL;
  if (volume != NULL)
    {
      drive = g_volume_get_drive (G_VOLUME (volume));
      g_object_unref (volume);
    }

  return drive;
}

static GVolume *
g_proxy_mount_get_volume (GMount *mount)
{
  GProxyMount *proxy_mount = G_PROXY_MOUNT (mount);
  GProxyVolume *volume;

  G_LOCK (proxy_mount);
  volume = NULL;
  if (proxy_mount->volume_id != NULL && strlen (proxy_mount->volume_id) > 0)
    volume = g_proxy_volume_monitor_get_volume_for_id (proxy_mount->volume_monitor,
                                                       proxy_mount->volume_id);
  G_UNLOCK (proxy_mount);

  return volume != NULL ? G_VOLUME (volume) : NULL;
}

static gboolean
g_proxy_mount_can_unmount (GMount *mount)
{
  GProxyMount *proxy_mount = G_PROXY_MOUNT (mount);
  gboolean res;

  G_LOCK (proxy_mount);
  res = proxy_mount->can_unmount;
  G_UNLOCK (proxy_mount);

  return res;
}

static gboolean
g_proxy_mount_can_eject (GMount *mount)
{
  GDrive *drive;
  gboolean can_eject;

  can_eject = FALSE;
  drive = g_proxy_mount_get_drive (mount);
  if (drive != NULL)
    {
      can_eject = g_drive_can_eject (drive);
      g_object_unref (drive);
    }

  return can_eject;
}

typedef struct {
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
} EjectWrapperOp;

static void
eject_wrapper_callback (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data)
{
  EjectWrapperOp *data  = user_data;
  data->callback (data->object, res, data->user_data);
  g_object_unref (data->object);
  g_free (data);
}

static void
g_proxy_mount_eject_with_operation (GMount              *mount,
                                    GMountUnmountFlags   flags,
                                    GMountOperation     *mount_operation,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
  GDrive *drive;

  drive = g_proxy_mount_get_drive (mount);

  if (drive != NULL)
    {
      EjectWrapperOp *data;
      data = g_new0 (EjectWrapperOp, 1);
      data->object = g_object_ref (mount);
      data->callback = callback;
      data->user_data = user_data;
      g_drive_eject_with_operation (drive, flags, mount_operation, cancellable, eject_wrapper_callback, data);
      g_object_unref (drive);
    }
}

static gboolean
g_proxy_mount_eject_with_operation_finish (GMount        *mount,
                                           GAsyncResult  *result,
                                           GError       **error)
{
  GDrive *drive;
  gboolean res;

  res = TRUE;

  drive = g_proxy_mount_get_drive (mount);

  if (drive != NULL)
    {
      res = g_drive_eject_with_operation_finish (drive, result, error);
      g_object_unref (drive);
    }
  return res;
}

static void
g_proxy_mount_eject (GMount              *mount,
                     GMountUnmountFlags   flags,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  g_proxy_mount_eject_with_operation (mount, flags, NULL, cancellable, callback, user_data);
}

static gboolean
g_proxy_mount_eject_finish (GMount        *mount,
                            GAsyncResult  *result,
                            GError       **error)
{
  return g_proxy_mount_eject_with_operation_finish (mount, result, error);
}

typedef struct {
  GProxyMount *mount;
  GAsyncReadyCallback callback;
  gpointer user_data;

  gchar *cancellation_id;
  GCancellable *cancellable;
  gulong cancelled_handler_id;

  const gchar *mount_op_id;
} DBusOp;

static void
cancel_operation_reply_cb (DBusMessage *reply,
                           GError      *error,
                           gpointer     user_data)
{
  if (error != NULL)
    {
      g_warning ("Error from CancelOperation(): %s", error->message);
    }
}

static void
operation_cancelled (GCancellable *cancellable,
                     gpointer      user_data)
{
  DBusOp *data = user_data;
  GSimpleAsyncResult *simple;
  DBusConnection *connection;
  DBusMessage *message;
  const char *name;

  G_LOCK (proxy_mount);

  simple = g_simple_async_result_new_error (G_OBJECT (data->mount),
                                            data->callback,
                                            data->user_data,
                                            G_IO_ERROR,
                                            G_IO_ERROR_CANCELLED,
                                            _("Operation was cancelled"));
  g_simple_async_result_complete_in_idle (simple);
  g_object_unref (simple);

  /* Now tell the remote volume monitor that the op has been cancelled */
  connection = g_proxy_volume_monitor_get_dbus_connection (data->mount->volume_monitor);
  name = g_proxy_volume_monitor_get_dbus_name (data->mount->volume_monitor);
  message = dbus_message_new_method_call (name,
                                          "/org/gtk/Private/RemoteVolumeMonitor",
                                          "org.gtk.Private.RemoteVolumeMonitor",
                                          "CancelOperation");
  dbus_message_append_args (message,
                            DBUS_TYPE_STRING,
                            &(data->cancellation_id),
                            DBUS_TYPE_INVALID);

  G_UNLOCK (proxy_mount);

  _g_dbus_connection_call_async (connection,
                                 message,
                                 -1,
                                 (GAsyncDBusCallback) cancel_operation_reply_cb,
                                 NULL);
  dbus_message_unref (message);
  dbus_connection_unref (connection);
}

static void
unmount_cb (DBusMessage *reply,
            GError *error,
            DBusOp *data)
{
  if (data->cancelled_handler_id > 0)
    g_signal_handler_disconnect (data->cancellable, data->cancelled_handler_id);

  if (!g_cancellable_is_cancelled (data->cancellable))
    {
      GSimpleAsyncResult *simple;
      if (error != NULL)
        simple = g_simple_async_result_new_from_error (G_OBJECT (data->mount),
                                                       data->callback,
                                                       data->user_data,
                                                       error);
      else
        simple = g_simple_async_result_new (G_OBJECT (data->mount),
                                            data->callback,
                                            data->user_data,
                                            NULL);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
    }

  g_proxy_mount_operation_destroy (data->mount_op_id);
  g_object_unref (data->mount);
  g_free (data->cancellation_id);
  if (data->cancellable != NULL)
    g_object_unref (data->cancellable);
  g_free (data);
}

static void
g_proxy_mount_unmount_with_operation (GMount              *mount,
                                      GMountUnmountFlags   flags,
                                      GMountOperation     *mount_operation,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
  GProxyMount *proxy_mount = G_PROXY_MOUNT (mount);
  DBusConnection *connection;
  const char *name;
  DBusMessage *message;
  DBusOp *data;
  dbus_uint32_t _flags = flags;

  G_LOCK (proxy_mount);

  if (g_cancellable_is_cancelled (cancellable))
    {
      GSimpleAsyncResult *simple;
      simple = g_simple_async_result_new_error (G_OBJECT (mount),
                                                callback,
                                                user_data,
                                                G_IO_ERROR,
                                                G_IO_ERROR_CANCELLED,
                                                _("Operation was cancelled"));
      g_simple_async_result_complete_in_idle (simple);
      g_object_unref (simple);
      G_UNLOCK (proxy_mount);
      goto out;
    }

  data = g_new0 (DBusOp, 1);
  data->mount = g_object_ref (mount);
  data->callback = callback;
  data->user_data = user_data;
  data->mount_op_id = g_proxy_mount_operation_wrap (mount_operation, proxy_mount->volume_monitor);

  if (cancellable != NULL)
    {
      data->cancellation_id = g_strdup_printf ("%p", cancellable);
      data->cancellable = g_object_ref (cancellable);
      data->cancelled_handler_id = g_signal_connect (data->cancellable,
                                                     "cancelled",
                                                     G_CALLBACK (operation_cancelled),
                                                     data);
    }
  else
    {
      data->cancellation_id = g_strdup ("");
    }

  connection = g_proxy_volume_monitor_get_dbus_connection (proxy_mount->volume_monitor);
  name = g_proxy_volume_monitor_get_dbus_name (proxy_mount->volume_monitor);

  message = dbus_message_new_method_call (name,
                                          "/org/gtk/Private/RemoteVolumeMonitor",
                                          "org.gtk.Private.RemoteVolumeMonitor",
                                          "MountUnmount");
  dbus_message_append_args (message,
                            DBUS_TYPE_STRING,
                            &(proxy_mount->id),
                            DBUS_TYPE_STRING,
                            &(data->cancellation_id),
                            DBUS_TYPE_UINT32,
                            &_flags,
                            DBUS_TYPE_STRING,
                            &(data->mount_op_id),
                            DBUS_TYPE_INVALID);
  G_UNLOCK (proxy_mount);

  _g_dbus_connection_call_async (connection,
                                 message,
                                 G_PROXY_VOLUME_MONITOR_DBUS_TIMEOUT, /* 30 minute timeout */
                                 (GAsyncDBusCallback) unmount_cb,
                                 data);

  dbus_message_unref (message);
  dbus_connection_unref (connection);
 out:
  ;
}

static gboolean
g_proxy_mount_unmount_with_operation_finish (GMount        *mount,
                                             GAsyncResult  *result,
                                             GError       **error)
{
  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error))
    return FALSE;
  return TRUE;
}

static void
g_proxy_mount_unmount (GMount              *mount,
                       GMountUnmountFlags   flags,
                       GCancellable        *cancellable,
                       GAsyncReadyCallback  callback,
                       gpointer             user_data)
{
  g_proxy_mount_unmount_with_operation (mount, flags, NULL, cancellable, callback, user_data);
}

static gboolean
g_proxy_mount_unmount_finish (GMount        *mount,
                              GAsyncResult  *result,
                              GError       **error)
{
  return g_proxy_mount_unmount_with_operation_finish (mount, result, error);
}

static void
g_proxy_mount_guess_content_type (GMount              *mount,
                                  gboolean             force_rescan,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  GSimpleAsyncResult *simple;

  /* TODO: handle force_rescan */
  simple = g_simple_async_result_new (G_OBJECT (mount),
                                      callback,
                                      user_data,
                                      NULL);
  g_simple_async_result_complete_in_idle (simple);
  g_object_unref (simple);
}

static char **
g_proxy_mount_guess_content_type_finish (GMount              *mount,
                                         GAsyncResult        *result,
                                         GError             **error)
{
  GProxyMount *proxy_mount = G_PROXY_MOUNT (mount);
  return g_strdupv (proxy_mount->x_content_types);
}

static char **
g_proxy_mount_guess_content_type_sync (GMount              *mount,
                                       gboolean             force_rescan,
                                       GCancellable        *cancellable,
                                       GError             **error)
{
  GProxyMount *proxy_mount = G_PROXY_MOUNT (mount);
  /* TODO: handle force_rescan */
  return g_strdupv (proxy_mount->x_content_types);
}

static void
g_proxy_mount_mount_iface_init (GMountIface *iface)
{
  iface->get_root = g_proxy_mount_get_root;
  iface->get_name = g_proxy_mount_get_name;
  iface->get_icon = g_proxy_mount_get_icon;
  iface->get_uuid = g_proxy_mount_get_uuid;
  iface->get_drive = g_proxy_mount_get_drive;
  iface->get_volume = g_proxy_mount_get_volume;
  iface->can_unmount = g_proxy_mount_can_unmount;
  iface->can_eject = g_proxy_mount_can_eject;
  iface->unmount = g_proxy_mount_unmount;
  iface->unmount_finish = g_proxy_mount_unmount_finish;
  iface->unmount_with_operation = g_proxy_mount_unmount_with_operation;
  iface->unmount_with_operation_finish = g_proxy_mount_unmount_with_operation_finish;
  iface->eject = g_proxy_mount_eject;
  iface->eject_finish = g_proxy_mount_eject_finish;
  iface->eject_with_operation = g_proxy_mount_eject_with_operation;
  iface->eject_with_operation_finish = g_proxy_mount_eject_with_operation_finish;
  iface->guess_content_type = g_proxy_mount_guess_content_type;
  iface->guess_content_type_finish = g_proxy_mount_guess_content_type_finish;
  iface->guess_content_type_sync = g_proxy_mount_guess_content_type_sync;
}

void
g_proxy_mount_register (GIOModule *module)
{
  g_proxy_mount_register_type (G_TYPE_MODULE (module));
}
