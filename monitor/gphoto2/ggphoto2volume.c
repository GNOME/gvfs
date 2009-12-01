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

#include "ggphoto2volume.h"

#ifndef HAVE_GUDEV
#include "hal-utils.h"
#endif

/* Protects all fields of GHalDrive that can change */
G_LOCK_DEFINE_STATIC(gphoto2_volume);

struct _GGPhoto2Volume {
  GObject parent;

  GVolumeMonitor *volume_monitor; /* owned by volume monitor */

  char *device_path;
#ifdef HAVE_GUDEV
  GUdevDevice *device;
#else
  HalDevice *device;
  HalDevice *drive_device;
#endif

  GFile *activation_root;

  char *name;
  char *icon;
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

#ifdef HAVE_GUDEV
static int hexdigit(char c)
{
  if (c >= 'a')
      return c - 'a' + 10;
  if (c >= 'A')
     return c - 'A' + 10;
  g_return_val_if_fail (c >= '0' && c <= '9', 0);
  return c - '0';
}

/* Do not free result, it's a static buffer */
static const char*
udev_decode_string (const char* encoded)
{
  static char decoded[4096];
  int len;
  const char* s;

  if (encoded == NULL)
      return NULL;

  for (len = 0, s = encoded; *s && len < sizeof(decoded)-1; ++len, ++s)
    {
      /* need to check for NUL terminator in advance */
      if (s[0] == '\\' && s[1] == 'x' && s[2] >= '0' && s[3] >= '0')
	{
	  decoded[len] = (hexdigit(s[2]) << 4) | hexdigit(s[3]);
	  s += 3;
	}
      else
	  decoded[len] = *s;
    }
  decoded[len] = '\0';
  return decoded;
}

static void
set_volume_name (GGPhoto2Volume *v)
{
  const char *gphoto_name;
  const char *product = NULL;
  const char *vendor;
  const char *model;

  /* our preference: ID_GPHOTO2 > ID_MEDIA_PLAYER_{VENDOR,PRODUCT} > product >
   * ID_{VENDOR,MODEL} */

  gphoto_name = g_udev_device_get_property (v->device, "ID_GPHOTO2");
  if (gphoto_name != NULL && strcmp (gphoto_name, "1") != 0)
    {
      v->name = g_strdup (gphoto_name);
      return;
    }

  vendor = g_udev_device_get_property (v->device, "ID_MEDIA_PLAYER_VENDOR");
  if (vendor == NULL)
      vendor = g_udev_device_get_property (v->device, "ID_VENDOR_ENC");
  model = g_udev_device_get_property (v->device, "ID_MEDIA_PLAYER_MODEL");
  if (model == NULL)
    {
      model = g_udev_device_get_property (v->device, "ID_MODEL_ENC");
      product = g_udev_device_get_sysfs_attr (v->device, "product");
    }

  v->name = NULL;
  if (product != NULL && strlen (product) > 0)
    v->name = g_strdup (product);
  else if (vendor == NULL)
    {
      if (model != NULL)
        v->name = g_strdup (udev_decode_string (model));
    }
  else
    {
      if (model != NULL)
	{
	  /* we can't call udev_decode_string() twice in one g_strdup_printf(),
	   * it returns a static buffer */
	  gchar *temp = g_strdup_printf ("%s %s", vendor, model);
          v->name = g_strdup (udev_decode_string (temp));
	  g_free (temp);
        }
      else
        {
          if (g_udev_device_has_property (v->device, "ID_MEDIA_PLAYER"))
            {
              /* Translators: %s is the device vendor */
              v->name = g_strdup_printf (_("%s Audio Player"), udev_decode_string (vendor));
	    }
	  else
	    {
	      /* Translators: %s is the device vendor */
	      v->name = g_strdup_printf (_("%s Camera"), udev_decode_string (vendor));
	    }
        }
    }

  if (v->name == NULL)
      v->name = g_strdup (_("Camera"));
}

static void
set_volume_icon (GGPhoto2Volume *volume)
{
  if (g_udev_device_has_property (volume->device, "ID_MEDIA_PLAYER_ICON_NAME"))
      volume->icon = g_strdup (g_udev_device_get_property (volume->device, "ID_MEDIA_PLAYER_ICON_NAME"));
  else if (g_udev_device_has_property (volume->device, "ID_MEDIA_PLAYER"))
      volume->icon = g_strdup ("multimedia-player");
  else
      volume->icon = g_strdup ("camera-photo");
}

#else
static gboolean
changed_in_idle (gpointer data)
{
  GGPhoto2Volume *volume = data;

  g_signal_emit_by_name (volume, "changed");
  if (volume->volume_monitor != NULL)
    g_signal_emit_by_name (volume->volume_monitor, "volume_changed", volume);
  g_object_unref (volume);

  return FALSE;
}

static char **
dupv_and_uniqify (char **str_array)
{
  int n, m, o;
  int len;
  char **result;

  result = g_strdupv (str_array);
  len = g_strv_length (result);

  for (n = 0; n < len; n++)
    {
      char *s = result[n];
      for (m = n + 1; m < len; m++)
        {
          char *p = result[m];
          if (strcmp (s, p) == 0)
            {
              for (o = m + 1; o < len; o++)
                result[o - 1] = result[o];
              len--;
              result[len] = NULL;
              m--;
            }
        }
    }

  return result;
}

static void
do_update_from_hal_for_camera (GGPhoto2Volume *v)
{
  const char *vendor;
  const char *product;
  const char *icon_from_hal;
  const char *name_from_hal;
  gboolean is_audio_player;

  vendor = hal_device_get_property_string (v->drive_device, "usb_device.vendor");
  product = hal_device_get_property_string (v->drive_device, "usb_device.product");
  icon_from_hal = hal_device_get_property_string (v->device, "info.desktop.icon");
  name_from_hal = hal_device_get_property_string (v->device, "info.desktop.name");

  is_audio_player = hal_device_has_capability (v->device, "portable_audio_player");

  v->name = NULL;
  if (strlen (name_from_hal) > 0)
    v->name = g_strdup (name_from_hal);
  else if (vendor == NULL)
    {
      if (product != NULL)
        v->name = g_strdup (product);
    }
  else
    {
      if (product != NULL)
        v->name = g_strdup_printf ("%s %s", vendor, product);
      else
        {
          if (is_audio_player)
            {
              /* Translators: %s is the device vendor */
              v->name = g_strdup_printf (_("%s Audio Player"), vendor);
            }
          else
            {
              /* Translators: %s is the device vendor */
              v->name = g_strdup_printf (_("%s Camera"), vendor);
            }
        }
    }
  if (v->name == NULL)
    {
      if (is_audio_player)
        v->name = g_strdup (_("Audio Player"));
      else
        v->name = g_strdup (_("Camera"));
    }

  if (strlen (icon_from_hal) > 0)
    v->icon = g_strdup (icon_from_hal);
  else if (is_audio_player)
    v->icon = g_strdup ("multimedia-player");
  else
    v->icon = g_strdup ("camera-photo");

  g_object_set_data_full (G_OBJECT (v),
                          "hal-storage-device-capabilities",
                          dupv_and_uniqify (hal_device_get_property_strlist (v->device, "info.capabilities")),
                          (GDestroyNotify) g_strfreev);
}

static void
update_from_hal (GGPhoto2Volume *mv, gboolean emit_changed)
{
  char *old_name;
  char *old_icon;

  G_LOCK (gphoto2_volume);

  old_name = g_strdup (mv->name);
  old_icon = g_strdup (mv->icon);

  g_free (mv->name);
  g_free (mv->icon);

  do_update_from_hal_for_camera (mv);

  if (emit_changed)
    {
      if (old_name == NULL ||
          old_icon == NULL ||
          strcmp (old_name, mv->name) != 0 ||
          strcmp (old_icon, mv->icon) != 0)
        g_idle_add (changed_in_idle, g_object_ref (mv));
    }
  g_free (old_name);
  g_free (old_icon);

  G_UNLOCK (gphoto2_volume);
}

static void
hal_changed (HalDevice *device, const char *key, gpointer user_data)
{
  GGPhoto2Volume *gphoto2_volume = G_GPHOTO2_VOLUME (user_data);

  /*g_warning ("hal modifying %s (property %s changed)", gphoto2_volume->device_path, key);*/
  update_from_hal (gphoto2_volume, TRUE);
}
#endif

GGPhoto2Volume *
g_gphoto2_volume_new (GVolumeMonitor   *volume_monitor,
#ifdef HAVE_GUDEV
                      GUdevDevice      *device,
                      GUdevClient      *gudev_client,
#else
                      HalDevice        *device,
                      HalPool          *pool,
#endif
                      GFile            *activation_root)
{
  GGPhoto2Volume *volume;
#ifndef HAVE_GUDEV
  HalDevice *drive_device;
  const char *storage_udi;
#endif
  const char *device_path;

  g_return_val_if_fail (volume_monitor != NULL, NULL);
  g_return_val_if_fail (device != NULL, NULL);
#ifdef HAVE_GUDEV
  g_return_val_if_fail (gudev_client != NULL, NULL);
#else
  g_return_val_if_fail (pool != NULL, NULL);
#endif
  g_return_val_if_fail (activation_root != NULL, NULL);

#ifdef HAVE_GUDEV
  if (!g_udev_device_has_property (device, "ID_GPHOTO2"))
      return NULL;
  device_path = g_udev_device_get_device_file (device);
#else
  if (!(hal_device_has_capability (device, "camera") ||
        (hal_device_has_capability (device, "portable_audio_player") &&
         hal_device_get_property_bool (device, "camera.libgphoto2.support"))))
    return NULL;

  /* OK, so we abuse storage_udi and drive_device for the USB main
   * device that holds this interface...
   */
  storage_udi = hal_device_get_property_string (device, "info.parent");
  if (storage_udi == NULL)
    return NULL;

  drive_device = hal_pool_get_device_by_udi (pool, storage_udi);
  if (drive_device == NULL)
    return NULL;

  /* TODO: other OS'es? Will address this with DK aka HAL 2.0 */
  device_path = hal_device_get_property_string (drive_device, "linux.device_file");
  if (strlen (device_path) == 0)
    device_path = NULL;
#endif

  volume = g_object_new (G_TYPE_GPHOTO2_VOLUME, NULL);
  volume->volume_monitor = volume_monitor;
  g_object_add_weak_pointer (G_OBJECT (volume_monitor), (gpointer) &(volume->volume_monitor));
  volume->device_path = g_strdup (device_path);
  volume->device = g_object_ref (device);
#ifndef HAVE_GUDEV
  volume->drive_device = g_object_ref (drive_device);
#endif
  volume->activation_root = g_object_ref (activation_root);

#ifdef HAVE_GUDEV
  set_volume_name (volume);
  set_volume_icon (volume);
  /* we do not really need to listen for changes */
#else
  g_signal_connect_object (device, "hal_property_changed", (GCallback) hal_changed, volume, 0);
  g_signal_connect_object (drive_device, "hal_property_changed", (GCallback) hal_changed, volume, 0);

  update_from_hal (volume, FALSE);
#endif

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

#ifdef HAVE_GUDEV
gboolean
g_gphoto2_volume_has_path (GGPhoto2Volume  *volume,
                      const char  *sysfs_path)
{
  GGPhoto2Volume *gphoto2_volume = G_GPHOTO2_VOLUME (volume);
  gboolean res;

  G_LOCK (gphoto2_volume);
  res = FALSE;
  if (gphoto2_volume->device != NULL)
    res = strcmp (g_udev_device_get_sysfs_path   (gphoto2_volume->device), sysfs_path) == 0;
  G_UNLOCK (gphoto2_volume);
  return res;
}

#else

gboolean
g_gphoto2_volume_has_udi (GGPhoto2Volume  *volume,
                      const char  *udi)
{
  GGPhoto2Volume *gphoto2_volume = G_GPHOTO2_VOLUME (volume);
  gboolean res;

  G_LOCK (gphoto2_volume);
  res = FALSE;
  if (gphoto2_volume->device != NULL)
    res = strcmp (hal_device_get_udi (gphoto2_volume->device), udi) == 0;
  G_UNLOCK (gphoto2_volume);
  return res;
}
#endif

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
#ifndef HAVE_GUDEV
  if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_HAL_UDI) == 0)
    id = g_strdup (hal_device_get_udi (gphoto2_volume->device));
  else 
#endif
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

#ifndef HAVE_GUDEV
  g_ptr_array_add (res,
                   g_strdup (G_VOLUME_IDENTIFIER_KIND_HAL_UDI));
#endif

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
