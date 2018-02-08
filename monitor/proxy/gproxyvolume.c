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
#include <gio/gio.h>

#include "gproxydrive.h"
#include "gproxyvolume.h"
#include "gproxymount.h"

#include "gproxymountoperation.h"

static void signal_emit_in_idle (gpointer object, const char *signal_name, gpointer other_object);

/* Protects all fields of GProxyVolume that can change */
G_LOCK_DEFINE_STATIC(proxy_volume);

struct _GProxyVolume {
  GObject parent;

  GProxyVolumeMonitor *volume_monitor;

  /* non-NULL only if activation_uri != NULL */
  GVolumeMonitor *union_monitor;

  char *id;
  char *name;
  char *uuid;
  char *activation_uri;
  GIcon *icon;
  GIcon *symbolic_icon;
  char *drive_id;
  char *mount_id;
  GHashTable *identifiers;

  gboolean can_mount;
  gboolean should_automount;
  gboolean always_call_mount;

  GProxyShadowMount *shadow_mount;

  gchar *sort_key;
};

static void g_proxy_volume_volume_iface_init (GVolumeIface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (GProxyVolume, g_proxy_volume, G_TYPE_OBJECT, 0,
                                G_IMPLEMENT_INTERFACE_DYNAMIC (G_TYPE_VOLUME,
                                                               g_proxy_volume_volume_iface_init))


static void union_monitor_mount_added (GVolumeMonitor *union_monitor,
                                       GMount         *mount,
                                       GProxyVolume   *volume);

static void union_monitor_mount_removed (GVolumeMonitor *union_monitor,
                                         GMount         *mount,
                                         GProxyVolume   *volume);

static void union_monitor_mount_changed (GVolumeMonitor *union_monitor,
                                         GMount         *mount,
                                         GProxyVolume   *volume);

static void update_shadow_mount (GProxyVolume *volume);

GProxyShadowMount *
g_proxy_volume_get_shadow_mount (GProxyVolume *volume)
{
  if (volume->shadow_mount != NULL)
    return g_object_ref (volume->shadow_mount);
  else
    return NULL;
}

static void
g_proxy_volume_finalize (GObject *object)
{
  GProxyVolume *volume;

  volume = G_PROXY_VOLUME (object);

  g_free (volume->id);
  g_free (volume->name);
  g_free (volume->uuid);
  g_free (volume->activation_uri);
  if (volume->icon != NULL)
    g_object_unref (volume->icon);
  if (volume->symbolic_icon != NULL)
    g_object_unref (volume->symbolic_icon);
  g_free (volume->drive_id);
  g_free (volume->mount_id);
  if (volume->identifiers != NULL)
    g_hash_table_unref (volume->identifiers);

  if (volume->volume_monitor != NULL)
    {
      g_object_unref (volume->volume_monitor);
    }
  g_free (volume->sort_key);

  if (G_OBJECT_CLASS (g_proxy_volume_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_proxy_volume_parent_class)->finalize) (object);
}

static void
g_proxy_volume_dispose (GObject *object)
{
  GProxyVolume *volume;

  volume = G_PROXY_VOLUME (object);

  if (volume->shadow_mount != NULL)
    {
      signal_emit_in_idle (volume->shadow_mount, "unmounted", NULL);
      signal_emit_in_idle (volume->volume_monitor, "mount-removed", volume->shadow_mount);
      g_proxy_shadow_mount_remove (volume->shadow_mount);
      g_object_unref (volume->shadow_mount);
      
      volume->shadow_mount = NULL;
    }
  
  if (volume->union_monitor != NULL)
    {
      g_signal_handlers_disconnect_by_func (volume->union_monitor,
                                            union_monitor_mount_added, volume);
      g_signal_handlers_disconnect_by_func (volume->union_monitor,
                                            union_monitor_mount_removed, volume);
      g_signal_handlers_disconnect_by_func (volume->union_monitor,
                                            union_monitor_mount_changed, volume);
      g_object_unref (volume->union_monitor);

      volume->union_monitor = NULL;
    }
  
  if (G_OBJECT_CLASS (g_proxy_volume_parent_class)->dispose)
    (*G_OBJECT_CLASS (g_proxy_volume_parent_class)->dispose) (object);
}


static void
g_proxy_volume_class_init (GProxyVolumeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = g_proxy_volume_dispose;
  gobject_class->finalize = g_proxy_volume_finalize;
}

static void
g_proxy_volume_class_finalize (GProxyVolumeClass *klass)
{
}

static void
g_proxy_volume_init (GProxyVolume *proxy_volume)
{
}

GProxyVolume *
g_proxy_volume_new (GProxyVolumeMonitor *volume_monitor)
{
  GProxyVolume *volume;
  volume = g_object_new (G_TYPE_PROXY_VOLUME, NULL);
  volume->volume_monitor = g_object_ref (volume_monitor);
  g_object_set_data (G_OBJECT (volume),
                     "g-proxy-volume-volume-monitor-name",
                     (gpointer) g_type_name (G_TYPE_FROM_INSTANCE (volume_monitor)));
  return volume;
}

static void
union_monitor_mount_added (GVolumeMonitor *union_monitor,
                           GMount         *mount,
                           GProxyVolume   *volume)
{
  update_shadow_mount (volume);
}

static void
union_monitor_mount_removed (GVolumeMonitor *union_monitor,
                             GMount         *mount,
                             GProxyVolume   *volume)
{
  update_shadow_mount (volume);
}

static void
union_monitor_mount_changed (GVolumeMonitor *union_monitor,
                             GMount         *mount,
                             GProxyVolume   *volume)
{
  if (volume->shadow_mount != NULL)
    {
      GMount *real_mount;
      real_mount = g_proxy_shadow_mount_get_real_mount (volume->shadow_mount);
      if (mount == real_mount)
        {
          signal_emit_in_idle (volume->shadow_mount, "changed", NULL);
          signal_emit_in_idle (volume->volume_monitor, "mount-changed", volume->shadow_mount);
        }
      g_object_unref (real_mount);
    }
}

static void
update_shadow_mount (GProxyVolume *volume)
{
  GFile *activation_root;
  GList *mounts;
  GList *l;
  GMount *mount_to_shadow;

  activation_root = NULL;
  mount_to_shadow = NULL;

  if (volume->activation_uri == NULL)
    goto out;

  activation_root = g_file_new_for_uri (volume->activation_uri);

  if (volume->union_monitor == NULL)
    {
      volume->union_monitor = g_volume_monitor_get ();
      g_signal_connect (volume->union_monitor, "mount-added", (GCallback) union_monitor_mount_added, volume);
      g_signal_connect (volume->union_monitor, "mount-removed", (GCallback) union_monitor_mount_removed, volume);
      g_signal_connect (volume->union_monitor, "mount-changed", (GCallback) union_monitor_mount_changed, volume);
    }

  mounts = g_volume_monitor_get_mounts (volume->union_monitor);
  for (l = mounts; l != NULL; l = l->next)
    {
      GMount *mount = G_MOUNT (l->data);
      GFile *mount_root;
      gboolean prefix_matches;

      /* don't consider our (possibly) existing shadow mount */
      if (G_IS_PROXY_SHADOW_MOUNT (mount))
        continue;

      mount_root = g_mount_get_root (mount);
      prefix_matches = (g_file_has_prefix (activation_root, mount_root) ||
                        g_file_equal (activation_root, mount_root));
      g_object_unref (mount_root);
      if (prefix_matches)
        {
          mount_to_shadow = g_object_ref (mount);
          break;
        }
    }
  g_list_free_full (mounts, g_object_unref);

  if (mount_to_shadow != NULL)
    {
      /* there's now a mount to shadow, if we don't have a GProxyShadowMount then create one */
      if (volume->shadow_mount == NULL)
        {
          volume->shadow_mount = g_proxy_shadow_mount_new (volume->volume_monitor,
                                                           volume,
                                                           mount_to_shadow);
          signal_emit_in_idle (volume->volume_monitor, "mount-added", volume->shadow_mount);
        }
      else
        {
          GFile *current_activation_root;

          /* we have a GProxyShadowMount already. However, we need to replace it if the
           * activation root has changed.
           */
          current_activation_root = g_proxy_shadow_mount_get_activation_root (volume->shadow_mount);
          if (!g_file_equal (current_activation_root, activation_root))
            {
              signal_emit_in_idle (volume->shadow_mount, "unmounted", NULL);
              signal_emit_in_idle (volume->volume_monitor, "mount-removed", volume->shadow_mount);
              g_proxy_shadow_mount_remove (volume->shadow_mount);
              g_object_unref (volume->shadow_mount);
              volume->shadow_mount = NULL;

              volume->shadow_mount = g_proxy_shadow_mount_new (volume->volume_monitor,
                                                               volume,
                                                               mount_to_shadow);
              signal_emit_in_idle (volume->volume_monitor, "mount-added", volume->shadow_mount);
            }
          g_object_unref (current_activation_root);
        }
    }
  else
    {
      /* no mount to shadow; if we have a GProxyShadowMount then remove it */
      if (volume->shadow_mount != NULL)
        {
          signal_emit_in_idle (volume->shadow_mount, "unmounted", NULL);
          signal_emit_in_idle (volume->volume_monitor, "mount-removed", volume->shadow_mount);
          g_proxy_shadow_mount_remove (volume->shadow_mount);
          g_object_unref (volume->shadow_mount);
          volume->shadow_mount = NULL;
        }
    }

 out:

  if (activation_root != NULL)
    g_object_unref (activation_root);

  if (mount_to_shadow != NULL)
    g_object_unref (mount_to_shadow);
}

static gboolean
update_shadow_mount_in_idle_do (GProxyVolume *volume)
{
  update_shadow_mount (volume);
  g_object_unref (volume);
  return FALSE;
}

static void
update_shadow_mount_in_idle (GProxyVolume *volume)
{
  g_idle_add ((GSourceFunc) update_shadow_mount_in_idle_do, g_object_ref (volume));
}

/* string               id
 * string               name
 * string               gicon_data
 * string               symbolic_gicon_data
 * string               uuid
 * string               activation_uri
 * boolean              can-mount
 * boolean              should-automount
 * string               drive-id
 * string               mount-id
 * dict:string->string  identifiers
 * string               sort_key
 * a{sv}                expansion
 */

#define VOLUME_STRUCT_TYPE "(&s&s&s&s&s&sbb&s&sa{ss}&s@a{sv})"

void g_proxy_volume_update (GProxyVolume    *volume,
                            GVariant        *iter)
{
  const char *id;
  const char *name;
  const char *gicon_data;
  const char *symbolic_gicon_data = NULL;
  const char *uuid;
  const char *activation_uri;
  const char *drive_id;
  const char *mount_id;
  gboolean can_mount;
  gboolean should_automount;
  GHashTable *identifiers;
  const gchar *sort_key;
  GVariantIter *iter_identifiers;
  GVariant *expansion;

  sort_key = NULL;
  g_variant_get (iter, VOLUME_STRUCT_TYPE,
                 &id, &name, &gicon_data, 
                 &symbolic_gicon_data,
                 &uuid, &activation_uri, 
                 &can_mount, &should_automount, 
                 &drive_id, &mount_id, 
                 &iter_identifiers,
                 &sort_key,
                 &expansion);

  identifiers = _get_identifiers (iter_identifiers);

  if (volume->id != NULL && strcmp (volume->id, id) != 0)
    {
      g_warning ("id mismatch during update of volume");
      goto out;
    }

  if (strlen (name) == 0)
    name = NULL;
  if (strlen (uuid) == 0)
    uuid = NULL;
  if (strlen (activation_uri) == 0)
    activation_uri = NULL;
  if (sort_key != NULL && strlen (sort_key) == 0)
    sort_key = NULL;

  /* out with the old */
  g_free (volume->id);
  g_free (volume->name);
  g_free (volume->uuid);
  g_free (volume->activation_uri);
  if (volume->icon != NULL)
    g_object_unref (volume->icon);
  if (volume->symbolic_icon != NULL)
    g_object_unref (volume->symbolic_icon);
  g_free (volume->drive_id);
  g_free (volume->mount_id);
  if (volume->identifiers != NULL)
    g_hash_table_unref (volume->identifiers);
  g_free (volume->sort_key);

  /* in with the new */
  volume->id = g_strdup (id);
  volume->name = g_strdup (name);
  volume->uuid = g_strdup (uuid);
  volume->activation_uri = g_strdup (activation_uri);
  if (*gicon_data == 0)
    volume->icon = NULL;
  else
    volume->icon = g_icon_new_for_string (gicon_data, NULL);
  if (*symbolic_gicon_data == 0)
    volume->symbolic_icon = NULL;
  else
    volume->symbolic_icon = g_icon_new_for_string (symbolic_gicon_data, NULL);
  volume->drive_id = g_strdup (drive_id);
  volume->mount_id = g_strdup (mount_id);
  volume->can_mount = can_mount;
  volume->should_automount = should_automount;
  volume->identifiers = identifiers != NULL ? g_hash_table_ref (identifiers) : NULL;
  volume->sort_key = g_strdup (sort_key);

  if (volume->activation_uri)
    {
      if (!g_variant_lookup (expansion, "always-call-mount", "b", &volume->always_call_mount))
        volume->always_call_mount = FALSE;
    }
  else
    volume->always_call_mount = FALSE;

  /* this calls into the union monitor; do it in idle to avoid locking issues */
  update_shadow_mount_in_idle (volume);

 out:
  g_variant_iter_free (iter_identifiers);
  g_variant_unref (expansion);
  g_hash_table_unref (identifiers);
}

const char *
g_proxy_volume_get_id (GProxyVolume *volume)
{
  return volume->id;
}

static GIcon *
g_proxy_volume_get_icon (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GIcon *icon;

  G_LOCK (proxy_volume);
  icon = proxy_volume->icon != NULL ? g_object_ref (proxy_volume->icon) : NULL;
  G_UNLOCK (proxy_volume);
  return icon;
}

static GIcon *
g_proxy_volume_get_symbolic_icon (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GIcon *icon;

  G_LOCK (proxy_volume);
  icon = proxy_volume->symbolic_icon != NULL ? g_object_ref (proxy_volume->symbolic_icon) : NULL;
  G_UNLOCK (proxy_volume);
  return icon;
}

static char *
g_proxy_volume_get_name (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  char *name;

  G_LOCK (proxy_volume);
  name = g_strdup (proxy_volume->name);
  G_UNLOCK (proxy_volume);
  return name;
}

static char *
g_proxy_volume_get_uuid (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  char *uuid;

  G_LOCK (proxy_volume);
  uuid = g_strdup (proxy_volume->uuid);
  G_UNLOCK (proxy_volume);
  return uuid;
}

static gboolean
g_proxy_volume_can_mount (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  gboolean res;

  G_LOCK (proxy_volume);
  res = proxy_volume->can_mount;
  G_UNLOCK (proxy_volume);
  return res;
}

static gboolean
g_proxy_volume_can_eject (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GProxyDrive *drive;
  gboolean res;

  G_LOCK (proxy_volume);
  res = FALSE;
  if (proxy_volume->drive_id != NULL && strlen (proxy_volume->drive_id) > 0)
    {
      drive = g_proxy_volume_monitor_get_drive_for_id (proxy_volume->volume_monitor,
                                                       proxy_volume->drive_id);
      if (drive != NULL)
        {
          res = g_drive_can_eject (G_DRIVE (drive));
          g_object_unref (drive);
        }
    }
  G_UNLOCK (proxy_volume);

  return res;
}

static gboolean
g_proxy_volume_should_automount (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  gboolean res;

  G_LOCK (proxy_volume);
  res = proxy_volume->should_automount;
  G_UNLOCK (proxy_volume);

  return res;
}

static GDrive *
g_proxy_volume_get_drive (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GProxyDrive *drive;

  G_LOCK (proxy_volume);
  drive = NULL;
  if (proxy_volume->drive_id != NULL && strlen (proxy_volume->drive_id) > 0)
    drive = g_proxy_volume_monitor_get_drive_for_id (proxy_volume->volume_monitor,
                                                     proxy_volume->drive_id);
  G_UNLOCK (proxy_volume);

  return drive != NULL ? G_DRIVE (drive) : NULL;
}

static GMount *
g_proxy_volume_get_mount (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GMount *mount;

  mount = NULL;

  G_LOCK (proxy_volume);

  if (proxy_volume->shadow_mount != NULL)
    {
      mount = G_MOUNT (g_object_ref (proxy_volume->shadow_mount));
    }
  else if (proxy_volume->mount_id != NULL && strlen (proxy_volume->mount_id) > 0)
    {
      GProxyMount *proxy_mount;
      proxy_mount = g_proxy_volume_monitor_get_mount_for_id (proxy_volume->volume_monitor,
                                                             proxy_volume->mount_id);
      if (proxy_mount != NULL)
        mount = G_MOUNT (proxy_mount);
    }
  G_UNLOCK (proxy_volume);

  return mount;
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
g_proxy_volume_eject_with_operation (GVolume             *volume,
                                     GMountUnmountFlags   flags,
                                     GMountOperation     *mount_operation,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GProxyDrive *drive;

  drive = NULL;
  G_LOCK (proxy_volume);
  if (proxy_volume->drive_id != NULL && strlen (proxy_volume->drive_id) > 0)
    {
      drive = g_proxy_volume_monitor_get_drive_for_id (proxy_volume->volume_monitor,
                                                       proxy_volume->drive_id);
    }
  G_UNLOCK (proxy_volume);

  if (drive != NULL)
    {
      EjectWrapperOp *data;
      data = g_new0 (EjectWrapperOp, 1);
      data->object = G_OBJECT (g_object_ref (volume));
      data->callback = callback;
      data->user_data = user_data;
      g_drive_eject_with_operation (G_DRIVE (drive), flags, mount_operation, cancellable, eject_wrapper_callback, data);
      g_object_unref (drive);
    }
}

static gboolean
g_proxy_volume_eject_with_operation_finish (GVolume        *volume,
                                            GAsyncResult  *result,
                                            GError       **error)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GProxyDrive *drive;
  gboolean res;

  G_LOCK (proxy_volume);
  res = TRUE;
  drive = NULL;
  if (proxy_volume->drive_id != NULL && strlen (proxy_volume->drive_id) > 0)
    drive = g_proxy_volume_monitor_get_drive_for_id (proxy_volume->volume_monitor,
                                                     proxy_volume->drive_id);
  G_UNLOCK (proxy_volume);

  if (drive != NULL)
    {
      res = g_drive_eject_with_operation_finish (G_DRIVE (drive), result, error);
      g_object_unref (drive);
    }

  return res;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_proxy_volume_eject (GVolume              *volume,
                      GMountUnmountFlags   flags,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  g_proxy_volume_eject_with_operation (volume, flags, NULL, cancellable, callback, user_data);
}

static gboolean
g_proxy_volume_eject_finish (GVolume        *volume,
                             GAsyncResult  *result,
                             GError       **error)
{
  return g_proxy_volume_eject_with_operation_finish (volume, result, error);
}

/* ---------------------------------------------------------------------------------------------------- */

static char *
g_proxy_volume_get_identifier (GVolume              *volume,
                               const char          *kind)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  char *res;

  G_LOCK (proxy_volume);
  if (proxy_volume->identifiers != NULL)
    res = g_strdup (g_hash_table_lookup (proxy_volume->identifiers, kind));
  else
    res = NULL;
  G_UNLOCK (proxy_volume);

  return res;
}

static void
add_identifier_key (const char *key, const char *value, GPtrArray *res)
{
  g_ptr_array_add (res, g_strdup (key));
}

static char **
g_proxy_volume_enumerate_identifiers (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GPtrArray *res;

  res = g_ptr_array_new ();

  G_LOCK (proxy_volume);
  if (proxy_volume->identifiers != NULL)
    g_hash_table_foreach (proxy_volume->identifiers, (GHFunc) add_identifier_key, res);
  G_UNLOCK (proxy_volume);

  /* Null-terminate */
  g_ptr_array_add (res, NULL);

  return (char **) g_ptr_array_free (res, FALSE);
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
mount_cb (GVfsRemoteVolumeMonitor *proxy,
          GAsyncResult *res,
          gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  DBusOp *data = g_task_get_task_data (task);
  GError *error = NULL;
 
  gvfs_remote_volume_monitor_call_volume_mount_finish (proxy, 
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
mount_foreign_callback (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data)
{
  GTask *task = G_TASK (user_data);
  GError *error = NULL;

  if (g_file_mount_enclosing_volume_finish (G_FILE (source_object), res, &error))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
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
mount_cancelled (GCancellable *cancellable,
                 gpointer      user_data)
{
  GTask *task = G_TASK (user_data);
  DBusOp *data = g_task_get_task_data (task);
  GProxyVolume *volume = G_PROXY_VOLUME (g_task_get_source_object (task));
  GVfsRemoteVolumeMonitor *proxy;

  G_LOCK (proxy_volume);

  /* Now tell the remote volume monitor that the op has been cancelled */
  proxy = g_proxy_volume_monitor_get_dbus_proxy (volume->volume_monitor);
  gvfs_remote_volume_monitor_call_cancel_operation (proxy,
                                                    data->cancellation_id,
                                                    NULL,
                                                    (GAsyncReadyCallback) cancel_operation_reply_cb,
                                                    NULL);
  g_object_unref (proxy);

  G_UNLOCK (proxy_volume);

  g_task_return_error_if_cancelled (task);
}

static void
g_proxy_volume_mount (GVolume             *volume,
                      GMountMountFlags     flags,
                      GMountOperation     *mount_operation,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  GTask *task;

  task = g_task_new (volume, cancellable, callback, user_data);
  g_task_set_source_tag (task, g_proxy_volume_mount);

  G_LOCK (proxy_volume);
  if (proxy_volume->activation_uri != NULL &&
      !proxy_volume->always_call_mount)
    {
      GFile *root;

      root = g_file_new_for_uri (proxy_volume->activation_uri);

      G_UNLOCK (proxy_volume);

      g_file_mount_enclosing_volume (root,
                                     flags,
                                     mount_operation,
                                     cancellable,
                                     mount_foreign_callback,
                                     task);

      g_object_unref (root);
    }
  else
    {
      DBusOp *data;
      GVfsRemoteVolumeMonitor *proxy;

      if (g_cancellable_is_cancelled (cancellable))
        {
          G_UNLOCK (proxy_volume);
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
                                                         G_CALLBACK (mount_cancelled),
                                                         task);
        }
      else
        {
          data->cancellation_id = g_strdup ("");
        }

      data->mount_op_id = g_proxy_mount_operation_wrap (mount_operation, proxy_volume->volume_monitor);

      g_task_set_task_data (task, data, (GDestroyNotify)dbus_op_free);

      proxy = g_proxy_volume_monitor_get_dbus_proxy (proxy_volume->volume_monitor);
      g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), G_PROXY_VOLUME_MONITOR_DBUS_TIMEOUT);  /* 30 minute timeout */

      gvfs_remote_volume_monitor_call_volume_mount (proxy,
                                                    proxy_volume->id,
                                                    data->cancellation_id,
                                                    flags,
                                                    data->mount_op_id,
                                                    NULL,
                                                    (GAsyncReadyCallback) mount_cb,
                                                    task);

      g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), -1);
      g_object_unref (proxy);

      G_UNLOCK (proxy_volume);
    }
}

static gboolean
g_proxy_volume_mount_finish (GVolume        *volume,
                             GAsyncResult  *result,
                             GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, volume), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_proxy_volume_mount), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static GFile *
g_proxy_volume_get_activation_root (GVolume *volume)
{
  GProxyVolume *proxy_volume = G_PROXY_VOLUME (volume);
  if (proxy_volume->activation_uri == NULL)
    return NULL;
  else
    return g_file_new_for_uri (proxy_volume->activation_uri);
}

static const gchar *
g_proxy_volume_get_sort_key (GVolume *_volume)
{
  GProxyVolume *volume = G_PROXY_VOLUME (_volume);
  return volume->sort_key;
}

static void
g_proxy_volume_volume_iface_init (GVolumeIface *iface)
{
  iface->get_name = g_proxy_volume_get_name;
  iface->get_icon = g_proxy_volume_get_icon;
  iface->get_symbolic_icon = g_proxy_volume_get_symbolic_icon;
  iface->get_uuid = g_proxy_volume_get_uuid;
  iface->get_drive = g_proxy_volume_get_drive;
  iface->get_mount = g_proxy_volume_get_mount;
  iface->can_mount = g_proxy_volume_can_mount;
  iface->can_eject = g_proxy_volume_can_eject;
  iface->should_automount = g_proxy_volume_should_automount;
  iface->mount_fn = g_proxy_volume_mount;
  iface->mount_finish = g_proxy_volume_mount_finish;
  iface->eject = g_proxy_volume_eject;
  iface->eject_finish = g_proxy_volume_eject_finish;
  iface->eject_with_operation = g_proxy_volume_eject_with_operation;
  iface->eject_with_operation_finish = g_proxy_volume_eject_with_operation_finish;
  iface->get_identifier = g_proxy_volume_get_identifier;
  iface->enumerate_identifiers = g_proxy_volume_enumerate_identifiers;
  iface->get_activation_root = g_proxy_volume_get_activation_root;
  iface->get_sort_key = g_proxy_volume_get_sort_key;
}

void
g_proxy_volume_register (GIOModule *module)
{
  g_proxy_volume_register_type (G_TYPE_MODULE (module));
}

typedef struct {
  const char *signal_name;
  GObject *object;
  GObject *other_object;
} SignalEmitIdleData;

static gboolean
signal_emit_in_idle_do (SignalEmitIdleData *data)
{
  if (data->other_object != NULL)
    {
      g_signal_emit_by_name (data->object, data->signal_name, data->other_object);
      g_object_unref (data->other_object);
    }
  else
    {
      g_signal_emit_by_name (data->object, data->signal_name);
    }
  g_object_unref (data->object);
  g_free (data);

  return FALSE;
}

static void
signal_emit_in_idle (gpointer object, const char *signal_name, gpointer other_object)
{
  SignalEmitIdleData *data;

  data = g_new0 (SignalEmitIdleData, 1);
  data->signal_name = signal_name;
  data->object = g_object_ref (G_OBJECT (object));
  data->other_object = other_object != NULL ? g_object_ref (G_OBJECT (other_object)) : NULL;
  g_idle_add ((GSourceFunc) signal_emit_in_idle_do, data);
}
