/* GIO - GLib Input, Output and Streaming Library
 *   Volume Monitor for MTP Backend
 *
 * Copyright (C) 2012 Philip Langdale <philipl@overt.org>
 * - Based on ggphoto2volume.c
 *   - Copyright (C) 2006-2007 Red Hat, Inc.
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
 * Public License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "gmtpvolume.h"
#include "gvfsgphoto2utils.h"

G_LOCK_DEFINE_STATIC (mtp_volume);

struct _GMtpVolume {
  GObject parent;

  GVolumeMonitor *volume_monitor; /* owned by volume monitor */

  char *device_path;
  GUdevDevice *device;

  GFile *activation_root;

  char *name;
  char *icon;
  char *symbolic_icon;
};

static void g_mtp_volume_volume_iface_init (GVolumeIface *iface);

G_DEFINE_TYPE_EXTENDED (GMtpVolume, g_mtp_volume, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME,
                                               g_mtp_volume_volume_iface_init))

static void
g_mtp_volume_finalize (GObject *object)
{
  GMtpVolume *volume;

  volume = G_MTP_VOLUME (object);

  g_clear_object (&volume->device);
  g_clear_object (&volume->activation_root);

  if (volume->volume_monitor != NULL)
    g_object_remove_weak_pointer (G_OBJECT (volume->volume_monitor), (gpointer) &(volume->volume_monitor));

  g_free (volume->name);
  g_free (volume->icon);

  (*G_OBJECT_CLASS (g_mtp_volume_parent_class)->finalize) (object);
}

static void
g_mtp_volume_class_init (GMtpVolumeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_mtp_volume_finalize;
}

static void
g_mtp_volume_init (GMtpVolume *mtp_volume)
{
}

GMtpVolume *
g_mtp_volume_new (GVolumeMonitor   *volume_monitor,
                  GUdevDevice      *device,
                  GUdevClient      *gudev_client,
                  GFile            *activation_root)
{
  GMtpVolume *volume;
  const char *device_path;

  g_return_val_if_fail (volume_monitor != NULL, NULL);
  g_return_val_if_fail (device != NULL, NULL);
  g_return_val_if_fail (gudev_client != NULL, NULL);
  g_return_val_if_fail (activation_root != NULL, NULL);

  if (!g_udev_device_has_property (device, "ID_MTP_DEVICE"))
    return NULL;
  device_path = g_udev_device_get_device_file (device);

  volume = g_object_new (G_TYPE_MTP_VOLUME, NULL);
  volume->volume_monitor = volume_monitor;
  g_object_add_weak_pointer (G_OBJECT (volume_monitor), (gpointer) &(volume->volume_monitor));
  volume->device_path = g_strdup (device_path);
  volume->device = g_object_ref (device);
  volume->activation_root = g_object_ref (activation_root);

  volume->name = g_vfs_get_volume_name (device, "ID_MTP");
  volume->icon = g_vfs_get_volume_icon (device);
  volume->symbolic_icon = g_vfs_get_volume_symbolic_icon (device);
  /* we do not really need to listen for changes */

  return volume;
}

void
g_mtp_volume_removed (GMtpVolume *volume)
{
}

static GIcon *
g_mtp_volume_get_icon (GVolume *volume)
{
  GMtpVolume *mtp_volume = G_MTP_VOLUME (volume);
  GIcon *icon;

  G_LOCK (mtp_volume);
  icon = g_themed_icon_new (mtp_volume->icon);
  G_UNLOCK (mtp_volume);
  return icon;
}

static GIcon *
g_mtp_volume_get_symbolic_icon (GVolume *volume)
{
  GMtpVolume *mtp_volume = G_MTP_VOLUME (volume);
  GIcon *icon;

  G_LOCK (mtp_volume);
  icon = g_themed_icon_new_with_default_fallbacks (mtp_volume->symbolic_icon);
  G_UNLOCK (mtp_volume);
  return icon;
}

static char *
g_mtp_volume_get_name (GVolume *volume)
{
  GMtpVolume *mtp_volume = G_MTP_VOLUME (volume);
  char *name;

  G_LOCK (mtp_volume);
  name = g_strdup (mtp_volume->name);
  G_UNLOCK (mtp_volume);

  return name;
}

static char *
g_mtp_volume_get_uuid (GVolume *volume)
{
  return NULL;
}

static gboolean
g_mtp_volume_can_mount (GVolume *volume)
{
  return TRUE;
}

static gboolean
g_mtp_volume_can_eject (GVolume *volume)
{
  return FALSE;
}

static gboolean
g_mtp_volume_should_automount (GVolume *volume)
{
  return TRUE;
}

static GDrive *
g_mtp_volume_get_drive (GVolume *volume)
{
  return NULL;
}

static GMount *
g_mtp_volume_get_mount (GVolume *volume)
{
  return NULL;
}

gboolean
g_mtp_volume_has_path (GMtpVolume  *volume,
                      const char  *sysfs_path)
{
  GMtpVolume *mtp_volume = G_MTP_VOLUME (volume);
  gboolean res;

  G_LOCK (mtp_volume);
  res = FALSE;
  if (mtp_volume->device != NULL)
    res = g_strcmp0 (g_udev_device_get_sysfs_path (mtp_volume->device), sysfs_path) == 0;
  G_UNLOCK (mtp_volume);
  return res;
}

typedef struct
{
  GMtpVolume *enclosing_volume;
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
g_mtp_volume_mount (GVolume             *volume,
                        GMountMountFlags     flags,
                        GMountOperation     *mount_operation,
                        GCancellable        *cancellable,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
  GMtpVolume *mtp_volume = G_MTP_VOLUME (volume);
  ActivationMountOp *data;

  /*g_warning ("mtp_volume_mount (can_mount=%d foreign=%p device_path=%s)",
              g_mtp_volume_can_mount (volume),
              mtp_volume->activation_root,
              mtp_volume->device_path);*/

  G_LOCK (mtp_volume);

  data = g_new0 (ActivationMountOp, 1);
  data->enclosing_volume = mtp_volume;
  data->callback = callback;
  data->user_data = user_data;

  g_file_mount_enclosing_volume (mtp_volume->activation_root,
                                 0,
                                 mount_operation,
                                 cancellable,
                                 mount_callback,
                                 data);

  G_UNLOCK (mtp_volume);
}

static gboolean
g_mtp_volume_mount_finish (GVolume       *volume,
                           GAsyncResult  *result,
                           GError       **error)
{
  GMtpVolume *mtp_volume = G_MTP_VOLUME (volume);
  gboolean res;

  G_LOCK (mtp_volume);
  res = g_file_mount_enclosing_volume_finish (mtp_volume->activation_root, result, error);
  G_UNLOCK (mtp_volume);

  return res;
}

static char *
g_mtp_volume_get_identifier (GVolume    *volume,
                             const char *kind)
{
  GMtpVolume *mtp_volume = G_MTP_VOLUME (volume);
  char *id;

  G_LOCK (mtp_volume);
  id = NULL;
  if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE) == 0)
    id = g_strdup (mtp_volume->device_path);
  G_UNLOCK (mtp_volume);

  return id;
}

static char **
g_mtp_volume_enumerate_identifiers (GVolume *volume)
{
  GMtpVolume *mtp_volume = G_MTP_VOLUME (volume);
  GPtrArray *res;

  G_LOCK (mtp_volume);

  res = g_ptr_array_new ();

  if (mtp_volume->device_path && *mtp_volume->device_path != 0)
    g_ptr_array_add (res,
                     g_strdup (G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));

  /* Null-terminate */
  g_ptr_array_add (res, NULL);

  G_UNLOCK (mtp_volume);

  return (char **)g_ptr_array_free (res, FALSE);
}

static GFile *
g_mtp_volume_get_activation_root (GVolume *volume)
{
  GMtpVolume *mtp_volume = G_MTP_VOLUME (volume);

  return g_object_ref (mtp_volume->activation_root);
}

static void
g_mtp_volume_volume_iface_init (GVolumeIface *iface)
{
  iface->get_name = g_mtp_volume_get_name;
  iface->get_icon = g_mtp_volume_get_icon;
  iface->get_symbolic_icon = g_mtp_volume_get_symbolic_icon;
  iface->get_uuid = g_mtp_volume_get_uuid;
  iface->get_drive = g_mtp_volume_get_drive;
  iface->get_mount = g_mtp_volume_get_mount;
  iface->can_mount = g_mtp_volume_can_mount;
  iface->can_eject = g_mtp_volume_can_eject;
  iface->should_automount = g_mtp_volume_should_automount;
  iface->mount_fn = g_mtp_volume_mount;
  iface->mount_finish = g_mtp_volume_mount_finish;
  iface->eject = NULL;
  iface->eject_finish = NULL;
  iface->get_identifier = g_mtp_volume_get_identifier;
  iface->enumerate_identifiers = g_mtp_volume_enumerate_identifiers;
  iface->get_activation_root = g_mtp_volume_get_activation_root;
}
