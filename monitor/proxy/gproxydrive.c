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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n-lib.h>

#include "gproxyvolumemonitor.h"
#include "gproxydrive.h"
#include "gproxyvolume.h"
#include "gproxymountoperation.h"
#include "gvfsvolumemonitordbus.h"

/* Protects all fields of GProxyDrive that can change */
G_LOCK_DEFINE_STATIC(proxy_drive);

struct _GProxyDrive {
  GObject parent;

  GProxyVolumeMonitor  *volume_monitor;

  char *id;
  char *name;
  GIcon *icon;
  GIcon *symbolic_icon;
  char **volume_ids;
  gboolean can_eject;
  gboolean can_poll_for_media;
  gboolean is_media_check_automatic;
  gboolean has_media;
  gboolean is_removable;
  gboolean is_media_removable;
  gboolean can_start;
  gboolean can_start_degraded;
  gboolean can_stop;
  GDriveStartStopType start_stop_type;

  GHashTable *identifiers;

  gchar *sort_key;
};

static void g_proxy_drive_drive_iface_init (GDriveIface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (GProxyDrive, g_proxy_drive, G_TYPE_OBJECT, 0,
                                G_IMPLEMENT_INTERFACE_DYNAMIC (G_TYPE_DRIVE,
                                                               g_proxy_drive_drive_iface_init))

static void
g_proxy_drive_finalize (GObject *object)
{
  GProxyDrive *drive;

  drive = G_PROXY_DRIVE (object);

  if (drive->volume_monitor != NULL)
    g_object_unref (drive->volume_monitor);
  g_free (drive->id);
  g_free (drive->name);
  if (drive->icon != NULL)
    g_object_unref (drive->icon);
  if (drive->symbolic_icon != NULL)
    g_object_unref (drive->symbolic_icon);
  g_strfreev (drive->volume_ids);
  if (drive->identifiers != NULL)
    g_hash_table_unref (drive->identifiers);
  g_free (drive->sort_key);

  if (G_OBJECT_CLASS (g_proxy_drive_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_proxy_drive_parent_class)->finalize) (object);
}

static void
g_proxy_drive_class_init (GProxyDriveClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_proxy_drive_finalize;
}

static void
g_proxy_drive_class_finalize (GProxyDriveClass *klass)
{
}

static void
g_proxy_drive_init (GProxyDrive *proxy_drive)
{
}

GProxyDrive *
g_proxy_drive_new (GProxyVolumeMonitor *volume_monitor)
{
  GProxyDrive *drive;
  drive = g_object_new (G_TYPE_PROXY_DRIVE, NULL);
  drive->volume_monitor = g_object_ref (volume_monitor);
  g_object_set_data (G_OBJECT (drive),
                     "g-proxy-drive-volume-monitor-name",
                     (gpointer) g_type_name (G_TYPE_FROM_INSTANCE (volume_monitor)));
  return drive;
}

/* string               id
 * string               name
 * string               gicon_data
 * string               symbolic_gicon_data
 * boolean              can-eject
 * boolean              can-poll-for-media
 * boolean              has-media
 * boolean              is-media-removable
 * boolean              is-media-check-automatic
 * boolean              can-start
 * boolean              can-start-degraded
 * boolean              can-stop
 * uint32               start-stop-type
 * array:string         volume-ids
 * dict:string->string  identifiers
 * string               sort_key
 * a{sv}                expansion
 *      boolean              is-removable
 */
#define DRIVE_STRUCT_TYPE "(&s&s&s&sbbbbbbbbuasa{ss}&sa{sv})"

void
g_proxy_drive_update (GProxyDrive  *drive,
                      GVariant     *iter)
{
  const char *id;
  const char *name;
  const char *gicon_data;
  const char *symbolic_gicon_data = NULL;
  gboolean can_eject;
  gboolean can_poll_for_media;
  gboolean has_media;
  gboolean is_media_removable;
  gboolean is_media_check_automatic;
  gboolean can_start;
  gboolean can_start_degraded;
  gboolean can_stop;
  guint32 start_stop_type;
  GPtrArray *volume_ids;
  GHashTable *identifiers;
  const char *sort_key;
  const gchar *volume_id;
  GVariantIter *iter_volume_ids;
  GVariantIter *iter_identifiers;
  GVariantIter *iter_expansion;
  GVariant *value;
  const gchar *key;


  sort_key = NULL;
  g_variant_get (iter, DRIVE_STRUCT_TYPE,
                 &id, &name, &gicon_data,
                 &symbolic_gicon_data,
                 &can_eject, &can_poll_for_media,
                 &has_media, &is_media_removable,
                 &is_media_check_automatic,
                 &can_start, &can_start_degraded,
                 &can_stop, &start_stop_type,
                 &iter_volume_ids,
                 &iter_identifiers,
                 &sort_key,
                 &iter_expansion);

  volume_ids = g_ptr_array_new ();
  while (g_variant_iter_loop (iter_volume_ids, "&s", &volume_id))
    g_ptr_array_add (volume_ids, (gpointer) volume_id);
  g_ptr_array_add (volume_ids, NULL);

  identifiers = _get_identifiers (iter_identifiers);
  if (drive->id != NULL && strcmp (drive->id, id) != 0)
    {
      g_warning ("id mismatch during update of drive");
      goto out;
    }

  if (strlen (name) == 0)
    name = NULL;

  if (sort_key != NULL && strlen (sort_key) == 0)
    sort_key = NULL;

  /* out with the old */
  g_free (drive->id);
  g_free (drive->name);
  if (drive->icon != NULL)
    g_object_unref (drive->icon);
  if (drive->symbolic_icon != NULL)
    g_object_unref (drive->symbolic_icon);
  g_strfreev (drive->volume_ids);
  if (drive->identifiers != NULL)
    g_hash_table_unref (drive->identifiers);
  g_free (drive->sort_key);

  /* in with the new */
  drive->id = g_strdup (id);
  drive->name = g_strdup (name);
  if (*gicon_data == 0)
    drive->icon = NULL;
  else
    drive->icon = g_icon_new_for_string (gicon_data, NULL);
  if (*symbolic_gicon_data == 0)
    drive->symbolic_icon = NULL;
  else
    drive->symbolic_icon = g_icon_new_for_string (symbolic_gicon_data, NULL);
  drive->can_eject = can_eject;
  drive->can_poll_for_media = can_poll_for_media;
  drive->has_media = has_media;
  drive->is_media_removable = is_media_removable;
  drive->is_media_check_automatic = is_media_check_automatic;
  drive->can_start = can_start;
  drive->can_start_degraded = can_start_degraded;
  drive->can_stop = can_stop;
  drive->start_stop_type = start_stop_type;
  drive->identifiers = identifiers != NULL ? g_hash_table_ref (identifiers) : NULL;
  drive->volume_ids = g_strdupv ((char **) volume_ids->pdata);
  drive->sort_key = g_strdup (sort_key);

  drive->is_removable = FALSE;
  while (g_variant_iter_loop (iter_expansion, "{sv}", &key, &value))
    {
      if (g_str_equal (key, "is-removable"))
        drive->is_removable = g_variant_get_boolean (value);
    }

 out:
  g_variant_iter_free (iter_volume_ids);
  g_variant_iter_free (iter_identifiers);
  g_variant_iter_free (iter_expansion);
  g_ptr_array_free (volume_ids, TRUE);
  g_hash_table_unref (identifiers);
}

static GIcon *
g_proxy_drive_get_icon (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  GIcon *icon;

  G_LOCK (proxy_drive);
  icon = proxy_drive->icon != NULL ? g_object_ref (proxy_drive->icon) : NULL;
  G_UNLOCK (proxy_drive);

  return icon;
}

static GIcon *
g_proxy_drive_get_symbolic_icon (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  GIcon *icon;

  G_LOCK (proxy_drive);
  icon = proxy_drive->symbolic_icon != NULL ? g_object_ref (proxy_drive->symbolic_icon) : NULL;
  G_UNLOCK (proxy_drive);

  return icon;
}

static char *
g_proxy_drive_get_name (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  char *name;

  G_LOCK (proxy_drive);
  name = g_strdup (proxy_drive->name);
  G_UNLOCK (proxy_drive);

  return name;
}

static gboolean
volume_compare (GVolume *a, GVolume *b)
{
  return g_strcmp0 (g_volume_get_sort_key (a), g_volume_get_sort_key (b));
}

static GList *
g_proxy_drive_get_volumes (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  GList *l;

  l = NULL;

  G_LOCK (proxy_drive);
  if (proxy_drive->volume_monitor != NULL && proxy_drive->volume_ids != NULL)
    {
      int n;

      for (n = 0; proxy_drive->volume_ids[n] != NULL; n++)
        {
          GProxyVolume *volume;
          volume = g_proxy_volume_monitor_get_volume_for_id (proxy_drive->volume_monitor, proxy_drive->volume_ids[n]);
          if (volume != NULL)
            l = g_list_append (l, volume);
        }
    }
  G_UNLOCK (proxy_drive);

  l = g_list_sort (l, (GCompareFunc) volume_compare);

  return l;
}

static gboolean
g_proxy_drive_has_volumes (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = (proxy_drive->volume_ids != NULL && g_strv_length (proxy_drive->volume_ids) > 0);
  G_UNLOCK (proxy_drive);

  return res;
}

static gboolean
g_proxy_drive_is_removable (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = proxy_drive->is_removable;
  G_UNLOCK (proxy_drive);

  return res;
}

static gboolean
g_proxy_drive_is_media_removable (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = proxy_drive->is_media_removable;
  G_UNLOCK (proxy_drive);

  return res;
}

static gboolean
g_proxy_drive_has_media (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = proxy_drive->has_media;
  G_UNLOCK (proxy_drive);

  return res;
}

static gboolean
g_proxy_drive_is_media_check_automatic (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = proxy_drive->is_media_check_automatic;
  G_UNLOCK (proxy_drive);

  return res;
}

static gboolean
g_proxy_drive_can_eject (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = proxy_drive->can_eject;
  G_UNLOCK (proxy_drive);

  return res;
}

static gboolean
g_proxy_drive_can_poll_for_media (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = proxy_drive->can_poll_for_media;
  G_UNLOCK (proxy_drive);

  return res;
}

static gboolean
g_proxy_drive_can_start (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = proxy_drive->can_start;
  G_UNLOCK (proxy_drive);

  return res;
}

static gboolean
g_proxy_drive_can_start_degraded (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = proxy_drive->can_start_degraded;
  G_UNLOCK (proxy_drive);

  return res;
}

static gboolean
g_proxy_drive_can_stop (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = proxy_drive->can_stop;
  G_UNLOCK (proxy_drive);

  return res;
}

static GDriveStartStopType
g_proxy_drive_get_start_stop_type (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  GDriveStartStopType res;

  G_LOCK (proxy_drive);
  res = proxy_drive->start_stop_type;
  G_UNLOCK (proxy_drive);

  return res;
}

static char *
g_proxy_drive_get_identifier (GDrive              *drive,
                              const char          *kind)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  char *res;

  G_LOCK (proxy_drive);
  if (proxy_drive->identifiers != NULL)
    res = g_strdup (g_hash_table_lookup (proxy_drive->identifiers, kind));
  else
    res = NULL;
  G_UNLOCK (proxy_drive);

  return res;
}

static void
add_identifier_key (const char *key, const char *value, GPtrArray *res)
{
  g_ptr_array_add (res, g_strdup (key));
}

static char **
g_proxy_drive_enumerate_identifiers (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  GPtrArray *res;

  res = g_ptr_array_new ();

  G_LOCK (proxy_drive);
  if (proxy_drive->identifiers != NULL)
    g_hash_table_foreach (proxy_drive->identifiers, (GHFunc) add_identifier_key, res);
  G_UNLOCK (proxy_drive);

  /* Null-terminate */
  g_ptr_array_add (res, NULL);

  return (char **) g_ptr_array_free (res, FALSE);
}

const char *
g_proxy_drive_get_id (GProxyDrive *drive)
{
  return drive->id;
}

typedef struct {
  gchar *cancellation_id;
  gulong cancelled_handler_id;

  const gchar *mount_op_id;
} DBusOp;

static void
dbus_op_free (DBusOp *data)
{
  g_free (data->cancellation_id);

  if (data->mount_op_id)
    g_proxy_mount_operation_destroy (data->mount_op_id);

  g_free (data);
}

static void
cancel_operation_reply_cb (GVfsRemoteVolumeMonitor *proxy,
                           GAsyncResult *res,
                           gpointer user_data)
{
  gboolean out_WasCancelled;
  GError *error = NULL;
  
  if (!gvfs_remote_volume_monitor_call_cancel_operation_finish (proxy,
                                                                &out_WasCancelled,
                                                                res,
                                                                &error))
    {
      g_warning ("Error from CancelOperation(): %s", error->message);
      g_error_free (error);
    }
}

static void
operation_cancelled (GCancellable *cancellable,
                     gpointer      user_data)
{
  GTask *task = G_TASK (user_data);
  DBusOp *data = g_task_get_task_data (task);
  GVfsRemoteVolumeMonitor *proxy;
  GProxyDrive *drive = G_PROXY_DRIVE (g_task_get_source_object (task));

  G_LOCK (proxy_drive);

  /* Now tell the remote volume monitor that the op has been cancelled */
  proxy = g_proxy_volume_monitor_get_dbus_proxy (drive->volume_monitor);
  gvfs_remote_volume_monitor_call_cancel_operation (proxy,
                                                    data->cancellation_id,
                                                    NULL,
                                                    (GAsyncReadyCallback) cancel_operation_reply_cb,
                                                    NULL);
  g_object_unref (proxy);

  G_UNLOCK (proxy_drive);

  g_task_return_error_if_cancelled (task);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
eject_cb (GVfsRemoteVolumeMonitor *proxy,
          GAsyncResult *res,
          gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  DBusOp *data = g_task_get_task_data (task);
  GError *error = NULL;
 
  gvfs_remote_volume_monitor_call_drive_eject_finish (proxy, 
                                                      res, 
                                                      &error);

  if (data->cancelled_handler_id > 0)
    g_signal_handler_disconnect (g_task_get_cancellable (task), data->cancelled_handler_id);

  if (!g_cancellable_is_cancelled (g_task_get_cancellable (task)))
    {
      if (error != NULL)
        {
          g_dbus_error_strip_remote_error (error);
          g_task_return_error (task, error);
          error = NULL;
        }
      else
        {
          g_task_return_boolean (task, TRUE);
        }
    }

  g_object_unref (task);
  if (error != NULL)
    g_error_free (error);
}

static void
g_proxy_drive_eject_with_operation (GDrive              *drive,
                                    GMountUnmountFlags   flags,
                                    GMountOperation     *mount_operation,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  DBusOp *data;
  GVfsRemoteVolumeMonitor *proxy;
  GTask *task;

  G_LOCK (proxy_drive);

  task = g_task_new (drive, cancellable, callback, user_data);
  g_task_set_source_tag (task, g_proxy_drive_eject_with_operation);

  if (g_cancellable_is_cancelled (cancellable))
    {
      G_UNLOCK (proxy_drive);

      g_task_return_error_if_cancelled (task);
      g_object_unref (task);
      return;
    }

  data = g_new0 (DBusOp, 1);
  data->mount_op_id = g_proxy_mount_operation_wrap (mount_operation, proxy_drive->volume_monitor);

  if (cancellable != NULL)
    {
      data->cancellation_id = g_strdup_printf ("%p", cancellable);
      data->cancelled_handler_id = g_signal_connect (cancellable,
                                                     "cancelled",
                                                     G_CALLBACK (operation_cancelled),
                                                     task);
    }
  else
    {
      data->cancellation_id = g_strdup ("");
    }

  g_task_set_task_data (task, data, (GDestroyNotify)dbus_op_free);

  proxy = g_proxy_volume_monitor_get_dbus_proxy (proxy_drive->volume_monitor);
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), G_PROXY_VOLUME_MONITOR_DBUS_TIMEOUT);  /* 30 minute timeout */
  
  gvfs_remote_volume_monitor_call_drive_eject (proxy,
                                               proxy_drive->id,
                                               data->cancellation_id,
                                               flags,
                                               data->mount_op_id,
                                               NULL,
                                               (GAsyncReadyCallback) eject_cb,
                                               task);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), -1);
  g_object_unref (proxy);

  G_UNLOCK (proxy_drive);
}

static gboolean
g_proxy_drive_eject_with_operation_finish (GDrive        *drive,
                                           GAsyncResult  *result,
                                           GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, drive), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_proxy_drive_eject_with_operation), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_proxy_drive_eject (GDrive              *drive,
                     GMountUnmountFlags   flags,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  g_proxy_drive_eject_with_operation (drive, flags, NULL, cancellable, callback, user_data);
}

static gboolean
g_proxy_drive_eject_finish (GDrive        *drive,
                            GAsyncResult  *result,
                            GError       **error)
{
  return g_proxy_drive_eject_with_operation_finish (drive, result, error);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
stop_cb (GVfsRemoteVolumeMonitor *proxy,
         GAsyncResult *res,
         gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  DBusOp *data = g_task_get_task_data (task);
  GError *error = NULL;

  gvfs_remote_volume_monitor_call_drive_stop_finish (proxy, 
                                                     res, 
                                                     &error);

  if (data->cancelled_handler_id > 0)
    g_signal_handler_disconnect (g_task_get_cancellable (task), data->cancelled_handler_id);

  if (!g_cancellable_is_cancelled (g_task_get_cancellable (task)))
    {
      if (error != NULL)
        {
          g_dbus_error_strip_remote_error (error);
          g_task_return_error (task, error);
          error = NULL;
        }
      else
        {
          g_task_return_boolean (task, TRUE);
        }
    }

  g_object_unref (task);
  if (error != NULL)
    g_error_free (error);
}

static void
g_proxy_drive_stop (GDrive              *drive,
                    GMountUnmountFlags   flags,
                    GMountOperation     *mount_operation,
                    GCancellable        *cancellable,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  DBusOp *data;
  GVfsRemoteVolumeMonitor *proxy;
  GTask *task;

  G_LOCK (proxy_drive);

  task = g_task_new (drive, cancellable, callback, user_data);
  g_task_set_source_tag (task, g_proxy_drive_stop);

  if (g_cancellable_is_cancelled (cancellable))
    {
      G_UNLOCK (proxy_drive);

      g_task_return_error_if_cancelled (task);
      g_object_unref (task);
      return;
    }

  data = g_new0 (DBusOp, 1);
  data->mount_op_id = g_proxy_mount_operation_wrap (mount_operation, proxy_drive->volume_monitor);

  if (cancellable != NULL)
    {
      data->cancellation_id = g_strdup_printf ("%p", cancellable);
      data->cancelled_handler_id = g_signal_connect (cancellable,
                                                     "cancelled",
                                                     G_CALLBACK (operation_cancelled),
                                                     task);
    }
  else
    {
      data->cancellation_id = g_strdup ("");
    }

  g_task_set_task_data (task, data, (GDestroyNotify)dbus_op_free);

  proxy = g_proxy_volume_monitor_get_dbus_proxy (proxy_drive->volume_monitor);
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), G_PROXY_VOLUME_MONITOR_DBUS_TIMEOUT);  /* 30 minute timeout */

  gvfs_remote_volume_monitor_call_drive_stop (proxy,
                                              proxy_drive->id,
                                              data->cancellation_id,
                                              flags,
                                              data->mount_op_id,
                                              NULL,
                                              (GAsyncReadyCallback) stop_cb,
                                              task);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), -1);
  g_object_unref (proxy);

  G_UNLOCK (proxy_drive);
}

static gboolean
g_proxy_drive_stop_finish (GDrive        *drive,
                           GAsyncResult  *result,
                           GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, drive), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_proxy_drive_stop), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
start_cb (GVfsRemoteVolumeMonitor *proxy,
          GAsyncResult *res,
          gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  DBusOp *data = g_task_get_task_data (task);
  GError *error = NULL;

  gvfs_remote_volume_monitor_call_drive_start_finish (proxy,
                                                      res,
                                                      &error);

  if (data->cancelled_handler_id > 0)
    g_signal_handler_disconnect (g_task_get_cancellable (task), data->cancelled_handler_id);

  if (!g_cancellable_is_cancelled (g_task_get_cancellable (task)))
    {
      if (error != NULL)
        {
          g_dbus_error_strip_remote_error (error);
          g_task_return_error (task, error);
          error = NULL;
        }
      else
        {
          g_task_return_boolean (task, TRUE);
        }
    }

  g_object_unref (task);
  if (error != NULL)
    g_error_free (error);
}

static void
start_cancelled (GCancellable *cancellable,
                 gpointer      user_data)
{
  GTask *task = G_TASK (user_data);
  DBusOp *data = g_task_get_task_data (task);
  GVfsRemoteVolumeMonitor *proxy;
  GProxyDrive *drive = G_PROXY_DRIVE (g_task_get_source_object (task));

  G_LOCK (proxy_drive);

  /* Now tell the remote drive monitor that the op has been cancelled */
  proxy = g_proxy_volume_monitor_get_dbus_proxy (drive->volume_monitor);
  gvfs_remote_volume_monitor_call_cancel_operation (proxy,
                                                    data->cancellation_id,
                                                    NULL,
                                                    (GAsyncReadyCallback) cancel_operation_reply_cb,
                                                    NULL);
  g_object_unref (proxy);

  G_UNLOCK (proxy_drive);

  g_task_return_error_if_cancelled (task);
}

static void
g_proxy_drive_start (GDrive              *drive,
                     GDriveStartFlags     flags,
                     GMountOperation     *mount_operation,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  DBusOp *data;
  GVfsRemoteVolumeMonitor *proxy;
  GTask *task;

  G_LOCK (proxy_drive);

  task = g_task_new (drive, cancellable, callback, user_data);
  g_task_set_source_tag (task, g_proxy_drive_start);

  if (g_cancellable_is_cancelled (cancellable))
    {
      G_UNLOCK (proxy_drive);

      g_task_return_error_if_cancelled (task);
      g_object_unref (task);
      return;
    }

  data = g_new0 (DBusOp, 1);
  if (cancellable != NULL)
    {
      data->cancellation_id = g_strdup_printf ("%p", cancellable);
      data->cancelled_handler_id = g_signal_connect (cancellable,
                                                     "cancelled",
                                                     G_CALLBACK (start_cancelled),
                                                     task);
    }
  else
    {
      data->cancellation_id = g_strdup ("");
    }

  data->mount_op_id = g_proxy_mount_operation_wrap (mount_operation, proxy_drive->volume_monitor);

  g_task_set_task_data (task, data, (GDestroyNotify)dbus_op_free);

  proxy = g_proxy_volume_monitor_get_dbus_proxy (proxy_drive->volume_monitor);
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), G_PROXY_VOLUME_MONITOR_DBUS_TIMEOUT);  /* 30 minute timeout */

  gvfs_remote_volume_monitor_call_drive_start (proxy,
                                               proxy_drive->id,
                                               data->cancellation_id,
                                               flags,
                                               data->mount_op_id,
                                               NULL,
                                               (GAsyncReadyCallback) start_cb,
                                               task);

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), -1);
  g_object_unref (proxy);

  G_UNLOCK (proxy_drive);
}

static gboolean
g_proxy_drive_start_finish (GDrive        *drive,
                            GAsyncResult  *result,
                            GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, drive), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_proxy_drive_start), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
poll_for_media_cb (GVfsRemoteVolumeMonitor *proxy,
                   GAsyncResult *res,
                   gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  DBusOp *data = g_task_get_task_data (task);
  GError *error = NULL;

  gvfs_remote_volume_monitor_call_drive_poll_for_media_finish (proxy, 
                                                               res, 
                                                               &error);

  if (!g_cancellable_is_cancelled (g_task_get_cancellable (task)))
    {
      if (error != NULL)
        {
          g_dbus_error_strip_remote_error (error);
          g_task_return_error (task, error);
          error = NULL;
        }
      else
        {
          g_task_return_boolean (task, TRUE);
        }
    }

  if (data->cancelled_handler_id > 0)
    g_signal_handler_disconnect (g_task_get_cancellable (task), data->cancelled_handler_id);

  g_object_unref (task);
  if (error != NULL)
    g_error_free (error);
}

static void
g_proxy_drive_poll_for_media (GDrive              *drive,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  DBusOp *data;
  GVfsRemoteVolumeMonitor *proxy;
  GTask *task;

  G_LOCK (proxy_drive);

  task = g_task_new (drive, cancellable, callback, user_data);
  g_task_set_source_tag (task, g_proxy_drive_poll_for_media);

  if (g_cancellable_is_cancelled (cancellable))
    {
      G_UNLOCK (proxy_drive);

      g_task_return_error_if_cancelled (task);
      g_object_unref (task);
      return;
    }

  data = g_new0 (DBusOp, 1);
  if (cancellable != NULL)
    {
      data->cancellation_id = g_strdup_printf ("%p", cancellable);
      data->cancelled_handler_id = g_signal_connect (cancellable,
                                                     "cancelled",
                                                     G_CALLBACK (operation_cancelled),
                                                     task);
    }
  else
    {
      data->cancellation_id = g_strdup ("");
    }

  g_task_set_task_data (task, data, (GDestroyNotify)dbus_op_free);

  proxy = g_proxy_volume_monitor_get_dbus_proxy (proxy_drive->volume_monitor);
  gvfs_remote_volume_monitor_call_drive_poll_for_media (proxy,
                                                        proxy_drive->id,
                                                        data->cancellation_id,
                                                        NULL,
                                                        (GAsyncReadyCallback) poll_for_media_cb,
                                                        task);
  g_object_unref (proxy);

  G_UNLOCK (proxy_drive);
}

static gboolean
g_proxy_drive_poll_for_media_finish (GDrive        *drive,
                                     GAsyncResult  *result,
                                     GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, drive), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_proxy_drive_poll_for_media), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---------------------------------------------------------------------------------------------------- */

static const gchar *
g_proxy_drive_get_sort_key (GDrive *_drive)
{
  GProxyDrive *drive = G_PROXY_DRIVE (_drive);
  return drive->sort_key;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_proxy_drive_drive_iface_init (GDriveIface *iface)
{
  iface->get_name = g_proxy_drive_get_name;
  iface->get_icon = g_proxy_drive_get_icon;
  iface->get_symbolic_icon = g_proxy_drive_get_symbolic_icon;
  iface->has_volumes = g_proxy_drive_has_volumes;
  iface->get_volumes = g_proxy_drive_get_volumes;
  iface->is_removable = g_proxy_drive_is_removable;
  iface->is_media_removable = g_proxy_drive_is_media_removable;
  iface->has_media = g_proxy_drive_has_media;
  iface->is_media_check_automatic = g_proxy_drive_is_media_check_automatic;
  iface->can_eject = g_proxy_drive_can_eject;
  iface->can_poll_for_media = g_proxy_drive_can_poll_for_media;
  iface->eject = g_proxy_drive_eject;
  iface->eject_finish = g_proxy_drive_eject_finish;
  iface->eject_with_operation = g_proxy_drive_eject_with_operation;
  iface->eject_with_operation_finish = g_proxy_drive_eject_with_operation_finish;
  iface->poll_for_media = g_proxy_drive_poll_for_media;
  iface->poll_for_media_finish = g_proxy_drive_poll_for_media_finish;
  iface->get_identifier = g_proxy_drive_get_identifier;
  iface->enumerate_identifiers = g_proxy_drive_enumerate_identifiers;
  iface->can_start = g_proxy_drive_can_start;
  iface->can_start_degraded = g_proxy_drive_can_start_degraded;
  iface->start = g_proxy_drive_start;
  iface->start_finish = g_proxy_drive_start_finish;
  iface->can_stop = g_proxy_drive_can_stop;
  iface->stop = g_proxy_drive_stop;
  iface->stop_finish = g_proxy_drive_stop_finish;
  iface->get_start_stop_type = g_proxy_drive_get_start_stop_type;
  iface->get_sort_key = g_proxy_drive_get_sort_key;
}

void
g_proxy_drive_register (GIOModule *module)
{
  g_proxy_drive_register_type (G_TYPE_MODULE (module));
}
