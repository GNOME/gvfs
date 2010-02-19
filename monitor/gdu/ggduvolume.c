/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2009 Red Hat, Inc.
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

#include "ggdudrive.h"
#include "ggduvolume.h"
#include "ggdumount.h"

/* for BUFSIZ */
#include <stdio.h>

typedef struct MountOpData MountOpData;

static void cancel_pending_mount_op (MountOpData *data);

static void g_gdu_volume_mount_unix_mount_point (GGduVolume          *volume,
                                                 GMountMountFlags     flags,
                                                 GMountOperation     *mount_operation,
                                                 GCancellable        *cancellable,
                                                 GAsyncReadyCallback  callback,
                                                 gpointer             user_data);

struct _GGduVolume
{
  GObject parent;

  GVolumeMonitor *volume_monitor; /* owned by volume monitor */
  GGduMount      *mount;          /* owned by volume monitor */
  GGduDrive      *drive;          /* owned by volume monitor */

  /* only set if constructed via new() */
  GduVolume *gdu_volume;

  /* only set if constructed via new_for_unix_mount_point() */
  GUnixMountPoint *unix_mount_point;

  /* if the volume is encrypted, this is != NULL when unlocked */
  GduVolume *cleartext_gdu_volume;

  /* If a mount operation is in progress, then pending_mount_op is != NULL. This
   * is used to cancel the operation to make possible authentication dialogs go
   * away.
   */
  MountOpData *pending_mount_op;

  /* the following members need to be set upon construction, see constructors and update_volume() */
  GIcon *icon;
  GFile *activation_root;
  gchar *name;
  gchar *device_file;
  dev_t dev;
  gchar *uuid;
  gboolean can_mount;
  gboolean should_automount;
};

static void g_gdu_volume_volume_iface_init (GVolumeIface *iface);

G_DEFINE_TYPE_EXTENDED (GGduVolume, g_gdu_volume, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME,
                                               g_gdu_volume_volume_iface_init))

static void gdu_volume_changed (GduPresentable *presentable,
                                GGduVolume     *volume);
static void gdu_volume_job_changed (GduPresentable *presentable,
                                    GGduVolume     *volume);

static void gdu_cleartext_volume_removed (GduPresentable *presentable,
                                          GGduVolume     *volume);
static void gdu_cleartext_volume_changed (GduPresentable *presentable,
                                          GGduVolume     *volume);
static void gdu_cleartext_volume_job_changed (GduPresentable *presentable,
                                              GGduVolume     *volume);

static void mount_with_mount_operation (MountOpData *data);

static void
g_gdu_volume_finalize (GObject *object)
{
  GGduVolume *volume;

  volume = G_GDU_VOLUME (object);

  if (volume->mount != NULL)
    g_gdu_mount_unset_volume (volume->mount, volume);

  if (volume->drive != NULL)
    g_gdu_drive_unset_volume (volume->drive, volume);

  if (volume->gdu_volume != NULL)
    {
      g_signal_handlers_disconnect_by_func (volume->gdu_volume, gdu_volume_changed, volume);
      g_signal_handlers_disconnect_by_func (volume->gdu_volume, gdu_volume_job_changed, volume);
      g_object_unref (volume->gdu_volume);
    }

  if (volume->unix_mount_point != NULL)
    {
      g_unix_mount_point_free (volume->unix_mount_point);
    }

  if (volume->cleartext_gdu_volume != NULL)
    {
      g_signal_handlers_disconnect_by_func (volume->cleartext_gdu_volume, gdu_cleartext_volume_removed, volume);
      g_signal_handlers_disconnect_by_func (volume->cleartext_gdu_volume, gdu_cleartext_volume_changed, volume);
      g_signal_handlers_disconnect_by_func (volume->cleartext_gdu_volume, gdu_cleartext_volume_job_changed, volume);
      g_object_unref (volume->cleartext_gdu_volume);
    }

  if (volume->icon != NULL)
    g_object_unref (volume->icon);
  if (volume->activation_root != NULL)
    g_object_unref (volume->activation_root);

  g_free (volume->name);
  g_free (volume->device_file);
  g_free (volume->uuid);

  if (G_OBJECT_CLASS (g_gdu_volume_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_gdu_volume_parent_class)->finalize) (object);
}

static void
g_gdu_volume_class_init (GGduVolumeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_gdu_volume_finalize;
}

static void
g_gdu_volume_init (GGduVolume *gdu_volume)
{
}

static void
emit_changed (GGduVolume *volume)
{
  g_signal_emit_by_name (volume, "changed");
  g_signal_emit_by_name (volume->volume_monitor, "volume_changed", volume);
}

static gboolean
update_volume (GGduVolume *volume)
{
  GduDevice *device;
  GduPool *pool = NULL;
  time_t now;
  gboolean changed;
  gboolean old_can_mount;
  gboolean old_should_automount;
  gchar *old_name;
  gchar *old_device_file;
  dev_t old_dev;
  GIcon *old_icon;
  gboolean keep_cleartext_volume;

  /* save old values */
  old_can_mount = volume->can_mount;
  old_should_automount = volume->should_automount;
  old_name = g_strdup (volume->name);
  old_device_file = g_strdup (volume->device_file);
  old_dev = volume->dev;
  old_icon = volume->icon != NULL ? g_object_ref (volume->icon) : NULL;

  /* ---------------------------------------------------------------------------------------------------- */

  /* if the volume is a fstab mount point, get the data from there */
  if (volume->unix_mount_point != NULL)
    {
      struct stat_buf;

      volume->can_mount = TRUE;
      volume->should_automount = FALSE;

      g_free (volume->device_file);
      volume->device_file = g_strdup (g_unix_mount_point_get_device_path (volume->unix_mount_point));
      volume->dev = 0;

      if (volume->icon != NULL)
        g_object_unref (volume->icon);
      if (g_strcmp0 (g_unix_mount_point_get_fs_type (volume->unix_mount_point), "nfs") == 0)
        volume->icon = g_themed_icon_new_with_default_fallbacks ("folder-remote");
      else
        volume->icon = g_unix_mount_point_guess_icon (volume->unix_mount_point);

      g_free (volume->name);
      volume->name = g_unix_mount_point_guess_name (volume->unix_mount_point);

      //volume->can_eject = g_unix_mount_point_guess_can_eject (volume->unix_mount_point);

      goto update_done;
    }

  /* in with the new */
  device = gdu_presentable_get_device (GDU_PRESENTABLE (volume->gdu_volume));
  if (device != NULL)
    pool = gdu_device_get_pool (device);

  keep_cleartext_volume = FALSE;
  if (device != NULL && gdu_device_is_luks (device))
    {
      const gchar *holder_objpath;

      holder_objpath = gdu_device_luks_get_holder (device);
      if (holder_objpath != NULL && g_strcmp0 (holder_objpath, "/") != 0)
        {
          GduDevice *cleartext_device;

          cleartext_device = gdu_pool_get_by_object_path (pool, holder_objpath);
          if (cleartext_device != NULL)
            {
              GduVolume *cleartext_gdu_volume;

              cleartext_gdu_volume = GDU_VOLUME (gdu_pool_get_volume_by_device (pool, cleartext_device));
              if (cleartext_gdu_volume != volume->cleartext_gdu_volume)
                {
                  if (volume->cleartext_gdu_volume != NULL)
                    {
                      g_signal_handlers_disconnect_by_func (volume->cleartext_gdu_volume, gdu_cleartext_volume_removed, volume);
                      g_signal_handlers_disconnect_by_func (volume->cleartext_gdu_volume, gdu_cleartext_volume_changed, volume);
                      g_signal_handlers_disconnect_by_func (volume->cleartext_gdu_volume, gdu_cleartext_volume_job_changed, volume);
                      g_object_unref (volume->cleartext_gdu_volume);
                    }

                  volume->cleartext_gdu_volume = g_object_ref (cleartext_gdu_volume);
                  g_signal_connect (volume->cleartext_gdu_volume, "removed", G_CALLBACK (gdu_cleartext_volume_removed), volume);
                  g_signal_connect (volume->cleartext_gdu_volume, "changed", G_CALLBACK (gdu_cleartext_volume_changed), volume);
                  g_signal_connect (volume->cleartext_gdu_volume, "job-changed", G_CALLBACK (gdu_cleartext_volume_job_changed), volume);
                }
              g_object_unref (cleartext_gdu_volume);

              g_object_unref (cleartext_device);

              keep_cleartext_volume = TRUE;
            }
        }
    }

  if (!keep_cleartext_volume)
    {
      if (volume->cleartext_gdu_volume != NULL)
        {
          g_signal_handlers_disconnect_by_func (volume->cleartext_gdu_volume, gdu_cleartext_volume_removed, volume);
          g_signal_handlers_disconnect_by_func (volume->cleartext_gdu_volume, gdu_cleartext_volume_changed, volume);
          g_signal_handlers_disconnect_by_func (volume->cleartext_gdu_volume, gdu_cleartext_volume_job_changed, volume);
          g_object_unref (volume->cleartext_gdu_volume);
          volume->cleartext_gdu_volume = NULL;
        }
    }


  /* Use data from cleartext LUKS volume if it is unlocked */
  if (volume->cleartext_gdu_volume != NULL)
    {
      GduDevice *luks_cleartext_volume_device;

      luks_cleartext_volume_device = gdu_presentable_get_device (GDU_PRESENTABLE (volume->cleartext_gdu_volume));

      if (volume->icon != NULL)
        g_object_unref (volume->icon);
      volume->icon = gdu_presentable_get_icon (GDU_PRESENTABLE (volume->cleartext_gdu_volume));

      g_free (volume->name);
      volume->name = gdu_presentable_get_name (GDU_PRESENTABLE (volume->cleartext_gdu_volume));

      g_free (volume->device_file);
      if (luks_cleartext_volume_device != NULL)
        {
          volume->device_file = g_strdup (gdu_device_get_device_file (luks_cleartext_volume_device));
          volume->dev = gdu_device_get_dev (luks_cleartext_volume_device);
        }
      else
        {
          volume->device_file = NULL;
          volume->dev = 0;
        }

      volume->can_mount = TRUE;

      volume->should_automount = FALSE;

      if (luks_cleartext_volume_device != NULL)
        g_object_unref (luks_cleartext_volume_device);
    }
  else
    {
      gchar *activation_uri;

      if (volume->icon != NULL)
        g_object_unref (volume->icon);
      volume->icon = gdu_presentable_get_icon (GDU_PRESENTABLE (volume->gdu_volume));

      g_free (volume->name);
      if (_is_pc_floppy_drive (device))
        volume->name = g_strdup (_("Floppy Disk"));
      else
        volume->name = gdu_presentable_get_name (GDU_PRESENTABLE (volume->gdu_volume));

      /* special case the name and icon for audio discs */
      activation_uri = volume->activation_root != NULL ? g_file_get_uri (volume->activation_root) : NULL;
      if (activation_uri != NULL && g_str_has_prefix (activation_uri, "cdda://"))
        {
          if (volume->icon != NULL)
            g_object_unref (volume->icon);
          volume->icon = g_themed_icon_new_with_default_fallbacks ("media-optical-audio");
          g_free (volume->name);
          volume->name = g_strdup (_("Audio Disc"));
        }

      g_free (volume->device_file);
      if (device != NULL)
        {
          volume->device_file = g_strdup (gdu_device_get_device_file (device));
          volume->dev = gdu_device_get_dev (device);
        }
      else
        {
          volume->device_file = NULL;
          volume->dev = 0;
        }

      volume->can_mount = TRUE;

      /* Only automount filesystems from drives of known types/interconnects:
       *
       *  - USB
       *  - Firewire
       *  - sdio
       *  - optical discs
       *
       * The mantra here is "be careful" - we really don't want to
       * automount fs'es from all devices in a SAN etc - We REALLY
       * need to be CAREFUL here.
       *
       * Sidebar: Actually, a surprisingly large number of admins like
       *          to log into GNOME as root (thus having all polkit
       *          authorizations) and if weren't careful we'd
       *          automount all mountable devices from the box. See
       *          the enterprise distro bug trackers for details.
       */
      volume->should_automount = FALSE;
      if (volume->drive != NULL)
        {
          GduPresentable *drive_presentable;
          drive_presentable = g_gdu_drive_get_presentable (volume->drive);
          if (drive_presentable != NULL)
            {
              GduDevice *drive_device;
              drive_device = gdu_presentable_get_device (drive_presentable);
              if (drive_device != NULL)
                {
                  if (gdu_device_is_drive (drive_device))
                    {
                      const gchar *connection_interface;

                      connection_interface = gdu_device_drive_get_connection_interface (drive_device);

                      if (g_strcmp0 (connection_interface, "usb") == 0 ||
                          g_strcmp0 (connection_interface, "firewire") == 0 ||
                          g_strcmp0 (connection_interface, "sdio") == 0 ||
                          gdu_device_is_optical_disc (drive_device))
                        {
                          volume->should_automount = TRUE;
                        }
                    }
                  g_object_unref (drive_device);
                }
            }

          /* If a volume (partition) appear _much later_ than when media was inserted it
           * can only be because the media was repartitioned. We don't want to automount
           * such volumes.
           */
          now = time (NULL);
          if (now - g_gdu_drive_get_time_of_last_media_insertion (volume->drive) > 5)
            volume->should_automount = FALSE;
        }

      /* Respect the presentation hint for whether the volume should be automounted - normally
       * nopolicy is only FALSE for "physical" devices - e.g. only "physical" devices will
       * be set to be automounted.
       */
      if (device != NULL && gdu_device_get_presentation_nopolicy (device))
        volume->should_automount = FALSE;

      g_free (activation_uri);
    }

  if (pool != NULL)
    g_object_unref (pool);
  if (device != NULL)
    g_object_unref (device);

  /* ---------------------------------------------------------------------------------------------------- */

 update_done:

  /* compute whether something changed */
  changed = !((old_can_mount == volume->can_mount) &&
              (old_should_automount == volume->should_automount) &&
              (g_strcmp0 (old_name, volume->name) == 0) &&
              (g_strcmp0 (old_device_file, volume->device_file) == 0) &&
              (old_dev == volume->dev) &&
              g_icon_equal (old_icon, volume->icon)
              );

  /* free old values */
  g_free (old_name);
  g_free (old_device_file);
  if (old_icon != NULL)
    g_object_unref (old_icon);

  /*g_debug ("in update_volume(), changed=%d", changed);*/

  return changed;
}

static void
gdu_volume_changed (GduPresentable *presentable,
                    GGduVolume     *volume)
{
  /*g_debug ("volume: presentable_changed: %p: %s", volume, gdu_presentable_get_id (GDU_PRESENTABLE (presentable)));*/
  if (update_volume (volume))
    emit_changed (volume);
}

static void
gdu_volume_job_changed (GduPresentable *presentable,
                        GGduVolume     *volume)
{
  /*g_debug ("volume: presentable_job_changed %p: %s", volume, gdu_presentable_get_id (GDU_PRESENTABLE (presentable)));*/
  if (update_volume (volume))
    emit_changed (volume);
}

static void
gdu_cleartext_volume_removed (GduPresentable *presentable,
                              GGduVolume     *volume)
{
  /*g_debug ("cleartext volume: presentable_removed: %p: %s", volume, gdu_presentable_get_id (GDU_PRESENTABLE (presentable)));*/
  if (update_volume (volume))
    emit_changed (volume);
}

static void
gdu_cleartext_volume_changed (GduPresentable *presentable,
                              GGduVolume     *volume)
{
  /*g_debug ("cleartext volume: presentable_changed: %p: %s", volume, gdu_presentable_get_id (GDU_PRESENTABLE (presentable)));*/
  if (update_volume (volume))
    emit_changed (volume);
}

static void
gdu_cleartext_volume_job_changed (GduPresentable *presentable,
                                  GGduVolume     *volume)
{
  /*g_debug ("cleartext volume: presentable_job_changed %p: %s", volume, gdu_presentable_get_id (GDU_PRESENTABLE (presentable)));*/
  if (update_volume (volume))
    emit_changed (volume);
}

GGduVolume *
g_gdu_volume_new_for_unix_mount_point (GVolumeMonitor   *volume_monitor,
                                       GUnixMountPoint  *unix_mount_point)
{
  GGduVolume *volume;

  volume = g_object_new (G_TYPE_GDU_VOLUME, NULL);
  volume->volume_monitor = volume_monitor;
  g_object_add_weak_pointer (G_OBJECT (volume_monitor), (gpointer) &(volume->volume_monitor));

  volume->unix_mount_point = unix_mount_point;

  update_volume (volume);

  return volume;
}

GGduVolume *
g_gdu_volume_new (GVolumeMonitor   *volume_monitor,
                  GduVolume        *gdu_volume,
                  GGduDrive        *drive,
                  GFile            *activation_root)
{
  GGduVolume *volume;

  volume = g_object_new (G_TYPE_GDU_VOLUME, NULL);
  volume->volume_monitor = volume_monitor;
  g_object_add_weak_pointer (G_OBJECT (volume_monitor), (gpointer) &(volume->volume_monitor));

  volume->gdu_volume = g_object_ref (gdu_volume);
  volume->activation_root = activation_root != NULL ? g_object_ref (activation_root) : NULL;

  g_signal_connect (volume->gdu_volume, "changed", G_CALLBACK (gdu_volume_changed), volume);
  g_signal_connect (volume->gdu_volume, "job-changed", G_CALLBACK (gdu_volume_job_changed), volume);

  volume->drive = drive;
  if (drive != NULL)
    g_gdu_drive_set_volume (drive, volume);

  update_volume (volume);

  return volume;
}

void
g_gdu_volume_removed (GGduVolume *volume)
{
  if (volume->pending_mount_op != NULL)
    cancel_pending_mount_op (volume->pending_mount_op);

  if (volume->mount != NULL)
    {
      g_gdu_mount_unset_volume (volume->mount, volume);
      volume->mount = NULL;
    }

  if (volume->drive != NULL)
    {
      g_gdu_drive_unset_volume (volume->drive, volume);
      volume->drive = NULL;
    }
}

void
g_gdu_volume_set_mount (GGduVolume  *volume,
                        GGduMount *mount)
{
  if (volume->mount != mount)
    {

      if (volume->mount != NULL)
        g_gdu_mount_unset_volume (volume->mount, volume);

      volume->mount = mount;

      emit_changed (volume);
    }
}

void
g_gdu_volume_unset_mount (GGduVolume  *volume,
                          GGduMount *mount)
{
  if (volume->mount == mount)
    {
      volume->mount = NULL;
      emit_changed (volume);
    }
}

void
g_gdu_volume_set_drive (GGduVolume  *volume,
                        GGduDrive *drive)
{
  if (volume->drive != drive)
    {
      if (volume->drive != NULL)
        g_gdu_drive_unset_volume (volume->drive, volume);

      volume->drive = drive;

      emit_changed (volume);
    }
}

void
g_gdu_volume_unset_drive (GGduVolume  *volume,
                          GGduDrive *drive)
{
  if (volume->drive == drive)
    {
      volume->drive = NULL;
      emit_changed (volume);
    }
}

static GIcon *
g_gdu_volume_get_icon (GVolume *_volume)
{
  GGduVolume *volume = G_GDU_VOLUME (_volume);
  return volume->icon != NULL ? g_object_ref (volume->icon) : NULL;
}

static char *
g_gdu_volume_get_name (GVolume *_volume)
{
  GGduVolume *volume = G_GDU_VOLUME (_volume);
  return g_strdup (volume->name);
}

static char *
g_gdu_volume_get_uuid (GVolume *_volume)
{
  GGduVolume *volume = G_GDU_VOLUME (_volume);
  return g_strdup (volume->uuid);
}

static gboolean
g_gdu_volume_can_mount (GVolume *_volume)
{
  GGduVolume *volume = G_GDU_VOLUME (_volume);
  return volume->can_mount;
}

static gboolean
g_gdu_volume_can_eject (GVolume *_volume)
{
  GGduVolume *volume = G_GDU_VOLUME (_volume);
  gboolean can_eject;

  can_eject = FALSE;
  if (volume->drive != NULL)
    can_eject = g_drive_can_eject (G_DRIVE (volume->drive));

  return can_eject;
}

static gboolean
g_gdu_volume_should_automount (GVolume *_volume)
{
  GGduVolume *volume = G_GDU_VOLUME (_volume);
  return volume->should_automount;
}

static GDrive *
g_gdu_volume_get_drive (GVolume *volume)
{
  GGduVolume *gdu_volume = G_GDU_VOLUME (volume);
  GDrive *drive;

  drive = NULL;
  if (gdu_volume->drive != NULL)
    drive = g_object_ref (gdu_volume->drive);

  return drive;
}

static GMount *
g_gdu_volume_get_mount (GVolume *volume)
{
  GGduVolume *gdu_volume = G_GDU_VOLUME (volume);
  GMount *mount;

  mount = NULL;
  if (gdu_volume->mount != NULL)
    mount = g_object_ref (gdu_volume->mount);

  return mount;
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar **
get_mount_options (GduDevice *device,
                   gboolean   allow_user_interaction)
{
  GPtrArray *p;

  p = g_ptr_array_new ();

  /* one day we might read this from user settings */
  if (g_strcmp0 (gdu_device_id_get_usage (device), "filesystem") == 0 &&
      g_strcmp0 (gdu_device_id_get_type (device), "vfat") == 0)
    {
      g_ptr_array_add (p, g_strdup ("flush"));
    }

  if (!allow_user_interaction)
    {
      g_ptr_array_add (p, g_strdup ("auth_no_user_interaction"));
    }

  g_ptr_array_add (p, NULL);

  return (gchar **) g_ptr_array_free (p, FALSE);
}

/* ---------------------------------------------------------------------------------------------------- */

struct MountOpData
{
  GGduVolume *volume;
  GduDevice *device_to_mount;
  GSimpleAsyncResult *simple;
  GCancellable *cancellable;
  gulong cancelled_handler_id;

  GMountOperation *mount_operation;
  gulong mount_operation_reply_handler_id;

  gboolean is_cancelled;
};

static void
mount_op_data_unref (MountOpData *data)
{
  g_object_unref (data->volume);
  if (data->device_to_mount != NULL)
    g_object_unref (data->device_to_mount);
  g_object_unref (data->simple);
  if (data->cancelled_handler_id != 0)
    g_signal_handler_disconnect (data->cancellable, data->cancelled_handler_id);
  if (data->cancellable != NULL)
    g_object_unref (data->cancellable);
  if (data->mount_operation_reply_handler_id != 0)
    g_signal_handler_disconnect (data->mount_operation, data->mount_operation_reply_handler_id);
  if (data->mount_operation != NULL)
    g_object_unref (data->mount_operation);
  g_free (data);
}

static void
cancel_pending_mount_op (MountOpData *data)
{
  /* we are no longer pending */
  data->volume->pending_mount_op = NULL;

  data->is_cancelled = TRUE;

  /* send an ::aborted signal to make the dialog go away */
  if (data->mount_operation != NULL)
    g_signal_emit_by_name (data->mount_operation, "aborted");

  /* complete the operation (sends reply to caller) */
  g_simple_async_result_set_error (data->simple,
                                   G_IO_ERROR,
                                   G_IO_ERROR_FAILED_HANDLED,
                                   "Operation was cancelled");
  g_simple_async_result_complete (data->simple);
}

static void
mount_cb (GduDevice *device,
          gchar     *mount_point,
          GError    *error,
          gpointer   user_data)
{
  MountOpData *data = user_data;

  /* if we've already aborted due to device removal / cancellation, just bail out */
  if (data->is_cancelled)
    goto bailout;

  if (error != NULL)
    {
      /* be quiet if the DeviceKit-disks daemon is inhibited */
      if (error->code == GDU_ERROR_INHIBITED)
        {
          error->domain = G_IO_ERROR;
          error->code = G_IO_ERROR_FAILED_HANDLED;
        }
      g_simple_async_result_set_from_error (data->simple, error);
      g_error_free (error);
    }
  else
    {
      g_free (mount_point);
    }

  g_simple_async_result_complete (data->simple);

 bailout:
  data->volume->pending_mount_op = NULL;
  mount_op_data_unref (data);

}

static void
mount_cleartext_device (MountOpData *data,
                        const gchar *object_path_of_cleartext_device)
{
  GduPool *pool;

  /* if we've already aborted due to device removal / cancellation, just bail out */
  if (data->is_cancelled)
    {
      mount_op_data_unref (data);
      goto bailout;
    }

  pool = gdu_presentable_get_pool (GDU_PRESENTABLE (data->volume->gdu_volume));

  data->device_to_mount = gdu_pool_get_by_object_path (pool, object_path_of_cleartext_device);
  if (data->device_to_mount == NULL)
    {
      g_simple_async_result_set_error (data->simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_FAILED,
                                       "Successfully unlocked encrypted volume but cleartext device does not exist");
      g_simple_async_result_complete (data->simple);
      data->volume->pending_mount_op = NULL;
      mount_op_data_unref (data);
    }
  else
    {
      gchar **mount_options;
      mount_options = get_mount_options (data->device_to_mount, data->mount_operation != NULL);
      gdu_device_op_filesystem_mount (data->device_to_mount, mount_options, mount_cb, data);
      g_strfreev (mount_options);
    }

  g_object_unref (pool);

 bailout:
  ;
}

static void
unlock_from_keyring_cb (GduDevice  *device,
                        char       *object_path_of_cleartext_device,
                        GError     *error,
                        gpointer    user_data)
{
  MountOpData *data = user_data;

  /* if we've already aborted due to device removal / cancellation, just bail out */
  if (data->is_cancelled)
    {
      mount_op_data_unref (data);
      goto bailout;
    }

  if (error != NULL)
    {
      /*g_debug ("keyring password didn't work: %s", error->message);*/

      /* The password we retrieved from the keyring didn't work. So go ahead and prompt
       * the user.
       */
      mount_with_mount_operation (data);

      g_error_free (error);
    }
  else
    {
      mount_cleartext_device (data, object_path_of_cleartext_device);
      g_free (object_path_of_cleartext_device);
    }

 bailout:
  ;
}

static void
unlock_cb (GduDevice  *device,
           gchar      *object_path_of_cleartext_device,
           GError     *error,
           gpointer    user_data)
{
  MountOpData *data = user_data;

  /* if we've already aborted due to device removal / cancellation, just bail out */
  if (data->is_cancelled)
    {
      mount_op_data_unref (data);
      goto bailout;
    }

  if (error != NULL)
    {
      /* be quiet if the daemon is inhibited */
      if (error->code == GDU_ERROR_INHIBITED)
        {
          error->domain = G_IO_ERROR;
          error->code = G_IO_ERROR_FAILED_HANDLED;
        }
      g_simple_async_result_set_from_error (data->simple, error);
      g_error_free (error);
      g_simple_async_result_complete (data->simple);
      data->volume->pending_mount_op = NULL;
      mount_op_data_unref (data);
    }
  else
    {
      GPasswordSave password_save;
      const gchar *password;

      password_save = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (device), "password-save"));
      password = g_object_get_data (G_OBJECT (device), "password");

      if (password != NULL)
        {
          switch (password_save)
            {
            case G_PASSWORD_SAVE_FOR_SESSION:
              gdu_util_save_secret (device, password, TRUE);
              break;

            case G_PASSWORD_SAVE_PERMANENTLY:
              gdu_util_save_secret (device, password, FALSE);
              break;

            default:
              /* do nothing */
              break;
            }
        }

      /* now we have a cleartext device; update the GVolume details to show that */
      if (update_volume (data->volume))
        emit_changed (data->volume);

      mount_cleartext_device (data, object_path_of_cleartext_device);
      g_free (object_path_of_cleartext_device);
    }

 bailout:

  /* scrub the password */
  g_object_set_data (G_OBJECT (device), "password-save", NULL);
  g_object_set_data (G_OBJECT (device), "password", NULL);
}

static void
scrub_n_free_string (char *password)
{
  memset (password, '\0', strlen (password));
  g_free (password);
}

static void
mount_operation_reply (GMountOperation       *mount_operation,
                       GMountOperationResult result,
                       gpointer              user_data)
{
  MountOpData *data = user_data;
  GduDevice *device;
  const gchar *password;

  /* if we've already aborted due to device removal, just bail out */
  if (data->is_cancelled)
    {
      mount_op_data_unref (data);
      goto out;
    }

  /* we got what we wanted; don't listen to any other signals from the mount operation */
  if (data->mount_operation_reply_handler_id != 0)
    {
      g_signal_handler_disconnect (data->mount_operation, data->mount_operation_reply_handler_id);
      data->mount_operation_reply_handler_id = 0;
    }

  if (result != G_MOUNT_OPERATION_HANDLED)
    {
      if (result == G_MOUNT_OPERATION_ABORTED)
        {
          /* The user aborted the operation so consider it "handled" */
          g_simple_async_result_set_error (data->simple,
                                           G_IO_ERROR,
                                           G_IO_ERROR_FAILED_HANDLED,
                                           "Password dialog aborted (user should never see this error since it is G_IO_ERROR_FAILED_HANDLED)");
        }
      else
        {
          g_simple_async_result_set_error (data->simple,
                                           G_IO_ERROR,
                                           G_IO_ERROR_PERMISSION_DENIED,
                                           "Expected G_MOUNT_OPERATION_HANDLED but got %d", result);
        }
      g_simple_async_result_complete (data->simple);
      data->volume->pending_mount_op = NULL;
      mount_op_data_unref (data);
      goto out;
    }

  password = g_mount_operation_get_password (mount_operation);

  device = gdu_presentable_get_device (GDU_PRESENTABLE (data->volume->gdu_volume));

  g_object_set_data (G_OBJECT (device),
                     "password-save",
                     GINT_TO_POINTER (g_mount_operation_get_password_save (mount_operation)));
  g_object_set_data_full (G_OBJECT (device),
                          "password",
                          g_strdup (password),
                          (GDestroyNotify) scrub_n_free_string);

  gdu_device_op_luks_unlock (device, password, unlock_cb, data);

  g_object_unref (device);

 out:
  ;
}

static void
mount_with_mount_operation (MountOpData *data)
{
  gchar *message;
  gchar *drive_name;
  GduPresentable *toplevel;
  GduDevice *device;

  device = NULL;
  drive_name = NULL;
  message = NULL;
  toplevel = NULL;

  /* if we've already aborted due to device removal, just bail out */
  if (data->is_cancelled)
    {
      mount_op_data_unref (data);
      goto out;
    }

  if (data->mount_operation == NULL)
    {
      g_simple_async_result_set_error (data->simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_FAILED,
                                       "Password required to access the encrypted data");
      g_simple_async_result_complete (data->simple);
      data->volume->pending_mount_op = NULL;
      mount_op_data_unref (data);
      goto out;
    }

  device = gdu_presentable_get_device (GDU_PRESENTABLE (data->volume->gdu_volume));

  toplevel = gdu_presentable_get_enclosing_presentable (GDU_PRESENTABLE (data->volume->gdu_volume));
  /* handle logical partitions enclosed by an extented partition */
  if (GDU_IS_VOLUME (toplevel))
    {
      GduPresentable *temp;
      temp = toplevel;
      toplevel = gdu_presentable_get_enclosing_presentable (toplevel);
      g_object_unref (temp);
      if (!GDU_IS_DRIVE (toplevel))
        {
          g_object_unref (toplevel);
          toplevel = NULL;
        }
    }

  if (toplevel != NULL)
    drive_name = gdu_presentable_get_name (toplevel);

  if (drive_name != NULL)
    {
      if (gdu_device_is_partition (device))
        {
          message = g_strdup_printf (_("Enter a password to unlock the volume\n"
                                       "The device \"%s\" contains encrypted data on partition %d."),
                                     drive_name,
                                     gdu_device_partition_get_number (device));
        }
      else
        {
          message = g_strdup_printf (_("Enter a password to unlock the volume\n"
                                       "The device \"%s\" contains encrypted data."),
                                     drive_name);
        }
    }
  else
    {
      message = g_strdup_printf (_("Enter a password to unlock the volume\n"
                                   "The device %s contains encrypted data."),
                                 gdu_device_get_device_file (device));
    }

  data->mount_operation_reply_handler_id = g_signal_connect (data->mount_operation,
                                                             "reply",
                                                             G_CALLBACK (mount_operation_reply),
                                                             data);

  g_signal_emit_by_name (data->mount_operation,
                         "ask-password",
                         message,
                         NULL,
                         NULL,
                         G_ASK_PASSWORD_NEED_PASSWORD |
                         G_ASK_PASSWORD_SAVING_SUPPORTED);

 out:
  g_free (drive_name);
  g_free (message);
  if (device != NULL)
    g_object_unref (device);
  if (toplevel != NULL)
    g_object_unref (toplevel);
}

static void
cancelled_cb (GCancellable *cancellable,
              GGduVolume   *volume)
{
  if (volume->pending_mount_op != NULL)
    {
      cancel_pending_mount_op (volume->pending_mount_op);
    }
}

static void
g_gdu_volume_mount (GVolume             *_volume,
                    GMountMountFlags     flags,
                    GMountOperation     *mount_operation,
                    GCancellable        *cancellable,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
  GGduVolume *volume = G_GDU_VOLUME (_volume);
  GSimpleAsyncResult *simple;
  GduDevice *device;
  GduPool *pool;
  const gchar *usage;
  const gchar *type;
  MountOpData *data;

  pool = NULL;
  device = NULL;

  /* for fstab mounts, call the native mount command */
  if (volume->unix_mount_point != NULL)
    {
      g_gdu_volume_mount_unix_mount_point (volume,
                                           flags,
                                           mount_operation,
                                           cancellable,
                                           callback,
                                           user_data);
      goto out;
    }

  if (volume->pending_mount_op != NULL)
    {
      simple = g_simple_async_result_new_error (G_OBJECT (volume),
                                                callback,
                                                user_data,
                                                G_IO_ERROR,
                                                G_IO_ERROR_FAILED,
                                                "A mount operation is already pending");
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      goto out;
    }

  device = gdu_presentable_get_device (GDU_PRESENTABLE (volume->gdu_volume));

  if (device == NULL)
    {
      simple = g_simple_async_result_new_error (G_OBJECT (volume),
                                                callback,
                                                user_data,
                                                G_IO_ERROR,
                                                G_IO_ERROR_FAILED,
                                                "Underlying device missing");
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      goto out;
    }

  pool = gdu_device_get_pool (device);

  /* Makes no sense to mount
   *
   *  - blank discs since these already have a burn:/// mount
   *  - other things that are already mounted
   *
   * Unfortunately Nautilus will try to do this anyway. For now, just return success for
   * such requests.
   */
  if (gdu_device_optical_disc_get_is_blank (device) || gdu_device_is_mounted (device))
    {
      simple = g_simple_async_result_new (G_OBJECT (volume),
                                          callback,
                                          user_data,
                                          g_gdu_volume_mount);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      goto out;
    }

  data = g_new0 (MountOpData, 1);

  data->volume = g_object_ref (volume);

  data->simple = g_simple_async_result_new (G_OBJECT (volume),
                                            callback,
                                            user_data,
                                            g_gdu_volume_mount);

  data->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;

  data->mount_operation = mount_operation != NULL ? g_object_ref (mount_operation) : NULL;

  if (data->cancellable != NULL)
    data->cancelled_handler_id = g_signal_connect (data->cancellable, "cancelled", G_CALLBACK (cancelled_cb), volume);

  volume->pending_mount_op = data;

  /* if the device is already unlocked, just attempt to mount it */
  if (volume->cleartext_gdu_volume != NULL)
    {
      GduDevice *luks_cleartext_volume_device;
      const gchar *object_path_of_cleartext_device;

      luks_cleartext_volume_device = gdu_presentable_get_device (GDU_PRESENTABLE (volume->cleartext_gdu_volume));

      if (luks_cleartext_volume_device != NULL)
        {
          object_path_of_cleartext_device = gdu_device_get_object_path (luks_cleartext_volume_device);

          mount_cleartext_device (data, object_path_of_cleartext_device);

          g_object_unref (luks_cleartext_volume_device);
        }
      goto out;
    }

  usage = gdu_device_id_get_usage (device);
  type = gdu_device_id_get_type (device);
  if (g_strcmp0 (usage, "crypto") == 0 && g_strcmp0 (type, "crypto_LUKS") == 0)
    {
      gchar *password;

      /* if we have the secret in the keyring, try with that first */
      password = gdu_util_get_secret (device);
      if (password != NULL)
        {
          gdu_device_op_luks_unlock (device, password, unlock_from_keyring_cb, data);

          scrub_n_free_string (password);
          goto out;
        }

      /* don't put up a password dialog if the daemon is inhibited */
      if (gdu_pool_is_daemon_inhibited (pool))
        {
          g_simple_async_result_set_error (data->simple,
                                           G_IO_ERROR,
                                           G_IO_ERROR_FAILED_HANDLED,
                                           "Daemon is currently inhibited");
          g_simple_async_result_complete (data->simple);
          volume->pending_mount_op = NULL;
          mount_op_data_unref (data);
          goto out;
        }

      mount_with_mount_operation (data);
    }
  else
    {
      gchar **mount_options;
      data->device_to_mount = g_object_ref (device);
      mount_options = get_mount_options (data->device_to_mount, data->mount_operation != NULL);
      gdu_device_op_filesystem_mount (data->device_to_mount, mount_options, mount_cb, data);
      g_strfreev (mount_options);
    }

 out:
  if (pool != NULL)
    g_object_unref (pool);
  if (device != NULL)
    g_object_unref (device);
}

static gboolean
g_gdu_volume_mount_finish (GVolume       *volume,
                           GAsyncResult  *result,
                           GError       **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);

  //g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == g_gdu_volume_mount);

  return !g_simple_async_result_propagate_error (simple, error);
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct {
  GGduVolume *volume;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  int error_fd;
  GIOChannel *error_channel;
  guint error_channel_source_id;
  GString *error_string;

  guint wait_for_mount_timeout_id;
  gulong wait_for_mount_changed_signal_handler_id;
} MountPointOp;

static void
mount_point_op_free (MountPointOp *data)
{
  if (data->error_channel_source_id > 0)
    g_source_remove (data->error_channel_source_id);
  if (data->error_channel != NULL)
    g_io_channel_unref (data->error_channel);
  if (data->error_string != NULL)
    g_string_free (data->error_string, TRUE);
  if (data->error_fd > 0)
    close (data->error_fd);
  g_free (data);
}

static void
mount_point_op_changed_cb (GVolume *volume,
                           gpointer user_data)
{
  MountPointOp *data = user_data;
  GSimpleAsyncResult *simple;

  /* keep waiting if the mount hasn't appeared */
  if (data->volume->mount == NULL)
    goto out;

  simple = g_simple_async_result_new (G_OBJECT (data->volume),
                                      data->callback,
                                      data->user_data,
                                      NULL);
  /* complete in idle to make sure the mount is added before we return */
  g_simple_async_result_complete_in_idle (simple);
  g_object_unref (simple);

  g_signal_handler_disconnect (data->volume, data->wait_for_mount_changed_signal_handler_id);
  g_source_remove (data->wait_for_mount_timeout_id);

  mount_point_op_free (data);

 out:
  ;
}

static gboolean
mount_point_op_never_appeared_cb (gpointer user_data)
{
  MountPointOp *data = user_data;
  GSimpleAsyncResult *simple;

  simple = g_simple_async_result_new_error (G_OBJECT (data->volume),
                                            data->callback,
                                            data->user_data,
                                            G_IO_ERROR,
                                            G_IO_ERROR_FAILED,
                                            "Timeout waiting for mount to appear");
  g_simple_async_result_complete (simple);
  g_object_unref (simple);

  g_signal_handler_disconnect (data->volume, data->wait_for_mount_changed_signal_handler_id);
  g_source_remove (data->wait_for_mount_timeout_id);

  mount_point_op_free (data);

  return FALSE;
}

static void
mount_point_op_cb (GPid pid, gint status, gpointer user_data)
{
  MountPointOp *data = user_data;
  GSimpleAsyncResult *simple;

  g_spawn_close_pid (pid);

  if (WEXITSTATUS (status) != 0)
    {
      GError *error;
      error = g_error_new_literal (G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   data->error_string->str);
      simple = g_simple_async_result_new_from_error (G_OBJECT (data->volume),
                                                     data->callback,
                                                     data->user_data,
                                                     error);
      g_error_free (error);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      mount_point_op_free (data);
    }
  else
    {
      /* wait for the GMount to appear - this is to honor this requirement
       *
       *  "If the mount operation succeeded, g_volume_get_mount() on
       *   volume is guaranteed to return the mount right after calling
       *   this function; there's no need to listen for the
       *   'mount-added' signal on GVolumeMonitor."
       *
       * So we set up a signal handler waiting for it to appear. We also set up
       * a timer for handling the case when it never appears.
       */
      if (data->volume->mount == NULL)
        {
          /* no need to ref, GSimpleAsyncResult has a ref on data->volume */
          data->wait_for_mount_timeout_id = g_timeout_add (5 * 1000,
                                            mount_point_op_never_appeared_cb,
                                            data);
          data->wait_for_mount_changed_signal_handler_id = g_signal_connect (data->volume,
                                                                             "changed",
                                                                             G_CALLBACK (mount_point_op_changed_cb),
                                                                             data);
        }
      else
        {
          /* have the mount already, finish up */
          simple = g_simple_async_result_new (G_OBJECT (data->volume),
                                              data->callback,
                                              data->user_data,
                                              NULL);
          g_simple_async_result_complete (simple);
          g_object_unref (simple);
          mount_point_op_free (data);
        }
    }
}

static gboolean
mount_point_op_read_error (GIOChannel *channel,
                           GIOCondition condition,
                           gpointer user_data)
{
  MountPointOp *data = user_data;
  gchar buf[BUFSIZ];
  gsize bytes_read;
  GError *error;
  GIOStatus status;

  error = NULL;
read:
  status = g_io_channel_read_chars (channel, buf, sizeof (buf), &bytes_read, &error);
  if (status == G_IO_STATUS_NORMAL)
   {
     g_string_append_len (data->error_string, buf, bytes_read);
     if (bytes_read == sizeof (buf))
        goto read;
   }
  else if (status == G_IO_STATUS_EOF)
    {
      g_string_append_len (data->error_string, buf, bytes_read);
    }
  else if (status == G_IO_STATUS_ERROR)
    {
      if (data->error_string->len > 0)
        g_string_append (data->error_string, "\n");

      g_string_append (data->error_string, error->message);
      g_error_free (error);
      return FALSE;
    }

  return TRUE;
}

static void
g_gdu_volume_mount_unix_mount_point (GGduVolume          *volume,
                                     GMountMountFlags     flags,
                                     GMountOperation     *mount_operation,
                                     GCancellable        *cancellable,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  MountPointOp *data;
  GPid child_pid;
  GError *error;
  const gchar *argv[] = {"mount", NULL, NULL};

  argv[1] = g_unix_mount_point_get_mount_path (volume->unix_mount_point);

  data = g_new0 (MountPointOp, 1);
  data->volume = volume;
  data->callback = callback;
  data->user_data = user_data;
  data->cancellable = cancellable;

  error = NULL;
  if (!g_spawn_async_with_pipes (NULL,         /* working dir */
                                 (gchar **) argv,
                                 NULL,         /* envp */
                                 G_SPAWN_DO_NOT_REAP_CHILD|G_SPAWN_SEARCH_PATH,
                                 NULL,         /* child_setup */
                                 NULL,         /* user_data for child_setup */
                                 &child_pid,
                                 NULL,         /* standard_input */
                                 NULL,         /* standard_output */
                                 &(data->error_fd),
                                 &error))
    {
      g_assert (error != NULL);
      goto handle_error;
    }

  data->error_string = g_string_new ("");

  data->error_channel = g_io_channel_unix_new (data->error_fd);
  g_io_channel_set_flags (data->error_channel, G_IO_FLAG_NONBLOCK, &error);
  if (error != NULL)
    goto handle_error;

  data->error_channel_source_id = g_io_add_watch (data->error_channel, G_IO_IN, mount_point_op_read_error, data);
  g_child_watch_add (child_pid, mount_point_op_cb, data);

handle_error:
  if (error != NULL)
    {
      GSimpleAsyncResult *simple;
      simple = g_simple_async_result_new_from_error (G_OBJECT (data->volume),
                                                     data->callback,
                                                     data->user_data,
                                                     error);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);

      mount_point_op_free (data);
    }
}

/* ---------------------------------------------------------------------------------------------------- */

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
g_gdu_volume_eject_with_operation (GVolume              *volume,
                                   GMountUnmountFlags   flags,
                                   GMountOperation     *mount_operation,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
  GGduVolume *gdu_volume = G_GDU_VOLUME (volume);
  GGduDrive *drive;

  drive = NULL;
  if (gdu_volume->drive != NULL)
    drive = g_object_ref (gdu_volume->drive);

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
  else
    {
      GSimpleAsyncResult *simple;
      simple = g_simple_async_result_new_error (G_OBJECT (volume),
                                                callback,
                                                user_data,
                                                G_IO_ERROR,
                                                G_IO_ERROR_FAILED,
                                                _("Operation not supported by backend"));
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
    }
}

static gboolean
g_gdu_volume_eject_with_operation_finish (GVolume        *volume,
                                          GAsyncResult  *result,
                                          GError       **error)
{
  GGduVolume *gdu_volume = G_GDU_VOLUME (volume);
  gboolean res;

  res = TRUE;
  if (gdu_volume->drive != NULL)
    {
      res = g_drive_eject_with_operation_finish (G_DRIVE (gdu_volume->drive), result, error);
    }
  else
    {
      g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
      res = FALSE;
    }

  return res;
}

static void
g_gdu_volume_eject (GVolume              *volume,
                    GMountUnmountFlags   flags,
                    GCancellable        *cancellable,
                    GAsyncReadyCallback  callback,
                    gpointer             user_data)
{
  g_gdu_volume_eject_with_operation (volume, flags, NULL, cancellable, callback, user_data);
}

static gboolean
g_gdu_volume_eject_finish (GVolume        *volume,
                           GAsyncResult  *result,
                           GError       **error)
{
  return g_gdu_volume_eject_with_operation_finish (volume, result, error);
}

static char *
g_gdu_volume_get_identifier (GVolume     *_volume,
                             const char  *kind)
{
  GGduVolume *volume = G_GDU_VOLUME (_volume);
  GduDevice *device;
  const gchar *label;
  const gchar *uuid;
  gchar *id;

  id = NULL;

  if (volume->gdu_volume != NULL)
    {
      device = gdu_presentable_get_device (GDU_PRESENTABLE (volume->gdu_volume));

      if (device != NULL)
        {
          label = gdu_device_id_get_label (device);
          uuid = gdu_device_id_get_uuid (device);

          g_object_unref (device);

          if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE) == 0)
            id = g_strdup (volume->device_file);
          else if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_LABEL) == 0)
            id = strlen (label) > 0 ? g_strdup (label) : NULL;
          else if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_UUID) == 0)
            id = strlen (uuid) > 0 ? g_strdup (uuid) : NULL;
        }
    }

  return id;
}

static char **
g_gdu_volume_enumerate_identifiers (GVolume *_volume)
{
  GGduVolume *volume = G_GDU_VOLUME (_volume);
  GduDevice *device;
  GPtrArray *p;
  const gchar *label;
  const gchar *uuid;

  p = g_ptr_array_new ();
  label = NULL;

  if (volume->gdu_volume != NULL)
    {
      device = gdu_presentable_get_device (GDU_PRESENTABLE (volume->gdu_volume));

      if (device != NULL)
        {
          label = gdu_device_id_get_label (device);
          uuid = gdu_device_id_get_uuid (device);
          g_object_unref (device);

          g_ptr_array_add (p, g_strdup (G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));
          if (strlen (label) > 0)
            g_ptr_array_add (p, g_strdup (G_VOLUME_IDENTIFIER_KIND_LABEL));
          if (strlen (uuid) > 0)
            g_ptr_array_add (p, g_strdup (G_VOLUME_IDENTIFIER_KIND_UUID));
        }
    }

  g_ptr_array_add (p, NULL);

  return (gchar **) g_ptr_array_free (p, FALSE);
}

static GFile *
g_gdu_volume_get_activation_root (GVolume *_volume)
{
  GGduVolume *volume = G_GDU_VOLUME (_volume);
  return volume->activation_root != NULL ? g_object_ref (volume->activation_root) : NULL;
}

static void
g_gdu_volume_volume_iface_init (GVolumeIface *iface)
{
  iface->get_name = g_gdu_volume_get_name;
  iface->get_icon = g_gdu_volume_get_icon;
  iface->get_uuid = g_gdu_volume_get_uuid;
  iface->get_drive = g_gdu_volume_get_drive;
  iface->get_mount = g_gdu_volume_get_mount;
  iface->can_mount = g_gdu_volume_can_mount;
  iface->can_eject = g_gdu_volume_can_eject;
  iface->should_automount = g_gdu_volume_should_automount;
  iface->mount_fn = g_gdu_volume_mount;
  iface->mount_finish = g_gdu_volume_mount_finish;
  iface->eject = g_gdu_volume_eject;
  iface->eject_finish = g_gdu_volume_eject_finish;
  iface->eject_with_operation = g_gdu_volume_eject_with_operation;
  iface->eject_with_operation_finish = g_gdu_volume_eject_with_operation_finish;
  iface->get_identifier = g_gdu_volume_get_identifier;
  iface->enumerate_identifiers = g_gdu_volume_enumerate_identifiers;
  iface->get_activation_root = g_gdu_volume_get_activation_root;
}

gboolean
g_gdu_volume_has_dev (GGduVolume   *volume,
                      dev_t         dev)
{
  dev_t _dev;

  _dev = volume->dev;

  if (volume->cleartext_gdu_volume != NULL)
    {
      GduDevice *luks_cleartext_volume_device;
      luks_cleartext_volume_device = gdu_presentable_get_device (GDU_PRESENTABLE (volume->cleartext_gdu_volume));
      if (luks_cleartext_volume_device != NULL)
        {
          _dev = gdu_device_get_dev (luks_cleartext_volume_device);
          g_object_unref (luks_cleartext_volume_device);
        }
    }

  return _dev == dev;
}

gboolean
g_gdu_volume_has_device_file (GGduVolume   *volume,
                              const gchar  *device_file)
{
  const gchar *_device_file;

  _device_file = volume->device_file;

  if (volume->cleartext_gdu_volume != NULL)
    {
      GduDevice *luks_cleartext_volume_device;
      luks_cleartext_volume_device = gdu_presentable_get_device (GDU_PRESENTABLE (volume->cleartext_gdu_volume));
      if (luks_cleartext_volume_device != NULL)
        {
          _device_file = gdu_device_get_device_file (luks_cleartext_volume_device);
          g_object_unref (luks_cleartext_volume_device);
        }
    }

  return g_strcmp0 (_device_file, device_file) == 0;
}


gboolean
g_gdu_volume_has_mount_path (GGduVolume *volume,
                             const char  *mount_path)
{
  GduDevice *device;
  GduPresentable *presentable;
  gboolean ret;

  ret = FALSE;

  presentable = g_gdu_volume_get_presentable_with_cleartext (volume);
  if (presentable != NULL)
    {
      device = gdu_presentable_get_device (presentable);
      if (device != NULL)
        {
          ret = g_strcmp0 (gdu_device_get_mount_path (device), mount_path) == 0;
          g_object_unref (device);
        }
    }

  return ret;
}

gboolean
g_gdu_volume_has_uuid (GGduVolume  *volume,
                       const char  *uuid)
{
  const gchar *_uuid;

  _uuid = volume->uuid;

  if (volume->cleartext_gdu_volume != NULL)
    {
      GduDevice *luks_cleartext_volume_device;
      luks_cleartext_volume_device = gdu_presentable_get_device (GDU_PRESENTABLE (volume->cleartext_gdu_volume));
      if (luks_cleartext_volume_device != NULL)
        {
          _uuid = gdu_device_id_get_uuid (luks_cleartext_volume_device);
          g_object_unref (luks_cleartext_volume_device);
        }
    }

  return g_strcmp0 (_uuid, uuid) == 0;
}

GduPresentable *
g_gdu_volume_get_presentable (GGduVolume *volume)
{
  return GDU_PRESENTABLE (volume->gdu_volume);
}

GduPresentable *
g_gdu_volume_get_presentable_with_cleartext (GGduVolume *volume)
{
  GduVolume *ret;

  ret = volume->cleartext_gdu_volume;
  if (ret == NULL)
    ret = volume->gdu_volume;

  return GDU_PRESENTABLE (ret);
}

GUnixMountPoint *
g_gdu_volume_get_unix_mount_point (GGduVolume *volume)
{
  return volume->unix_mount_point;
}

gboolean
g_gdu_volume_has_presentable (GGduVolume       *volume,
                              GduPresentable  *presentable)
{
  return volume->gdu_volume != NULL &&
    g_strcmp0 (gdu_presentable_get_id (GDU_PRESENTABLE (volume->gdu_volume)),
               gdu_presentable_get_id (presentable)) == 0;
}
