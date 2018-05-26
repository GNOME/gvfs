/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2012 Red Hat, Inc.
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

#include "gvfsudisks2volumemonitor.h"
#include "gvfsudisks2drive.h"
#include "gvfsudisks2volume.h"
#include "gvfsudisks2utils.h"

typedef struct _GVfsUDisks2DriveClass GVfsUDisks2DriveClass;

struct _GVfsUDisks2DriveClass
{
  GObjectClass parent_class;
};

struct _GVfsUDisks2Drive
{
  GObject parent;

  GVfsUDisks2VolumeMonitor  *monitor; /* owned by volume monitor */
  GList                     *volumes; /* entries in list are owned by volume monitor */

  /* If TRUE, the drive was discovered at coldplug time */
  gboolean coldplug;

  UDisksDrive *udisks_drive;

  GIcon *icon;
  GIcon *symbolic_icon;
  gchar *name;
  gchar *sort_key;
  gchar *device_file;
  dev_t dev;
  gboolean is_removable;
  gboolean is_media_removable;
  gboolean has_media;
  gboolean can_eject;
  gboolean can_stop;
};

static void gvfs_udisks2_drive_drive_iface_init (GDriveIface *iface);

static void on_udisks_drive_notify (GObject     *object,
                                    GParamSpec  *pspec,
                                    gpointer     user_data);

G_DEFINE_TYPE_EXTENDED (GVfsUDisks2Drive, gvfs_udisks2_drive, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_DRIVE, gvfs_udisks2_drive_drive_iface_init))

static void
gvfs_udisks2_drive_finalize (GObject *object)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (object);
  GList *l;

  for (l = drive->volumes; l != NULL; l = l->next)
    {
      GVfsUDisks2Volume *volume = l->data;
      gvfs_udisks2_volume_unset_drive (volume, drive);
    }

  if (drive->udisks_drive != NULL)
    {
      g_signal_handlers_disconnect_by_func (drive->udisks_drive, on_udisks_drive_notify, drive);
      g_object_unref (drive->udisks_drive);
    }

  g_clear_object (&drive->icon);
  g_clear_object (&drive->symbolic_icon);
  g_free (drive->name);
  g_free (drive->sort_key);
  g_free (drive->device_file);

  G_OBJECT_CLASS (gvfs_udisks2_drive_parent_class)->finalize (object);
}

static void
gvfs_udisks2_drive_class_init (GVfsUDisks2DriveClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = gvfs_udisks2_drive_finalize;
}

static void
gvfs_udisks2_drive_init (GVfsUDisks2Drive *drive)
{
}

static void
emit_changed (GVfsUDisks2Drive *drive)
{
  g_signal_emit_by_name (drive, "changed");
  g_signal_emit_by_name (drive->monitor, "drive-changed", drive);
}


#if UDISKS_CHECK_VERSION(2,0,90)
static gpointer
_g_object_ref0 (gpointer object)
{
  if (object != NULL)
    return g_object_ref (G_OBJECT (object));
  else
    return NULL;
}
#endif

static gboolean
update_drive (GVfsUDisks2Drive *drive)
{
  UDisksClient *udisks_client;
  gboolean changed;
  GIcon *old_icon;
  GIcon *old_symbolic_icon;
  gchar *old_name;
  gchar *old_sort_key;
  gchar *old_device_file;
  dev_t old_dev;
  gboolean old_is_removable;
  gboolean old_is_media_removable;
  gboolean old_has_media;
  gboolean old_can_eject;
  gboolean old_can_stop;
  UDisksBlock *block;
#if UDISKS_CHECK_VERSION(2,0,90)
  UDisksObjectInfo *info = NULL;
#endif

  udisks_client = gvfs_udisks2_volume_monitor_get_udisks_client (drive->monitor);

  /* ---------------------------------------------------------------------------------------------------- */
  /* save old values */

  old_is_removable = drive->is_removable;
  old_is_media_removable = drive->is_media_removable;
  old_has_media = drive->has_media;
  old_can_eject = drive->can_eject;
  old_can_stop = drive->can_stop;

  old_name = g_strdup (drive->name);
  old_sort_key = g_strdup (drive->sort_key);
  old_device_file = g_strdup (drive->device_file);
  old_dev = drive->dev;
  old_icon = drive->icon != NULL ? g_object_ref (drive->icon) : NULL;
  old_symbolic_icon = drive->symbolic_icon != NULL ? g_object_ref (drive->symbolic_icon) : NULL;

  /* ---------------------------------------------------------------------------------------------------- */
  /* reset */

  drive->is_removable = drive->is_media_removable = drive->has_media = drive->can_eject = drive->can_stop = FALSE;
  g_free (drive->name); drive->name = NULL;
  g_free (drive->sort_key); drive->sort_key = NULL;
  g_free (drive->device_file); drive->device_file = NULL;
  drive->dev = 0;
  g_clear_object (&drive->icon);
  g_clear_object (&drive->symbolic_icon);

  /* ---------------------------------------------------------------------------------------------------- */
  /* in with the new */

  block = udisks_client_get_block_for_drive (udisks_client,
                                             drive->udisks_drive,
                                             FALSE);
  if (block != NULL)
    {
      drive->device_file = udisks_block_dup_device (block);
      drive->dev = udisks_block_get_device_number (block);

      g_object_unref (block);
    }

  drive->sort_key = g_strdup (udisks_drive_get_sort_key (drive->udisks_drive));

  drive->is_removable = udisks_drive_get_removable (drive->udisks_drive);
  drive->is_media_removable = udisks_drive_get_media_removable (drive->udisks_drive);
  if (drive->is_media_removable)
    {
      drive->has_media = udisks_drive_get_media_available (drive->udisks_drive);
    }
  else
    {
      drive->has_media = TRUE;
    }
  drive->can_eject = udisks_drive_get_ejectable (drive->udisks_drive);

#if UDISKS_CHECK_VERSION(2,0,90)
  {
    UDisksObject *object = (UDisksObject *) g_dbus_interface_get_object (G_DBUS_INTERFACE (drive->udisks_drive));
    if (object != NULL)
      {
        info = udisks_client_get_object_info (udisks_client, object);
        if (info != NULL)
          {
            drive->name = g_strdup (udisks_object_info_get_name (info));
            drive->icon = _g_object_ref0 (udisks_object_info_get_icon (info));
            drive->symbolic_icon = _g_object_ref0 (udisks_object_info_get_icon_symbolic (info));
          }
      }
  }
#else
  udisks_client_get_drive_info (udisks_client,
                                drive->udisks_drive,
                                NULL,         /* drive_name */
                                &drive->name,
                                &drive->icon,
                                NULL,         /* media_desc */
                                NULL);        /* media_icon */
#endif

#if UDISKS_CHECK_VERSION(2,0,90)
  {
    /* If can_stop is TRUE, then
     *
     *  - the GUI (e.g. Files, Shell) will call GDrive.stop() whenever the
     *    user presses the Eject icon, which will result in:
     *
     *  - us calling UDisksDrive.PowerOff() on GDrive.stop(), which
     *    will result in:
     *
     *  - UDisks asking the kernel to power off the USB port the drive
     *    is connected to, which will result in
     *
     *  - Most drives powering off (especially true for bus-powered
     *    drives such as 2.5" HDDs and USB sticks), which will result in
     *
     *  - Users feeling warm and cozy when they see the LED on the
     *    device turn off (win)
     *
     * Obviously this is unwanted if
     *
     *  - the drive is using removable media (e.g. optical discs,
     *    flash media etc); or
     *
     *  - the device is internal
     *
     * So for the latter, only do this for drives we appear *during*
     * the login session.  Note that this heuristic has the nice
     * side-effect that USB-attached hard disks that are plugged in
     * when the computer starts up will not be powered off when the
     * user clicks the "eject" icon.
     */
    if (!drive->is_media_removable && !drive->coldplug)
      {
        if (udisks_drive_get_can_power_off (drive->udisks_drive))
          {
            drive->can_stop = TRUE;
          }
      }
  }
#endif

  /* ---------------------------------------------------------------------------------------------------- */
  /* fallbacks */

  /* Never use empty/blank names (#582772) */
  if (drive->name == NULL || strlen (drive->name) == 0)
    {
      if (drive->device_file != NULL)
        drive->name = g_strdup_printf (_("Unnamed Drive (%s)"), drive->device_file);
      else
        drive->name = g_strdup (_("Unnamed Drive"));
    }
  if (drive->icon == NULL)
    drive->icon = g_themed_icon_new ("drive-removable-media");
  if (drive->symbolic_icon == NULL)
    drive->symbolic_icon = g_themed_icon_new ("drive-removable-media-symbolic");

  /* ---------------------------------------------------------------------------------------------------- */
  /* compute whether something changed */
  changed = !((old_is_removable == drive->is_removable) &&
              (old_is_media_removable == drive->is_media_removable) &&
              (old_has_media == drive->has_media) &&
              (old_can_eject == drive->can_eject) &&
              (old_can_stop == drive->can_stop) &&
              (g_strcmp0 (old_name, drive->name) == 0) &&
              (g_strcmp0 (old_sort_key, drive->sort_key) == 0) &&
              (g_strcmp0 (old_device_file, drive->device_file) == 0) &&
              (old_dev == drive->dev) &&
              g_icon_equal (old_icon, drive->icon) &&
              g_icon_equal (old_symbolic_icon, drive->symbolic_icon)
              );

  /* free old values */
  g_free (old_name);
  g_free (old_sort_key);
  g_free (old_device_file);
  g_clear_object (&old_icon);
  g_clear_object (&old_symbolic_icon);

  /*g_debug ("in update_drive(); has_media=%d changed=%d", drive->has_media, changed);*/

#if UDISKS_CHECK_VERSION(2,0,90)
  g_clear_object (&info);
#endif

  return changed;
}

static void
on_udisks_drive_notify (GObject     *object,
                        GParamSpec  *pspec,
                        gpointer     user_data)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (user_data);
  if (update_drive (drive))
    emit_changed (drive);
}

GVfsUDisks2Drive *
gvfs_udisks2_drive_new (GVfsUDisks2VolumeMonitor  *monitor,
                        UDisksDrive               *udisks_drive,
                        gboolean                   coldplug)
{
  GVfsUDisks2Drive *drive;

  drive = g_object_new (GVFS_TYPE_UDISKS2_DRIVE, NULL);
  drive->monitor = monitor;
  drive->coldplug = coldplug;

  drive->udisks_drive = g_object_ref (udisks_drive);
  g_signal_connect (drive->udisks_drive,
                    "notify",
                    G_CALLBACK (on_udisks_drive_notify),
                    drive);

  update_drive (drive);

  return drive;
}

void
gvfs_udisks2_drive_disconnected (GVfsUDisks2Drive *drive)
{
  GList *l, *volumes;

  volumes = drive->volumes;
  drive->volumes = NULL;
  for (l = volumes; l != NULL; l = l->next)
    {
      GVfsUDisks2Volume *volume = l->data;
      gvfs_udisks2_volume_unset_drive (volume, drive);
    }
  g_list_free (volumes);
}

void
gvfs_udisks2_drive_set_volume (GVfsUDisks2Drive  *drive,
                               GVfsUDisks2Volume *volume)
{
  if (g_list_find (drive->volumes, volume) == NULL)
    {
      drive->volumes = g_list_prepend (drive->volumes, volume);
      emit_changed (drive);
    }
}

void
gvfs_udisks2_drive_unset_volume (GVfsUDisks2Drive  *drive,
                                 GVfsUDisks2Volume *volume)
{
  GList *l;
  l = g_list_find (drive->volumes, volume);
  if (l != NULL)
    {
      drive->volumes = g_list_delete_link (drive->volumes, l);
      emit_changed (drive);
    }
}

static GIcon *
gvfs_udisks2_drive_get_icon (GDrive *_drive)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  return drive->icon != NULL ? g_object_ref (drive->icon) : NULL;
}

static GIcon *
gvfs_udisks2_drive_get_symbolic_icon (GDrive *_drive)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  return drive->symbolic_icon != NULL ? g_object_ref (drive->symbolic_icon) : NULL;
}

static char *
gvfs_udisks2_drive_get_name (GDrive *_drive)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  return g_strdup (drive->name);
}

static GList *
gvfs_udisks2_drive_get_volumes (GDrive *_drive)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  GList *l;
  l = g_list_copy (drive->volumes);
  g_list_foreach (l, (GFunc) g_object_ref, NULL);
  return l;
}

static gboolean
gvfs_udisks2_drive_has_volumes (GDrive *_drive)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  gboolean res;
  res = drive->volumes != NULL;
  return res;
}

static gboolean
gvfs_udisks2_drive_is_removable (GDrive *_drive)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  return drive->is_removable;
}

static gboolean
gvfs_udisks2_drive_is_media_removable (GDrive *_drive)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  return drive->is_media_removable;
}

static gboolean
gvfs_udisks2_drive_has_media (GDrive *_drive)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  return drive->has_media;
}

static gboolean
gvfs_udisks2_drive_is_media_check_automatic (GDrive *_drive)
{
  return TRUE;
}

static gboolean
gvfs_udisks2_drive_can_eject (GDrive *_drive)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  return drive->can_eject;
}

static gboolean
gvfs_udisks2_drive_can_poll_for_media (GDrive *_drive)
{
  return FALSE;
}

static gboolean
gvfs_udisks2_drive_can_start (GDrive *_drive)
{
  return FALSE;
}

static gboolean
gvfs_udisks2_drive_can_start_degraded (GDrive *_drive)
{
  return FALSE;
}

static gboolean
gvfs_udisks2_drive_can_stop (GDrive *_drive)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  return drive->can_stop;
}

static GDriveStartStopType
gvfs_udisks2_drive_get_start_stop_type (GDrive *_drive)
{
  return G_DRIVE_START_STOP_TYPE_SHUTDOWN;
}

/* ---------------------------------------------------------------------------------------------------- */

static char *
gvfs_udisks2_drive_get_identifier (GDrive      *_drive,
                                   const gchar *kind)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  gchar *ret = NULL;

  if (drive->device_file != NULL)
    {
      if (g_strcmp0 (kind, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE) == 0)
        ret = g_strdup (drive->device_file);
    }
  return ret;
}

static gchar **
gvfs_udisks2_drive_enumerate_identifiers (GDrive *_drive)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  GPtrArray *p;

  p = g_ptr_array_new ();
  if (drive->device_file != NULL)
    g_ptr_array_add (p, g_strdup (G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));
  g_ptr_array_add (p, NULL);

  return (gchar **) g_ptr_array_free (p, FALSE);
}

/* ---------------------------------------------------------------------------------------------------- */

typedef void (*UnmountAndLockFunc) (GTask *task);

typedef struct {
  GMountUnmountFlags flags;

  GList *pending_mounts;
  GList *pending_encrypted;

  GTask *task;

  UnmountAndLockFunc on_all_unmounted;
} UnmountAndLockOp;

static void
free_unmount_and_lock_op (UnmountAndLockOp *data)
{
  g_list_free_full (data->pending_mounts, g_object_unref);

  g_free (data);
}

static void
unmount_cb (GObject       *source_object,
            GAsyncResult  *res,
            gpointer       user_data);

static void
lock_cb (GObject       *source_object,
         GAsyncResult  *res,
         gpointer       user_data);

static void
unmount_and_lock_do (UnmountAndLockOp *data)
{
  if (data->pending_mounts != NULL)
    {
      GMount *mount;
      mount = data->pending_mounts->data;
      data->pending_mounts = g_list_remove (data->pending_mounts, mount);

      g_mount_unmount_with_operation (mount,
                                      data->flags,
                                      g_task_get_task_data (data->task),
                                      g_task_get_cancellable (data->task),
                                      unmount_cb,
                                      data);
    }
  else if (data->pending_encrypted != NULL)
    {
      UDisksEncrypted *encrypted;

      encrypted = data->pending_encrypted->data;
      data->pending_encrypted = g_list_remove (data->pending_encrypted, encrypted);

      udisks_encrypted_call_lock (encrypted,
                                  g_variant_new ("a{sv}", NULL), /* options */
                                  g_task_get_cancellable (data->task),
                                  lock_cb,
                                  data);
    }
  else
    {
      data->on_all_unmounted (g_object_ref (data->task));
      free_unmount_and_lock_op (data);
    }
}

static void
unmount_cb (GObject      *source_object,
            GAsyncResult *res,
            gpointer      user_data)
{
  UnmountAndLockOp *data = user_data;
  GMount *mount = G_MOUNT (source_object);
  GError *error = NULL;
  GMountOperation *mount_operation = g_task_get_task_data (data->task);

  if (!g_mount_unmount_with_operation_finish (mount, res, &error))
    {
      /* make the error dialog more targeted to the drive.. unless the user has already seen a dialog */
      if (error->domain == G_IO_ERROR && error->code == G_IO_ERROR_BUSY)
        {
          g_error_free (error);
          error = g_error_new (G_IO_ERROR,
                               G_IO_ERROR_BUSY,
                               _("Failed to eject medium; one or more volumes on the medium are busy."));
        }

      if (mount_operation != NULL)
        gvfs_udisks2_unmount_notify_stop (mount_operation, error != NULL);

      /* unmount failed; need to fail the whole eject operation */
      g_task_return_error (data->task, error);
      free_unmount_and_lock_op (data);
    }
  else
    {
      /* move on to the next mount.. */
      unmount_and_lock_do (data);
    }
  g_object_unref (mount);
}

static void
lock_cb (GObject       *source_object,
         GAsyncResult  *res,
         gpointer       user_data)
{
  UDisksEncrypted *encrypted = UDISKS_ENCRYPTED (source_object);
  UnmountAndLockOp *data = user_data;
  GError *error = NULL;

  if (!udisks_encrypted_call_lock_finish (encrypted,
                                          res,
                                          &error))
    {
      /* lock failed; need to fail the whole eject operation */
      g_task_return_error (data->task, error);
      free_unmount_and_lock_op (data);
    }
  else
    {
      /* move on to the next encrypted.. */
      unmount_and_lock_do (data);
    }
  g_object_unref (encrypted);
}

static void
unmount_and_lock (GVfsUDisks2Drive    *drive,
                  GMountUnmountFlags   flags,
                  GTask               *task,
                  UnmountAndLockFunc   on_all_unmounted)
{
  GMount *mount;
  UnmountAndLockOp *data;
  GList *l;

  data = g_new0 (UnmountAndLockOp, 1);
  data->flags = flags;
  data->on_all_unmounted = on_all_unmounted;
  data->task = task;

  for (l = drive->volumes; l != NULL; l = l->next)
    {
      GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (l->data);
      mount = g_volume_get_mount (G_VOLUME (volume));
      if (mount != NULL)
        {
          if (g_mount_can_unmount (mount))
            data->pending_mounts = g_list_prepend (data->pending_mounts, g_object_ref (mount));
        }
      else
        {
          UDisksBlock *block;

          block = gvfs_udisks2_volume_get_block (volume);
          if (block != NULL)
            {
              UDisksEncrypted *encrypted;
              GDBusObject *object;

              object = g_dbus_interface_get_object (G_DBUS_INTERFACE (block));
              if (object == NULL)
                {
                  g_task_return_new_error (data->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                           "No object for D-Bus interface");
                  free_unmount_and_lock_op (data);
                  goto out;
                }

              encrypted = udisks_object_get_encrypted (UDISKS_OBJECT (object));
              if (encrypted != NULL)
                {
                  UDisksBlock *cleartext_block;
                  UDisksClient *client;

                  client = gvfs_udisks2_volume_monitor_get_udisks_client (drive->monitor);
                  cleartext_block = udisks_client_get_cleartext_block (client, block);
                  if (cleartext_block != NULL)
                    {
                      data->pending_encrypted = g_list_prepend (data->pending_encrypted,
                                                                g_object_ref (encrypted));
                      g_object_unref (cleartext_block);
                    }

                  g_object_unref (encrypted);
                }
            }
        }
    }

  unmount_and_lock_do (data);

 out:
  ;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
eject_cb (GObject      *source_object,
          GAsyncResult *res,
          gpointer      user_data)
{
  GTask *task = G_TASK (user_data);
  GMountOperation *mount_operation = g_task_get_task_data (task);
  GError *error;

  error = NULL;
  if (!udisks_drive_call_eject_finish (UDISKS_DRIVE (source_object), res, &error))
    gvfs_udisks2_utils_udisks_error_to_gio_error (error);

  if (mount_operation != NULL)
    {
      /* If we fail send an ::aborted signal to make any notification go away */
      if (error != NULL)
        g_signal_emit_by_name (mount_operation, "aborted");

      gvfs_udisks2_unmount_notify_stop (mount_operation, error != NULL);
    }

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);

  g_object_unref (task);
}

static void
gvfs_udisks2_drive_eject_on_all_unmounted (GTask *task)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (g_task_get_source_object (task));
  GMountOperation *mount_operation = g_task_get_task_data (task);
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (mount_operation == NULL)
    {
      g_variant_builder_add (&builder,
                             "{sv}",
                             "auth.no_user_interaction", g_variant_new_boolean (TRUE));
    }
  udisks_drive_call_eject (drive->udisks_drive,
                           g_variant_builder_end (&builder),
                           g_task_get_cancellable (task),
                           eject_cb,
                           task);
}

static void
gvfs_udisks2_drive_eject_with_operation (GDrive              *_drive,
                                         GMountUnmountFlags   flags,
                                         GMountOperation     *mount_operation,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  GTask *task;

  task = g_task_new (drive, cancellable, callback, user_data);
  g_task_set_source_tag (task, gvfs_udisks2_drive_eject_with_operation);

  /* This information is needed in GVfsDdisks2Volume when apps have
   * open files on the device ... we need to know if the button should
   * be "Unmount Anyway", "Eject Anyway" or "Power Off Anyway"
   */
  if (mount_operation != NULL)
    {
      g_object_set_data (G_OBJECT (mount_operation), "x-udisks2-is-eject", GINT_TO_POINTER (1));
      gvfs_udisks2_unmount_notify_start (mount_operation, NULL, _drive);
      g_task_set_task_data (task, g_object_ref (mount_operation), g_object_unref);
    }

  /* first we need to go through all the volumes and unmount their assoicated mounts (if any) */
  unmount_and_lock (drive,
                    flags,
                    task,
                    gvfs_udisks2_drive_eject_on_all_unmounted);
}

static gboolean
gvfs_udisks2_drive_eject_with_operation_finish (GDrive        *drive,
                                                GAsyncResult  *result,
                                                GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, drive), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, gvfs_udisks2_drive_eject_with_operation), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gvfs_udisks2_drive_eject (GDrive              *drive,
                          GMountUnmountFlags   flags,
                          GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
  gvfs_udisks2_drive_eject_with_operation (drive, flags, NULL, cancellable, callback, user_data);
}

static gboolean
gvfs_udisks2_drive_eject_finish (GDrive        *drive,
                                 GAsyncResult  *result,
                                 GError       **error)
{
  return gvfs_udisks2_drive_eject_with_operation_finish (drive, result, error);
}

/* ---------------------------------------------------------------------------------------------------- */

#if UDISKS_CHECK_VERSION(2,0,90)

static void
power_off_cb (GObject      *source_object,
              GAsyncResult *res,
              gpointer      user_data)
{
  GTask *task = G_TASK (user_data);
  GMountOperation *mount_operation = g_task_get_task_data (task);
  GError *error;

  error = NULL;
  if (!udisks_drive_call_power_off_finish (UDISKS_DRIVE (source_object), res, &error))
    gvfs_udisks2_utils_udisks_error_to_gio_error (error);

  if (mount_operation != NULL)
    {
      /* If we fail send an ::aborted signal to make any notification go away */
      if (error != NULL)
        g_signal_emit_by_name (mount_operation, "aborted");

      gvfs_udisks2_unmount_notify_stop (mount_operation, error != NULL);
    }

  if (error)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);

  g_object_unref (task);
}

static void
gvfs_udisks2_drive_stop_on_all_unmounted (GTask *task)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (g_task_get_source_object (task));
  GMountOperation *mount_operation = g_task_get_task_data (task);
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (mount_operation == NULL)
    {
      g_variant_builder_add (&builder,
                             "{sv}",
                             "auth.no_user_interaction", g_variant_new_boolean (TRUE));
    }
  udisks_drive_call_power_off (drive->udisks_drive,
                               g_variant_builder_end (&builder),
                               g_task_get_cancellable (task),
                               power_off_cb,
                               task);
}

static void
gvfs_udisks2_drive_stop (GDrive              *_drive,
                         GMountUnmountFlags   flags,
                         GMountOperation     *mount_operation,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  GTask *task;

  task = g_task_new (drive, cancellable, callback, user_data);
  g_task_set_source_tag (task, gvfs_udisks2_drive_stop);

  /* This information is needed in GVfsDdisks2Volume when apps have
   * open files on the device ... we need to know if the button should
   * be "Unmount Anyway", "Eject Anyway" or "Power Off Anyway"
   */
  if (mount_operation != NULL)
    {
      g_object_set_data (G_OBJECT (mount_operation), "x-udisks2-is-stop", GINT_TO_POINTER (1));
      gvfs_udisks2_unmount_notify_start (mount_operation, NULL, _drive);
      g_task_set_task_data (task, g_object_ref (mount_operation), g_object_unref);
    }

  /* first we need to go through all the volumes and unmount their assoicated mounts (if any) */
  unmount_and_lock (drive,
                    flags,
                    task,
                    gvfs_udisks2_drive_stop_on_all_unmounted);
}

static gboolean
gvfs_udisks2_drive_stop_finish (GDrive        *drive,
                                GAsyncResult  *result,
                                GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, drive), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, gvfs_udisks2_drive_stop), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

#endif /* UDISKS_CHECK_VERSION(2,0,90) */

/* ---------------------------------------------------------------------------------------------------- */

static const gchar *
gvfs_udisks2_drive_get_sort_key (GDrive *_drive)
{
  GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (_drive);
  return drive->sort_key;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
gvfs_udisks2_drive_drive_iface_init (GDriveIface *iface)
{
  iface->get_name = gvfs_udisks2_drive_get_name;
  iface->get_icon = gvfs_udisks2_drive_get_icon;
  iface->get_symbolic_icon = gvfs_udisks2_drive_get_symbolic_icon;
  iface->has_volumes = gvfs_udisks2_drive_has_volumes;
  iface->get_volumes = gvfs_udisks2_drive_get_volumes;
  iface->is_removable = gvfs_udisks2_drive_is_removable;
  iface->is_media_removable = gvfs_udisks2_drive_is_media_removable;
  iface->has_media = gvfs_udisks2_drive_has_media;
  iface->is_media_check_automatic = gvfs_udisks2_drive_is_media_check_automatic;
  iface->can_eject = gvfs_udisks2_drive_can_eject;
  iface->can_poll_for_media = gvfs_udisks2_drive_can_poll_for_media;
  iface->get_identifier = gvfs_udisks2_drive_get_identifier;
  iface->enumerate_identifiers = gvfs_udisks2_drive_enumerate_identifiers;
  iface->get_start_stop_type = gvfs_udisks2_drive_get_start_stop_type;
  iface->can_start = gvfs_udisks2_drive_can_start;
  iface->can_start_degraded = gvfs_udisks2_drive_can_start_degraded;
  iface->can_stop = gvfs_udisks2_drive_can_stop;
  iface->eject = gvfs_udisks2_drive_eject;
  iface->eject_finish = gvfs_udisks2_drive_eject_finish;
  iface->eject_with_operation = gvfs_udisks2_drive_eject_with_operation;
  iface->eject_with_operation_finish = gvfs_udisks2_drive_eject_with_operation_finish;
  iface->get_sort_key = gvfs_udisks2_drive_get_sort_key;
#if 0
  iface->poll_for_media = gvfs_udisks2_drive_poll_for_media;
  iface->poll_for_media_finish = gvfs_udisks2_drive_poll_for_media_finish;
  iface->start = gvfs_udisks2_drive_start;
  iface->start_finish = gvfs_udisks2_drive_start_finish;
#endif

#if UDISKS_CHECK_VERSION(2,0,90)
  iface->stop = gvfs_udisks2_drive_stop;
  iface->stop_finish = gvfs_udisks2_drive_stop_finish;
#endif
}

UDisksDrive *
gvfs_udisks2_drive_get_udisks_drive (GVfsUDisks2Drive *drive)
{
  return drive->udisks_drive;
}
