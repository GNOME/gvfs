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
#include "gproxyshadowmount.h"
#include "gproxyvolume.h"

static void signal_emit_in_idle (gpointer object, const char *signal_name, gpointer other_object);

/* Protects all fields of GProxyShadowMount that can change */
G_LOCK_DEFINE_STATIC(proxy_shadow_mount);

struct _GProxyShadowMount {
  GObject parent;

  GProxyVolumeMonitor *volume_monitor;

  GProxyVolume *volume;
  GMount *real_mount;
  gulong pre_unmount_signal_id;
  gboolean real_mount_shadowed;
  GFile *root;
};

static void g_proxy_shadow_mount_mount_iface_init (GMountIface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (GProxyShadowMount, g_proxy_shadow_mount, G_TYPE_OBJECT, 0,
                                G_IMPLEMENT_INTERFACE_DYNAMIC (G_TYPE_MOUNT,
                                                               g_proxy_shadow_mount_mount_iface_init))

static void
g_proxy_shadow_mount_finalize (GObject *object)
{
  GProxyShadowMount *mount;

  mount = G_PROXY_SHADOW_MOUNT (object);

  g_proxy_shadow_mount_remove (mount);

  if (mount->real_mount != NULL)
    {
      g_object_unref (mount->real_mount);
      mount->real_mount = NULL;
    }
  
  if (mount->volume_monitor != NULL)
    g_object_unref (mount->volume_monitor);

  if (mount->volume != NULL)
    g_object_unref (mount->volume);

  if (mount->root != NULL)
    g_object_unref (mount->root);

  if (G_OBJECT_CLASS (g_proxy_shadow_mount_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_proxy_shadow_mount_parent_class)->finalize) (object);
}

static void
g_proxy_shadow_mount_class_init (GProxyShadowMountClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_proxy_shadow_mount_finalize;
}

static void
g_proxy_shadow_mount_class_finalize (GProxyShadowMountClass *klass)
{
}

static void
g_proxy_shadow_mount_init (GProxyShadowMount *proxy_shadow_mount)
{
}

static void
real_mount_pre_unmount_cb (GMount            *real_mount,
                           GProxyShadowMount *shadow_mount)
{
  g_signal_emit_by_name (shadow_mount, "pre-unmount", 0);
  g_signal_emit_by_name (shadow_mount->volume_monitor, "mount-pre-unmount", shadow_mount);
}

void
g_proxy_shadow_mount_remove (GProxyShadowMount *mount)
{
  if (mount->real_mount_shadowed)
    {
      g_mount_unshadow (mount->real_mount);
      signal_emit_in_idle (mount->real_mount, "changed", NULL);
      signal_emit_in_idle (mount->volume_monitor, "mount-changed", mount->real_mount);
      mount->real_mount_shadowed = FALSE;

      if (mount->pre_unmount_signal_id != 0)
        {
          g_signal_handler_disconnect (mount->real_mount,
                                       mount->pre_unmount_signal_id);
          mount->pre_unmount_signal_id = 0;
        }
    }
}

GProxyShadowMount *
g_proxy_shadow_mount_new (GProxyVolumeMonitor *volume_monitor,
                          GProxyVolume        *volume,
                          GMount              *real_mount)
{
  GProxyShadowMount *mount;
  GFile *activation_root;

  mount = NULL;

  activation_root = g_volume_get_activation_root (G_VOLUME (volume));
  if (activation_root == NULL)
    {
      g_warning ("Cannot construct a GProxyShadowMount object for a volume without an activation root");
      goto out;
    }

  mount = g_object_new (G_TYPE_PROXY_SHADOW_MOUNT, NULL);
  mount->volume_monitor = g_object_ref (volume_monitor);
  mount->volume = g_object_ref (volume);
  mount->real_mount = g_object_ref (real_mount);
  mount->real_mount_shadowed = TRUE;
  mount->root = activation_root;

  g_mount_shadow (mount->real_mount);
  signal_emit_in_idle (mount->real_mount, "changed", NULL);
  signal_emit_in_idle (mount->volume_monitor, "mount-changed", mount->real_mount);

  mount->pre_unmount_signal_id = g_signal_connect (mount->real_mount, "pre-unmount",
                                                   G_CALLBACK (real_mount_pre_unmount_cb), mount);

  g_object_set_data (G_OBJECT (mount),
                     "g-proxy-shadow-mount-volume-monitor-name",
                     (gpointer) g_type_name (G_TYPE_FROM_INSTANCE (volume_monitor)));

 out:
  return mount;
}

gboolean
g_proxy_shadow_mount_has_mount_path (GProxyShadowMount *mount, const char *mount_path)
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

static GFile *
g_proxy_shadow_mount_get_root (GMount *mount)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);
  GFile *root;

  G_LOCK (proxy_shadow_mount);
  root = g_object_ref (proxy_shadow_mount->root);
  G_UNLOCK (proxy_shadow_mount);
  return root;
}

static GIcon *
g_proxy_shadow_mount_get_icon (GMount *mount)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);
  GIcon *icon;

  G_LOCK (proxy_shadow_mount);
  icon = g_volume_get_icon (G_VOLUME (proxy_shadow_mount->volume));
  G_UNLOCK (proxy_shadow_mount);
  return icon;
}

static char *
g_proxy_shadow_mount_get_uuid (GMount *mount)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);
  char *uuid;

  G_LOCK (proxy_shadow_mount);
  uuid = g_mount_get_uuid (G_MOUNT (proxy_shadow_mount->real_mount));
  G_UNLOCK (proxy_shadow_mount);
  return uuid;
}

static char *
g_proxy_shadow_mount_get_name (GMount *mount)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);
  char *name;

  G_LOCK (proxy_shadow_mount);
  name = g_volume_get_name (G_VOLUME (proxy_shadow_mount->volume));
  G_UNLOCK (proxy_shadow_mount);

  return name;
}

static GDrive *
g_proxy_shadow_mount_get_drive (GMount *mount)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);
  GDrive *drive;

  G_LOCK (proxy_shadow_mount);
  drive = g_mount_get_drive (G_MOUNT (proxy_shadow_mount->real_mount));
  G_UNLOCK (proxy_shadow_mount);

  return drive;
}

static GVolume *
g_proxy_shadow_mount_get_volume (GMount *mount)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);
  GVolume *volume;

  G_LOCK (proxy_shadow_mount);
  volume = g_object_ref (proxy_shadow_mount->volume);
  G_UNLOCK (proxy_shadow_mount);

  return volume;
}

static gboolean
g_proxy_shadow_mount_can_unmount (GMount *mount)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);
  gboolean res;

  G_LOCK (proxy_shadow_mount);
  res = g_mount_can_unmount (G_MOUNT (proxy_shadow_mount->real_mount));
  G_UNLOCK (proxy_shadow_mount);

  return res;
}

static gboolean
g_proxy_shadow_mount_can_eject (GMount *mount)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);
  gboolean res;

  G_LOCK (proxy_shadow_mount);
  res = g_volume_can_eject (G_VOLUME (proxy_shadow_mount->volume));
  G_UNLOCK (proxy_shadow_mount);

  return res;
}


typedef struct {
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
} AsyncWrapperOp;

static void
async_wrapper_callback (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data)
{
  AsyncWrapperOp *data  = user_data;
  data->callback (data->object, res, data->user_data);
  g_object_unref (data->object);
  g_free (data);
}

static AsyncWrapperOp *
setup_async_wrapper (GMount *mount,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  AsyncWrapperOp *data;
  
  data = g_new0 (AsyncWrapperOp, 1);
  data->object = g_object_ref (mount);
  data->callback = callback;
  data->user_data = user_data;
  return data;
}
                     

static void
g_proxy_shadow_mount_eject_with_operation (GMount              *mount,
                                           GMountUnmountFlags   flags,
                                           GMountOperation     *mount_operation,
                                           GCancellable        *cancellable,
                                           GAsyncReadyCallback  callback,
                                           gpointer             user_data)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);
  AsyncWrapperOp *data;

  data = setup_async_wrapper (mount, callback, user_data);
  G_LOCK (proxy_shadow_mount);
  g_volume_eject_with_operation (G_VOLUME (proxy_shadow_mount->volume),
                                 flags, mount_operation, cancellable,
                                 async_wrapper_callback, data);
  G_UNLOCK (proxy_shadow_mount);
}

static gboolean
g_proxy_shadow_mount_eject_with_operation_finish (GMount        *mount,
                                                  GAsyncResult  *result,
                                                  GError       **error)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);
  gboolean res;

  G_LOCK (proxy_shadow_mount);
  res = g_volume_eject_with_operation_finish (G_VOLUME (proxy_shadow_mount->volume), result, error);
  G_UNLOCK (proxy_shadow_mount);

  return res;
}

static void
g_proxy_shadow_mount_unmount_with_operation (GMount              *mount,
                                             GMountUnmountFlags   flags,
                                             GMountOperation     *mount_operation,
                                             GCancellable        *cancellable,
                                             GAsyncReadyCallback  callback,
                                             gpointer             user_data)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);
  AsyncWrapperOp *data;

  data = setup_async_wrapper (mount, callback, user_data);
  g_mount_unmount_with_operation (proxy_shadow_mount->real_mount,
                                  flags,
                                  mount_operation,
                                  cancellable,
                                  async_wrapper_callback, data);
}

static gboolean
g_proxy_shadow_mount_unmount_with_operation_finish (GMount        *mount,
                                                    GAsyncResult  *result,
                                                    GError       **error)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);

  return g_mount_unmount_with_operation_finish (proxy_shadow_mount->real_mount,
                                                result,
                                                error);
}

static void
g_proxy_shadow_mount_unmount (GMount              *mount,
                              GMountUnmountFlags   flags,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  g_proxy_shadow_mount_unmount_with_operation (mount, flags, NULL, cancellable, callback, user_data);
}

static gboolean
g_proxy_shadow_mount_unmount_finish (GMount        *mount,
                                     GAsyncResult  *result,
                                     GError       **error)
{
  return g_proxy_shadow_mount_unmount_with_operation_finish (mount, result, error);
}

static void
g_proxy_shadow_mount_eject (GMount              *mount,
                            GMountUnmountFlags   flags,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  g_proxy_shadow_mount_eject_with_operation (mount, flags, NULL, cancellable, callback, user_data);
}

static gboolean
g_proxy_shadow_mount_eject_finish (GMount        *mount,
                                   GAsyncResult  *result,
                                   GError       **error)
{
  return g_proxy_shadow_mount_eject_with_operation_finish (mount, result, error);
}

static void
g_proxy_shadow_mount_guess_content_type (GMount              *mount,
                                         gboolean             force_rescan,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);
  AsyncWrapperOp *data;

  data = setup_async_wrapper (mount, callback, user_data);
  g_mount_guess_content_type (proxy_shadow_mount->real_mount,
                              force_rescan,
                              cancellable,
                              async_wrapper_callback, data);
}

static char **
g_proxy_shadow_mount_guess_content_type_finish (GMount              *mount,
                                                GAsyncResult        *result,
                                                GError             **error)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);

  return g_mount_guess_content_type_finish (proxy_shadow_mount->real_mount,
                                            result,
                                            error);
}

static char **
g_proxy_shadow_mount_guess_content_type_sync (GMount              *mount,
                                              gboolean             force_rescan,
                                              GCancellable        *cancellable,
                                              GError             **error)
{
  GProxyShadowMount *proxy_shadow_mount = G_PROXY_SHADOW_MOUNT (mount);
  return g_mount_guess_content_type_sync (proxy_shadow_mount->real_mount,
                                          force_rescan,
                                          cancellable,
                                          error);
}

GMount *
g_proxy_shadow_mount_get_real_mount (GProxyShadowMount *mount)
{
  return g_object_ref (mount->real_mount);
}

GFile *
g_proxy_shadow_mount_get_activation_root (GProxyShadowMount *mount)
{
  return g_object_ref (mount->root);
}

static void
g_proxy_shadow_mount_mount_iface_init (GMountIface *iface)
{
  iface->get_root = g_proxy_shadow_mount_get_root;
  iface->get_name = g_proxy_shadow_mount_get_name;
  iface->get_icon = g_proxy_shadow_mount_get_icon;
  iface->get_uuid = g_proxy_shadow_mount_get_uuid;
  iface->get_drive = g_proxy_shadow_mount_get_drive;
  iface->get_volume = g_proxy_shadow_mount_get_volume;
  iface->can_unmount = g_proxy_shadow_mount_can_unmount;
  iface->can_eject = g_proxy_shadow_mount_can_eject;
  iface->unmount = g_proxy_shadow_mount_unmount;
  iface->unmount_finish = g_proxy_shadow_mount_unmount_finish;
  iface->unmount_with_operation = g_proxy_shadow_mount_unmount_with_operation;
  iface->unmount_with_operation_finish = g_proxy_shadow_mount_unmount_with_operation_finish;
  iface->eject = g_proxy_shadow_mount_eject;
  iface->eject_finish = g_proxy_shadow_mount_eject_finish;
  iface->eject_with_operation = g_proxy_shadow_mount_eject_with_operation;
  iface->eject_with_operation_finish = g_proxy_shadow_mount_eject_with_operation_finish;
  iface->guess_content_type = g_proxy_shadow_mount_guess_content_type;
  iface->guess_content_type_finish = g_proxy_shadow_mount_guess_content_type_finish;
  iface->guess_content_type_sync = g_proxy_shadow_mount_guess_content_type_sync;
}

void
g_proxy_shadow_mount_register (GIOModule *module)
{
  g_proxy_shadow_mount_register_type (G_TYPE_MODULE (module));
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
