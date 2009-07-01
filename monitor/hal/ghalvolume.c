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

#include "ghaldrive.h"
#include "ghalvolume.h"
#include "ghalmount.h"

#include "hal-utils.h"

struct _GHalVolume {
  GObject parent;

  GVolumeMonitor *volume_monitor; /* owned by volume monitor */
  GHalMount      *mount;          /* owned by volume monitor */
  GHalDrive      *drive;          /* owned by volume monitor */

  char *device_path;
  char *mount_path;
  char *uuid;
  HalDevice *device;
  HalDevice *drive_device;

  /* set on creation if we won't create a GHalMount object ourselves
   * and instead except to adopt one, with the given mount root,
   * via adopt_orphan_mount() 
   */
  GFile *foreign_mount_root;
  GMount *foreign_mount;
  gboolean is_mountable;
  gboolean ignore_automount;

  char *name;
  char *icon;
  char *icon_fallback;
};

static void g_hal_volume_volume_iface_init (GVolumeIface *iface);

G_DEFINE_TYPE_EXTENDED (GHalVolume, g_hal_volume, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME,
                                               g_hal_volume_volume_iface_init))

static void
g_hal_volume_finalize (GObject *object)
{
  GHalVolume *volume;
  
  volume = G_HAL_VOLUME (object);

  if (volume->mount != NULL)
    g_hal_mount_unset_volume (volume->mount, volume);

  if (volume->drive != NULL)
    g_hal_drive_unset_volume (volume->drive, volume);
  
  g_free (volume->mount_path);
  g_free (volume->device_path);
  g_free (volume->uuid);
  if (volume->device != NULL)
    g_object_unref (volume->device);
  if (volume->drive_device != NULL)
    g_object_unref (volume->drive_device);

  if (volume->foreign_mount_root != NULL)
    g_object_unref (volume->foreign_mount_root);

  if (volume->foreign_mount != NULL)
    g_object_unref (volume->foreign_mount);

  if (volume->volume_monitor != NULL)
    g_object_remove_weak_pointer (G_OBJECT (volume->volume_monitor), (gpointer) &(volume->volume_monitor));

  g_free (volume->name);
  g_free (volume->icon);
  g_free (volume->icon_fallback);

  if (G_OBJECT_CLASS (g_hal_volume_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_hal_volume_parent_class)->finalize) (object);
}

static void
g_hal_volume_class_init (GHalVolumeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_hal_volume_finalize;
}

static void
g_hal_volume_init (GHalVolume *hal_volume)
{
}

static void
emit_volume_changed (GHalVolume *volume)
{
  g_signal_emit_by_name (volume, "changed");
  if (volume->volume_monitor != NULL)
    g_signal_emit_by_name (volume->volume_monitor, "volume_changed", volume);
}

#define KILOBYTE_FACTOR 1000.0
#define MEGABYTE_FACTOR (1000.0 * 1000.0)
#define GIGABYTE_FACTOR (1000.0 * 1000.0 * 1000.0)

/**
 * format_size_for_display:
 * @size: a number of octects
 *
 * Format a human readable string that can conveys how much storage a
 * user-visible drive or piece of media can hold.
 *
 * As a matter of policy, we want this string to resemble what's on
 * the packaging of the drive/media. Since all manufacturers use
 * powers of 10, g_format_size_for_display() is not suitable here.
 *
 * TODO: we probably want to round to nearest power of two if @size is
 * "close" (e.g. within 5%) - this is to avoid e.g. 63.4G when the
 * packaging says "64G drive". We could also use per-drive or
 * per-media quirks to make a better guess.
 *
 * Returns: A human readable string, caller must free it using
 * g_free().
 **/
static char *
format_size_for_display (guint64 size)
{
  char *str;
  gdouble displayed_size;
  
  if (size < MEGABYTE_FACTOR)
    {
      displayed_size = (double) size / KILOBYTE_FACTOR;
      str = g_strdup_printf (_("%.1f kB"), displayed_size);
    } 
  else if (size < GIGABYTE_FACTOR)
    {
      displayed_size = (double) size / MEGABYTE_FACTOR;
      str = g_strdup_printf (_("%.1f MB"), displayed_size);
    } 
  else 
    {
      displayed_size = (double) size / GIGABYTE_FACTOR;
      str = g_strdup_printf (_("%.1f GB"), displayed_size);
    }
  
  return str;
}

static void
do_update_from_hal (GHalVolume *mv)
{
  const char *drive_type;
  const char *drive_bus;
  gboolean drive_uses_removable_media;
  const char *volume_fs_label;
  guint64 volume_size;
  gboolean volume_is_disc;
  gboolean volume_disc_has_audio;
  gboolean volume_disc_has_data;
  const char *volume_disc_type;
  gboolean volume_disc_is_blank;
  const char *volume_fsusage;
  const char *volume_fstype;
  HalDevice *volume;
  HalDevice *drive;
  char *name;
  char *size;
  gboolean is_crypto;
  gboolean is_crypto_cleartext;

  volume = mv->device;
  drive = mv->drive_device;
  
  drive_type = hal_device_get_property_string (drive, "storage.drive_type");
  drive_bus = hal_device_get_property_string (drive, "storage.bus");
  drive_uses_removable_media = hal_device_get_property_bool (drive, "storage.removable");
  volume_fs_label = hal_device_get_property_string (volume, "volume.label");
  volume_size = hal_device_get_property_uint64 (volume, "volume.size");
  volume_is_disc = hal_device_get_property_bool (volume, "volume.is_disc");
  volume_disc_has_audio = hal_device_get_property_bool (volume, "volume.disc.has_audio");
  volume_disc_has_data = hal_device_get_property_bool (volume, "volume.disc.has_data");
  volume_disc_is_blank = hal_device_get_property_bool (volume, "volume.disc.is_blank");
  volume_disc_type = hal_device_get_property_string (volume, "volume.disc.type");
  volume_fsusage = hal_device_get_property_string (volume, "volume.fsusage");
  volume_fstype = hal_device_get_property_string (volume, "volume.fstype");

  is_crypto = FALSE;
  is_crypto_cleartext = FALSE;
  if (strcmp (hal_device_get_property_string (volume, "volume.fsusage"), "crypto") == 0)
    is_crypto = TRUE;
  if (strlen (hal_device_get_property_string (volume, "volume.crypto_luks.clear.backing_volume")) > 0)
    is_crypto_cleartext = TRUE;

  if (volume_is_disc && volume_disc_has_audio && mv->foreign_mount_root != NULL)
    name = g_strdup (_("Audio Disc"));
  else
    {
      if (strcmp (volume_fsusage, "crypto") == 0 && strcmp (volume_fstype, "crypto_LUKS") == 0)
        {
          size = format_size_for_display (volume_size);
          /* Translators: %s is the size of the volume (e.g. 512 MB) */
          name = g_strdup_printf (_("%s Encrypted Data"), size);
          g_free (size);
        }
      else
        {
          if (volume_fs_label != NULL && strlen (volume_fs_label) > 0)
            name = g_strdup (volume_fs_label);
          else if (volume_is_disc)
            {
              if (volume_disc_has_audio)
                {
                  if (volume_disc_has_data)
                    name = g_strdup (_("Mixed Audio/Data Disc"));
                  else
                    name = g_strdup (_("Audio Disc"));
                }
              else
                name = g_strdup (get_disc_name (volume_disc_type, volume_disc_is_blank));
            }
          else
            {
              size = format_size_for_display (volume_size);
              /* Translators: %s is the size of the volume (e.g. 512 MB) */
              name = g_strdup_printf (_("%s Media"), size);
              g_free (size);
            }
        }
    }

  mv->name = name;
  mv->icon = _drive_get_icon (drive); /* use the drive icon since we're unmounted */

  if (is_crypto || is_crypto_cleartext)
    {
      mv->icon_fallback = mv->icon;
      mv->icon = g_strdup ("drive-encrypted");
    }

  if (hal_device_get_property_bool (volume, "volume.is_mounted"))
    mv->mount_path = g_strdup (hal_device_get_property_string (volume, "volume.mount_point"));
  else
    mv->mount_path = NULL;

  g_object_set_data_full (G_OBJECT (mv), 
                          "hal-storage-device-capabilities",
                          dupv_and_uniqify (hal_device_get_property_strlist (mv->drive_device, "info.capabilities")),
                          (GDestroyNotify) g_strfreev);

  if (volume_disc_type != NULL && strlen (volume_disc_type) == 0)
    volume_disc_type = NULL;
  g_object_set_data_full (G_OBJECT (mv), 
                          "hal-volume.disc.type",
                          g_strdup (volume_disc_type),
                          (GDestroyNotify) g_free);
}

static void
update_from_hal (GHalVolume *mv, gboolean emit_changed)
{
  char *old_name;
  char *old_icon;
  char *old_mount_path;

  old_name = g_strdup (mv->name);
  old_icon = g_strdup (mv->icon);
  old_mount_path = g_strdup (mv->mount_path);

  g_free (mv->name);
  g_free (mv->icon);
  g_free (mv->mount_path);
  do_update_from_hal (mv);

  if (emit_changed)
    {
      gboolean mount_path_changed;

      if ((old_mount_path == NULL && mv->mount_path != NULL) ||
          (old_mount_path != NULL && mv->mount_path == NULL) ||
          (old_mount_path != NULL && mv->mount_path != NULL && strcmp (old_mount_path, mv->mount_path) != 0))
        mount_path_changed = TRUE;
      else
        mount_path_changed = FALSE;

      if (mount_path_changed ||
          (old_name == NULL || 
           old_icon == NULL ||
           strcmp (old_name, mv->name) != 0 ||
           strcmp (old_icon, mv->icon) != 0))
	emit_volume_changed (mv);
    }
  g_free (old_name);
  g_free (old_icon);
  g_free (old_mount_path);
}

static void
hal_changed (HalDevice *device, const char *key, gpointer user_data)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (user_data);
  
  /*g_warning ("hal modifying %s (property %s changed)", hal_volume->device_path, key);*/
  update_from_hal (hal_volume, TRUE);
}

static void
compute_uuid (GHalVolume *volume)
{
  const char *fs_uuid;
  const char *fs_label;

  /* use the FS uuid before falling back to the FS label */

  fs_uuid = hal_device_get_property_string (volume->device, "volume.uuid");
  fs_label = hal_device_get_property_string (volume->device, "volume.label");

  if (strlen (fs_uuid) == 0)
    {
      if (strlen (fs_label) == 0)
        volume->uuid = NULL;
      else
        volume->uuid = g_strdup (fs_label);
    }
  else
    volume->uuid = g_strdup (fs_uuid);
}

GHalVolume *
g_hal_volume_new (GVolumeMonitor   *volume_monitor,
                  HalDevice        *device,
                  HalPool          *pool,
                  GFile            *foreign_mount_root,
                  gboolean          is_mountable,
                  GHalDrive        *drive)
{
  GHalVolume *volume;
  HalDevice *drive_device;
  const char *storage_udi;
  const char *device_path;
  gboolean ignore_automount;

  ignore_automount = FALSE;
  
  if (hal_device_has_capability (device, "block"))
    {
      storage_udi = hal_device_get_property_string (device, "block.storage_device");
      if (storage_udi == NULL)
        return NULL;
      
      drive_device = hal_pool_get_device_by_udi (pool, storage_udi);
      if (drive_device == NULL)
        return NULL;
      
      device_path = hal_device_get_property_string (device, "block.device");
    }
  else
    {
      return NULL;
    }

  if (drive_device && 
      hal_device_has_property (drive_device, "storage.automount_enabled_hint") &&
      !hal_device_get_property_bool (drive_device, "storage.automount_enabled_hint"))
    ignore_automount = TRUE;
  
  volume = g_object_new (G_TYPE_HAL_VOLUME, NULL);
  volume->volume_monitor = volume_monitor;
  g_object_add_weak_pointer (G_OBJECT (volume_monitor), (gpointer) &(volume->volume_monitor));
  volume->mount_path = NULL;
  volume->device_path = g_strdup (device_path);
  volume->device = g_object_ref (device);
  volume->drive_device = g_object_ref (drive_device);
  volume->foreign_mount_root = foreign_mount_root != NULL ? g_object_ref (foreign_mount_root) : NULL;
  volume->is_mountable = is_mountable;
  volume->ignore_automount = ignore_automount || ! hal_device_is_recently_plugged_in (device);
  
  g_signal_connect_object (device, "hal_property_changed", (GCallback) hal_changed, volume, 0);
  g_signal_connect_object (drive_device, "hal_property_changed", (GCallback) hal_changed, volume, 0);

  compute_uuid (volume);
  update_from_hal (volume, FALSE);

  /* need to do this last */
  volume->drive = drive;
  if (drive != NULL)
    g_hal_drive_set_volume (drive, volume);
  
  return volume;
}

/**
 * g_hal_volume_removed:
 * @volume:
 * 
 **/
void
g_hal_volume_removed (GHalVolume *volume)
{

  if (volume->mount != NULL)
    {
      g_hal_mount_unset_volume (volume->mount, volume);
      volume->mount = NULL;
    }

  if (volume->drive != NULL)
    {
      g_hal_drive_unset_volume (volume->drive, volume);
      volume->drive = NULL;
    }
}

void
g_hal_volume_set_mount (GHalVolume  *volume,
                        GHalMount *mount)
{
  if (volume->mount != mount)
    {
      
      if (volume->mount != NULL)
        g_hal_mount_unset_volume (volume->mount, volume);
      
      volume->mount = mount;

      emit_volume_changed (volume);
    }
}
 
void
g_hal_volume_unset_mount (GHalVolume  *volume,
                          GHalMount *mount)
{
  if (volume->mount == mount)
    {
      volume->mount = NULL;
      emit_volume_changed (volume);
    }
}

void
g_hal_volume_set_drive (GHalVolume  *volume,
                        GHalDrive *drive)
{
  if (volume->drive != drive)
    {
      if (volume->drive != NULL)
        g_hal_drive_unset_volume (volume->drive, volume);
      
      volume->drive = drive;
      
      emit_volume_changed (volume);
    }
}

void
g_hal_volume_unset_drive (GHalVolume  *volume,
                          GHalDrive *drive)
{
  if (volume->drive == drive)
    {
      volume->drive = NULL;
      emit_volume_changed (volume);
    }
}

static GIcon *
g_hal_volume_get_icon (GVolume *volume)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);
  GIcon *icon;
  const char *name;
  const char *fallback;

  name = hal_volume->icon;

  if (hal_volume->icon_fallback)
    fallback = hal_volume->icon_fallback;
  else /* if no custom fallbacks are set, use the icon to create them */
    fallback = name;

  icon = get_themed_icon_with_fallbacks (name, fallback);
  return icon;
}

static char *
g_hal_volume_get_name (GVolume *volume)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);

  return g_strdup (hal_volume->name);
}

static char *
g_hal_volume_get_uuid (GVolume *volume)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);

  return g_strdup (hal_volume->uuid);
}

static gboolean
g_hal_volume_can_mount (GVolume *volume)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);

  return hal_volume->is_mountable;
}

static gboolean
g_hal_volume_can_eject (GVolume *volume)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);
  gboolean res;

  res = FALSE;
  if (hal_volume->drive != NULL)
    res = g_drive_can_eject (G_DRIVE (hal_volume->drive));
  
  return res;
}

static gboolean
g_hal_volume_should_automount (GVolume *volume)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);

  return  ! (hal_volume->ignore_automount);
}

static GDrive *
g_hal_volume_get_drive (GVolume *volume)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);
  GDrive *drive;

  drive = NULL;
  if (hal_volume->drive != NULL)
    drive = g_object_ref (hal_volume->drive);
  
  return drive;
}

static GMount *
g_hal_volume_get_mount (GVolume *volume)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);
  GMount *mount;

  mount = NULL;
  if (hal_volume->foreign_mount != NULL)
    mount = g_object_ref (hal_volume->foreign_mount);
  else if (hal_volume->mount != NULL)
    mount = g_object_ref (hal_volume->mount);

  return mount;
}

gboolean
g_hal_volume_has_mount_path (GHalVolume *volume,
                             const char  *mount_path)
{
  gboolean res;

  res = FALSE;
  if (volume->mount_path != NULL)
    res = strcmp (volume->mount_path, mount_path) == 0;

  return res;
}

gboolean
g_hal_volume_has_device_path (GHalVolume *volume,
                              const char  *device_path)
{
  gboolean res;

  res = FALSE;
  if (volume->device_path != NULL)
    res = strcmp (volume->device_path, device_path) == 0;
  return res;
}

gboolean
g_hal_volume_has_udi (GHalVolume  *volume,
                      const char  *udi)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);
  gboolean res;
  
  res = FALSE;
  if (hal_volume->device != NULL)
    res = strcmp (hal_device_get_udi (hal_volume->device), udi) == 0;
  return res;
}

gboolean
g_hal_volume_has_uuid (GHalVolume  *volume,
                       const char  *uuid)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);
  gboolean res;

  res = FALSE;
  if (hal_volume->uuid != NULL)
    res = strcmp (hal_volume->uuid, uuid) == 0;

  return res;
}

static void
foreign_mount_unmounted (GMount *mount, gpointer user_data)
{
  GHalVolume *volume = G_HAL_VOLUME (user_data);
  gboolean check;

  check = volume->foreign_mount == mount;
  if (check)
    g_hal_volume_adopt_foreign_mount (volume, NULL);
}

void
g_hal_volume_adopt_foreign_mount (GHalVolume *volume, GMount *foreign_mount)
{
  if (volume->foreign_mount != NULL)
    g_object_unref (volume->foreign_mount);

  if (foreign_mount != NULL)
    {
      volume->foreign_mount =  g_object_ref (foreign_mount);
      g_signal_connect_object (foreign_mount, "unmounted", (GCallback) foreign_mount_unmounted, volume, 0);
    }
  else
    volume->foreign_mount =  NULL;
  
  emit_volume_changed (volume);
}

gboolean
g_hal_volume_has_foreign_mount_root (GHalVolume       *volume,
                                     GFile            *mount_root)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);
  gboolean res;

  res = FALSE;
  if (hal_volume->foreign_mount_root != NULL)
    res = g_file_equal (hal_volume->foreign_mount_root, mount_root);
  
  return res;
}


typedef struct {
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
} SpawnOp;

static void 
spawn_cb (GPid pid, gint status, gpointer user_data)
{
  SpawnOp *data = user_data;
  GSimpleAsyncResult *simple;

  /* ensure that the #GHalMount corrosponding to the #GHalVolume we've
   * mounted is made available before returning to the user (make sure
   * we don't emit the signals in idle; see #552168).
   */
  g_hal_volume_monitor_force_update (G_HAL_VOLUME_MONITOR (G_HAL_VOLUME (data->object)->volume_monitor), FALSE);

  if (WEXITSTATUS (status) != 0)
    {
      GError *error;
      error = g_error_new_literal (G_IO_ERROR, 
                                   G_IO_ERROR_FAILED_HANDLED,
                                   "You are not supposed to show G_IO_ERROR_FAILED_HANDLED in the UI");
      simple = g_simple_async_result_new_from_error (data->object,
                                                     data->callback,
                                                     data->user_data,
                                                     error);
      g_error_free (error);
    }
  else
    {
      simple = g_simple_async_result_new (data->object,
                                          data->callback,
                                          data->user_data,
                                          NULL);
    }

  g_simple_async_result_complete (simple);
  g_object_unref (simple);
  g_object_unref (data->object);
  g_free (data);
}

static void
spawn_do (GVolume             *volume,
          GCancellable        *cancellable,
          GAsyncReadyCallback  callback,
          gpointer             user_data,
          char               **argv)
{
  SpawnOp *data;
  GPid child_pid;
  GError *error;

  data = g_new0 (SpawnOp, 1);
  data->object = g_object_ref (volume);
  data->callback = callback;
  data->user_data = user_data;
  data->cancellable = cancellable;
  
  error = NULL;
  if (!g_spawn_async (NULL,         /* working dir */
                      argv,
                      NULL,         /* envp */
                      G_SPAWN_DO_NOT_REAP_CHILD|G_SPAWN_SEARCH_PATH,
                      NULL,         /* child_setup */
                      NULL,         /* user_data for child_setup */
                      &child_pid,
                      &error))
    {
      g_simple_async_report_gerror_in_idle (data->object,
                                            data->callback,
                                            data->user_data,
                                            error);
      g_object_unref (data->object);
      g_error_free (error);
      g_free (data);
      return;
    }
  
  g_child_watch_add (child_pid, spawn_cb, data);
}

typedef struct
{
  GHalVolume *enclosing_volume;
  GAsyncReadyCallback  callback;
  gpointer user_data;
} ForeignMountOp;

static void
mount_foreign_callback (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data)
{
  ForeignMountOp *data = user_data;
  data->callback (G_OBJECT (data->enclosing_volume), res, data->user_data);
  g_object_unref (data->enclosing_volume);
  g_free (data);
}

static void
g_hal_volume_mount (GVolume             *volume,
                    GMountMountFlags     flags,
                    GMountOperation     *mount_operation,
                    GCancellable        *cancellable,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);

  /*g_warning ("hal_volume_mount (can_mount=%d foreign=%p device_path=%s)", 
              g_hal_volume_can_mount (volume),
              hal_volume->foreign_mount_root,
              hal_volume->device_path);*/

  if (hal_volume->foreign_mount_root != NULL)
    {
      ForeignMountOp *data;

      data = g_new0 (ForeignMountOp, 1);
      data->enclosing_volume = g_object_ref (hal_volume);
      data->callback = callback;
      data->user_data = user_data;

      g_file_mount_enclosing_volume (hal_volume->foreign_mount_root,
                                     0,
                                     mount_operation, 
                                     cancellable, 
                                     mount_foreign_callback, 
                                     data);
    }
  else
    {
      char *argv[] = {"gnome-mount", "-b", "-d", NULL, NULL, NULL};
      argv[3] = hal_volume->device_path;
      /* ask for no dialogs if mount_operation is NULL */
      if (mount_operation == NULL)
        argv[4] = "-n";
      spawn_do (volume, cancellable, callback, user_data, argv);
    }
}

static gboolean
g_hal_volume_mount_finish (GVolume       *volume,
                           GAsyncResult  *result,
                           GError       **error)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);
  gboolean res;

  res = TRUE;
  
  if (hal_volume->foreign_mount_root != NULL)
    res = g_file_mount_enclosing_volume_finish (hal_volume->foreign_mount_root, result, error);
  
  return res;
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
g_hal_volume_eject_with_operation (GVolume              *volume,
                                   GMountUnmountFlags   flags,
                                   GMountOperation     *mount_operation,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);
  GHalDrive *drive;

  /*g_warning ("hal_volume_eject");*/

  drive = NULL;
  if (hal_volume->drive != NULL)
    drive = g_object_ref (hal_volume->drive);

  if (drive != NULL)
    {
      EjectWrapperOp *data;
      data = g_new0 (EjectWrapperOp, 1);
      data->object = g_object_ref (volume);
      data->callback = callback;
      data->user_data = user_data;
      g_drive_eject_with_operation (G_DRIVE (drive), flags, mount_operation, cancellable, eject_wrapper_callback, data);
      g_object_unref (drive);
    }
}

static gboolean
g_hal_volume_eject_with_operation_finish (GVolume        *volume,
                                          GAsyncResult  *result,
                                          GError       **error)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);
  gboolean res;

  res = TRUE;
  if (hal_volume->drive != NULL)
    res = g_drive_eject_with_operation_finish (G_DRIVE (hal_volume->drive), result, error);
  return res;
}

static void
g_hal_volume_eject (GVolume              *volume,
                    GMountUnmountFlags   flags,
                    GCancellable        *cancellable,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
  return g_hal_volume_eject_with_operation (volume, flags, NULL, cancellable, callback, user_data);
}

static gboolean
g_hal_volume_eject_finish (GVolume        *volume,
                           GAsyncResult  *result,
                           GError       **error)
{
  return g_hal_volume_eject_with_operation_finish (volume, result, error);
}

static char *
g_hal_volume_get_identifier (GVolume              *volume,
                             const char          *kind)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);
  char *id;

  id = NULL;
  if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_HAL_UDI) == 0)
    id = g_strdup (hal_device_get_udi (hal_volume->device));
  else if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE) == 0)
    id = g_strdup (hal_volume->device_path);
  else if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_LABEL) == 0)
    id = g_strdup (hal_device_get_property_string (hal_volume->device, "volume.label"));
  else if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_UUID) == 0)
    id = g_strdup (hal_device_get_property_string (hal_volume->device, "volume.uuid"));
  
  return id;
}

static char **
g_hal_volume_enumerate_identifiers (GVolume *volume)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);
  GPtrArray *res;
  const char *label, *uuid;

  res = g_ptr_array_new ();

  g_ptr_array_add (res,
                   g_strdup (G_VOLUME_IDENTIFIER_KIND_HAL_UDI));

  if (hal_volume->device_path && *hal_volume->device_path != 0)
    g_ptr_array_add (res,
                     g_strdup (G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));

  label = hal_device_get_property_string (hal_volume->device, "volume.label");
  uuid = hal_device_get_property_string (hal_volume->device, "volume.uuid");

  if (label && *label != 0)
    g_ptr_array_add (res,
                     g_strdup (G_VOLUME_IDENTIFIER_KIND_LABEL));

  if (uuid && *uuid != 0)
    g_ptr_array_add (res,
                     g_strdup (G_VOLUME_IDENTIFIER_KIND_UUID));

  /* Null-terminate */
  g_ptr_array_add (res, NULL);

  return (char **)g_ptr_array_free (res, FALSE);
}

static GFile *
g_hal_volume_get_activation_root (GVolume *volume)
{
  GHalVolume *hal_volume = G_HAL_VOLUME (volume);
  GFile *root = NULL;

  if (hal_volume->foreign_mount_root != NULL)
    root = g_object_ref (hal_volume->foreign_mount_root);

  return root;
}

static void
g_hal_volume_volume_iface_init (GVolumeIface *iface)
{
  iface->get_name = g_hal_volume_get_name;
  iface->get_icon = g_hal_volume_get_icon;
  iface->get_uuid = g_hal_volume_get_uuid;
  iface->get_drive = g_hal_volume_get_drive;
  iface->get_mount = g_hal_volume_get_mount;
  iface->can_mount = g_hal_volume_can_mount;
  iface->can_eject = g_hal_volume_can_eject;
  iface->should_automount = g_hal_volume_should_automount;
  iface->mount_fn = g_hal_volume_mount;
  iface->mount_finish = g_hal_volume_mount_finish;
  iface->eject = g_hal_volume_eject;
  iface->eject_finish = g_hal_volume_eject_finish;
  iface->eject_with_operation = g_hal_volume_eject_with_operation;
  iface->eject_with_operation_finish = g_hal_volume_eject_with_operation_finish;
  iface->get_identifier = g_hal_volume_get_identifier;
  iface->enumerate_identifiers = g_hal_volume_enumerate_identifiers;
  iface->get_activation_root = g_hal_volume_get_activation_root;
}
