/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
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
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "ggphoto2volume.h"

#include "gvfsgphoto2utils.h"

/* Protects all fields of GHalDrive that can change */
G_LOCK_DEFINE_STATIC(gphoto2_volume);

struct _GGPhoto2Volume {
  GObject parent;

  GVolumeMonitor *volume_monitor; /* owned by volume monitor */

  char *device_path;
  GUdevDevice *device;

  GFile *activation_root;

  char *name;
  char *icon;
  char *symbolic_icon;
};

static void g_gphoto2_volume_volume_iface_init (GVolumeIface *iface);

G_DEFINE_TYPE_EXTENDED (GGPhoto2Volume, g_gphoto2_volume, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME,
                                               g_gphoto2_volume_volume_iface_init))

static void
g_gphoto2_volume_finalize (GObject *object)
{
  GGPhoto2Volume *volume;

  volume = G_GPHOTO2_VOLUME (object);

  if (volume->device != NULL)
    g_object_unref (volume->device);

  if (volume->activation_root != NULL)
    g_object_unref (volume->activation_root);

  if (volume->volume_monitor != NULL)
    g_object_remove_weak_pointer (G_OBJECT (volume->volume_monitor), (gpointer) &(volume->volume_monitor));

  g_free (volume->name);
  g_free (volume->icon);
  g_free (volume->symbolic_icon);

  if (G_OBJECT_CLASS (g_gphoto2_volume_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_gphoto2_volume_parent_class)->finalize) (object);
}

static void
g_gphoto2_volume_class_init (GGPhoto2VolumeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_gphoto2_volume_finalize;
}

static void
g_gphoto2_volume_init (GGPhoto2Volume *gphoto2_volume)
{
}

GGPhoto2Volume *
g_gphoto2_volume_new (GVolumeMonitor   *volume_monitor,
                      GUdevDevice      *device,
                      GUdevClient      *gudev_client,
                      GFile            *activation_root)
{
  GGPhoto2Volume *volume;
  const char *device_path;

  g_return_val_if_fail (volume_monitor != NULL, NULL);
  g_return_val_if_fail (device != NULL, NULL);
  g_return_val_if_fail (gudev_client != NULL, NULL);
  g_return_val_if_fail (activation_root != NULL, NULL);

  if (!g_udev_device_has_property (device, "ID_GPHOTO2"))
      return NULL;
  device_path = g_udev_device_get_device_file (device);

  volume = g_object_new (G_TYPE_GPHOTO2_VOLUME, NULL);
  volume->volume_monitor = volume_monitor;
  g_object_add_weak_pointer (G_OBJECT (volume_monitor), (gpointer) &(volume->volume_monitor));
  volume->device_path = g_strdup (device_path);
  volume->device = g_object_ref (device);
  volume->activation_root = g_object_ref (activation_root);

  volume->name = g_vfs_get_volume_name (device, "ID_GPHOTO2");
  volume->icon = g_vfs_get_volume_icon (device);
  volume->symbolic_icon = g_vfs_get_volume_symbolic_icon (device);
  /* we do not really need to listen for changes */

  return volume;
}

void
g_gphoto2_volume_removed (GGPhoto2Volume *volume)
{
  ;
}

static GIcon *
g_gphoto2_volume_get_icon (GVolume *volume)
{
  GGPhoto2Volume *gphoto2_volume = G_GPHOTO2_VOLUME (volume);
  GIcon *icon;

  G_LOCK (gphoto2_volume);
  icon = g_themed_icon_new (gphoto2_volume->icon);
  G_UNLOCK (gphoto2_volume);
  return icon;
}

static GIcon *
g_gphoto2_volume_get_symbolic_icon (GVolume *volume)
{
  GGPhoto2Volume *gphoto2_volume = G_GPHOTO2_VOLUME (volume);
  GIcon *icon;

  G_LOCK (gphoto2_volume);
  icon = g_themed_icon_new_with_default_fallbacks (gphoto2_volume->symbolic_icon);
  G_UNLOCK (gphoto2_volume);
  return icon;
}

static char *
g_gphoto2_volume_get_name (GVolume *volume)
{
  GGPhoto2Volume *gphoto2_volume = G_GPHOTO2_VOLUME (volume);
  char *name;

  G_LOCK (gphoto2_volume);
  name = g_strdup (gphoto2_volume->name);
  G_UNLOCK (gphoto2_volume);

  return name;
}

static char *
g_gphoto2_volume_get_uuid (GVolume *volume)
{
  return NULL;
}

static gboolean
g_gphoto2_volume_can_mount (GVolume *volume)
{
  return TRUE;
}

static gboolean
g_gphoto2_volume_can_eject (GVolume *volume)
{
  return FALSE;
}

static gboolean
g_gphoto2_volume_should_automount (GVolume *volume)
{
  return TRUE;
}

static GDrive *
g_gphoto2_volume_get_drive (GVolume *volume)
{
  return NULL;
}

static GMount *
g_gphoto2_volume_get_mount (GVolume *volume)
{
  return NULL;
}

gboolean
g_gphoto2_volume_has_path (GGPhoto2Volume  *volume,
                      const char  *sysfs_path)
{
  GGPhoto2Volume *gphoto2_volume = G_GPHOTO2_VOLUME (volume);
  gboolean res;

  G_LOCK (gphoto2_volume);
  res = FALSE;
  if (gphoto2_volume->device != NULL)
    res = g_strcmp0 (g_udev_device_get_sysfs_path (gphoto2_volume->device), sysfs_path) == 0;
  G_UNLOCK (gphoto2_volume);
  return res;
}

typedef struct
{
  GGPhoto2Volume *enclosing_volume;
  GAsyncReadyCallback  callback;
  gpointer user_data;
} ActivationMountOp;

static void
mount_callback (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data)
{
  ActivationMountOp *data = user_data;
  data->callback (G_OBJECT (data->enclosing_volume), res, data->user_data);
  g_free (data);
}

static void
g_gphoto2_volume_mount (GVolume             *volume,
                        GMountMountFlags     flags,
                        GMountOperation     *mount_operation,
                        GCancellable        *cancellable,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
  GGPhoto2Volume *gphoto2_volume = G_GPHOTO2_VOLUME (volume);
  ActivationMountOp *data;

  /*g_warning ("gphoto2_volume_mount (can_mount=%d foreign=%p device_path=%s)",
              g_gphoto2_volume_can_mount (volume),
              gphoto2_volume->activation_root,
              gphoto2_volume->device_path);*/

  G_LOCK (gphoto2_volume);

  data = g_new0 (ActivationMountOp, 1);
  data->enclosing_volume = gphoto2_volume;
  data->callback = callback;
  data->user_data = user_data;

  g_file_mount_enclosing_volume (gphoto2_volume->activation_root,
                                 0,
                                 mount_operation,
                                 cancellable,
                                 mount_callback,
                                 data);

  G_UNLOCK (gphoto2_volume);
}

static gboolean
g_gphoto2_volume_mount_finish (GVolume       *volume,
                           GAsyncResult  *result,
                           GError       **error)
{
  GGPhoto2Volume *gphoto2_volume = G_GPHOTO2_VOLUME (volume);
  gboolean res;

  G_LOCK (gphoto2_volume);
  res = g_file_mount_enclosing_volume_finish (gphoto2_volume->activation_root, result, error);
  G_UNLOCK (gphoto2_volume);

  return res;
}

static char *
g_gphoto2_volume_get_identifier (GVolume              *volume,
                             const char          *kind)
{
  GGPhoto2Volume *gphoto2_volume = G_GPHOTO2_VOLUME (volume);
  char *id;

  G_LOCK (gphoto2_volume);
  id = NULL;
  if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE) == 0)
    id = g_strdup (gphoto2_volume->device_path);
  G_UNLOCK (gphoto2_volume);

  return id;
}

static char **
g_gphoto2_volume_enumerate_identifiers (GVolume *volume)
{
  GGPhoto2Volume *gphoto2_volume = G_GPHOTO2_VOLUME (volume);
  GPtrArray *res;

  G_LOCK (gphoto2_volume);

  res = g_ptr_array_new ();

  if (gphoto2_volume->device_path && *gphoto2_volume->device_path != 0)
    g_ptr_array_add (res,
                     g_strdup (G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));

  /* Null-terminate */
  g_ptr_array_add (res, NULL);

  G_UNLOCK (gphoto2_volume);

  return (char **)g_ptr_array_free (res, FALSE);
}

static GFile *
g_gphoto2_volume_get_activation_root (GVolume *volume)
{
  GGPhoto2Volume *gphoto2_volume = G_GPHOTO2_VOLUME (volume);

  return g_object_ref (gphoto2_volume->activation_root);
}

static void
g_gphoto2_volume_volume_iface_init (GVolumeIface *iface)
{
  iface->get_name = g_gphoto2_volume_get_name;
  iface->get_icon = g_gphoto2_volume_get_icon;
  iface->get_symbolic_icon = g_gphoto2_volume_get_symbolic_icon;
  iface->get_uuid = g_gphoto2_volume_get_uuid;
  iface->get_drive = g_gphoto2_volume_get_drive;
  iface->get_mount = g_gphoto2_volume_get_mount;
  iface->can_mount = g_gphoto2_volume_can_mount;
  iface->can_eject = g_gphoto2_volume_can_eject;
  iface->should_automount = g_gphoto2_volume_should_automount;
  iface->mount_fn = g_gphoto2_volume_mount;
  iface->mount_finish = g_gphoto2_volume_mount_finish;
  iface->eject = NULL;
  iface->eject_finish = NULL;
  iface->get_identifier = g_gphoto2_volume_get_identifier;
  iface->enumerate_identifiers = g_gphoto2_volume_enumerate_identifiers;
  iface->get_activation_root = g_gphoto2_volume_get_activation_root;
}
