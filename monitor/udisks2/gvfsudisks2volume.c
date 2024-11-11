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
#include <gio/gio.h>

#ifdef HAVE_KEYRING
#define SECRET_API_SUBJECT_TO_CHANGE
#include <libsecret/secret.h>
#endif

#include "gvfsudisks2drive.h"
#include "gvfsudisks2volume.h"
#include "gvfsudisks2mount.h"
#include "gvfsudisks2utils.h"

typedef struct _GVfsUDisks2VolumeClass GVfsUDisks2VolumeClass;

struct _GVfsUDisks2VolumeClass
{
  GObjectClass parent_class;
};

static void mount_cancel_pending_op (GTask *task);

struct _GVfsUDisks2Volume
{
  GObject parent;

  GVfsUDisks2VolumeMonitor *monitor; /* owned by volume monitor */
  GVfsUDisks2Mount         *mount;   /* owned by volume monitor */
  GVfsUDisks2Drive         *drive;   /* owned by volume monitor */

  /* If TRUE, the volume was discovered at coldplug time */
  gboolean coldplug;

  /* exactly one of these are set */
  UDisksBlock *block;
  GUnixMountPoint *mount_point;

  /* set in update_volume() */
  GIcon *icon;
  GIcon *symbolic_icon;
  GFile *activation_root;
  gchar *name;
  gchar *sort_key;
  gchar *device_file;
  dev_t dev;
  gchar *uuid;
  gboolean can_mount;
  gboolean should_automount;

  /* If a mount operation is in progress, then pending_mount_op is != NULL. This
   * is used to cancel the operation to make possible authentication dialogs go
   * away.
   */
  GTask *mount_pending_op;
};

typedef struct MountData
{
  gulong mount_operation_reply_handler_id;
  gulong mount_operation_aborted_handler_id;
  GMountOperation *mount_operation;

  gchar *passphrase;
  gboolean hidden_volume;
  gboolean system_volume;
  guint pim;

  gchar *passphrase_from_keyring;
  GPasswordSave password_save;

  gchar *uuid_of_encrypted_to_unlock;
  gchar *desc_of_encrypted_to_unlock;
  UDisksEncrypted *encrypted_to_unlock;
  UDisksFilesystem *filesystem_to_mount;

  gboolean checked_keyring;
} MountData;

static void gvfs_udisks2_volume_volume_iface_init (GVolumeIface *iface);

static void on_block_changed (GObject    *object,
                              GParamSpec *pspec,
                              gpointer    user_data);

static void on_udisks_client_changed (UDisksClient *client,
                                      gpointer      user_data);

G_DEFINE_TYPE_EXTENDED (GVfsUDisks2Volume, gvfs_udisks2_volume, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME, gvfs_udisks2_volume_volume_iface_init))

static void
gvfs_udisks2_volume_finalize (GObject *object)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (object);

  g_signal_handlers_disconnect_by_func (gvfs_udisks2_volume_monitor_get_udisks_client (volume->monitor),
                                        G_CALLBACK (on_udisks_client_changed),
                                        volume);

  if (volume->mount != NULL)
    {
      gvfs_udisks2_mount_unset_volume (volume->mount, volume);
    }

  if (volume->drive != NULL)
    {
      gvfs_udisks2_drive_unset_volume (volume->drive, volume);
    }

  if (volume->block != NULL)
    {
      g_signal_handlers_disconnect_by_func (volume->block, G_CALLBACK (on_block_changed), volume);
      g_object_unref (volume->block);
    }

  if (volume->mount_point != NULL)
    g_unix_mount_point_free (volume->mount_point);

  g_clear_object (&volume->icon);
  g_clear_object (&volume->symbolic_icon);
  g_clear_object (&volume->activation_root);

  g_free (volume->name);
  g_free (volume->sort_key);
  g_free (volume->device_file);
  g_free (volume->uuid);

  G_OBJECT_CLASS (gvfs_udisks2_volume_parent_class)->finalize (object);
}

static void
gvfs_udisks2_volume_class_init (GVfsUDisks2VolumeClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = gvfs_udisks2_volume_finalize;
}

static void
gvfs_udisks2_volume_init (GVfsUDisks2Volume *volume)
{
}

static void
emit_changed (GVfsUDisks2Volume *volume)
{
  g_signal_emit_by_name (volume, "changed");
  g_signal_emit_by_name (volume->monitor, "volume-changed", volume);
}

static void
apply_options_from_fstab (GVfsUDisks2Volume *volume,
                          const gchar       *fstab_options)
{
  gchar *name;
  gchar *icon_name;
  gchar *symbolic_icon_name;

  name = gvfs_udisks2_utils_lookup_fstab_options_value (fstab_options, "x-gvfs-name=");
  if (name != NULL)
    {
      g_free (volume->name);
      volume->name = name;
    }

  icon_name = gvfs_udisks2_utils_lookup_fstab_options_value (fstab_options, "x-gvfs-icon=");
  if (icon_name != NULL)
    {
      g_clear_object (&volume->icon);
      volume->icon = g_themed_icon_new_with_default_fallbacks (icon_name);
      g_free (icon_name);
    }

  symbolic_icon_name = gvfs_udisks2_utils_lookup_fstab_options_value (fstab_options, "x-gvfs-symbolic-icon=");
  if (symbolic_icon_name != NULL)
    {
      g_clear_object (&volume->symbolic_icon);
      volume->symbolic_icon = g_themed_icon_new_with_default_fallbacks (symbolic_icon_name);
      g_free (symbolic_icon_name);
    }
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
update_volume (GVfsUDisks2Volume *volume)
{
  UDisksClient *udisks_client;
  gboolean changed;
  gboolean old_can_mount;
  gboolean old_should_automount;
  gchar *old_name;
  gchar *old_device_file;
  gchar *old_uuid;
  dev_t old_dev;
  GIcon *old_icon;
  UDisksDrive *udisks_drive;
  gchar *s;

  udisks_client = gvfs_udisks2_volume_monitor_get_udisks_client (volume->monitor);

  /* ---------------------------------------------------------------------------------------------------- */
  /* save old values */

  old_can_mount = volume->can_mount;
  old_should_automount = volume->should_automount;
  old_name = g_strdup (volume->name);
  old_device_file = g_strdup (volume->device_file);
  old_uuid = g_strdup (volume->uuid);
  old_dev = volume->dev;
  old_icon = volume->icon != NULL ? g_object_ref (volume->icon) : NULL;

  /* ---------------------------------------------------------------------------------------------------- */
  /* reset */

  volume->can_mount = volume->should_automount = FALSE;
  g_free (volume->name); volume->name = NULL;
  g_free (volume->device_file); volume->device_file = NULL;
  g_free (volume->uuid); volume->uuid = NULL;
  volume->dev = 0;
  g_clear_object (&volume->icon);
  g_clear_object (&volume->symbolic_icon);

  /* ---------------------------------------------------------------------------------------------------- */
  /* in with the new */

  if (volume->block != NULL)
    {
      const gchar *hint;
      UDisksBlock *block;
      UDisksBlock *cleartext_block;
      GVariantIter iter;
      const gchar *configuration_type;
      GVariant *configuration_value;
      UDisksLoop *loop = NULL;

      loop = udisks_client_get_loop_for_block (udisks_client,
                                               volume->block);

      /* If unlocked, use the values from the unlocked block device for presentation */
      cleartext_block = udisks_client_get_cleartext_block (udisks_client, volume->block);
      if (cleartext_block != NULL)
        block = cleartext_block;
      else
        block = volume->block;

      volume->dev = udisks_block_get_device_number (block);
      volume->device_file = udisks_block_dup_device (block);
      volume->uuid = udisks_block_dup_id_uuid (block);

      if (strlen (udisks_block_get_id_label (block)) > 0)
        {
          volume->name = g_strdup (udisks_block_get_id_label (block));
        }
      else if (g_strcmp0 (udisks_block_get_id_usage (block), "crypto") == 0)
        {
          s = udisks_client_get_size_for_display (udisks_client, udisks_block_get_size (volume->block), FALSE, FALSE);
          if (g_strcmp0 (udisks_block_get_id_type (block), "crypto_unknown") == 0)
            {
              /* Translators: This is used for possibly encrypted volumes.
               *              The first %s is the formatted size (e.g. "42.0 MB").
               */
              volume->name = g_strdup_printf (_("%s Possibly Encrypted"), s);
            }
          else
            {
              /* Translators: This is used for encrypted volumes.
               *              The first %s is the formatted size (e.g. "42.0 MB").
               */
              volume->name = g_strdup_printf (_("%s Encrypted"), s);
            }
          g_free (s);
        }
      else
        {
          guint64 size = udisks_block_get_size (block);
          if (size > 0)
            {
              s = udisks_client_get_size_for_display (udisks_client, size, FALSE, FALSE);
              /* Translators: This is used for volume with no filesystem label.
               *              The first %s is the formatted size (e.g. "42.0 MB").
               */
              volume->name = g_strdup_printf (_("%s Volume"), s);
              g_free (s);
            }
        }

      udisks_drive = udisks_client_get_drive_for_block (udisks_client, volume->block);
      if (udisks_drive != NULL)
        {
          gchar *drive_desc = NULL;
          GIcon *drive_icon = NULL;
          GIcon *drive_symbolic_icon = NULL;
          gchar *media_desc = NULL;
          GIcon *media_icon = NULL;
          GIcon *media_symbolic_icon = NULL;

#if UDISKS_CHECK_VERSION(2,0,90)
          {
            UDisksObject *object = (UDisksObject *) g_dbus_interface_get_object (G_DBUS_INTERFACE (udisks_drive));
            if (object != NULL)
              {
                UDisksObjectInfo *info = udisks_client_get_object_info (udisks_client, object);
                if (info != NULL)
                  {
                    drive_desc = g_strdup (udisks_object_info_get_description (info));
                    drive_icon = _g_object_ref0 (udisks_object_info_get_icon (info));
                    drive_symbolic_icon = _g_object_ref0 (udisks_object_info_get_icon_symbolic (info));
                    media_desc = g_strdup (udisks_object_info_get_media_description (info));
                    media_icon = _g_object_ref0 (udisks_object_info_get_media_icon (info));
                    media_symbolic_icon = _g_object_ref0 (udisks_object_info_get_media_icon_symbolic (info));
                    g_object_unref (info);
                  }
              }
          }
#else
          udisks_client_get_drive_info (udisks_client,
                                        udisks_drive,
                                        NULL, /* drive_name */
                                        &drive_desc,
                                        &drive_icon,
                                        &media_desc,
                                        &media_icon);
#endif

          if (media_desc == NULL)
            {
              media_desc = drive_desc;
              drive_desc = NULL;
            }
          if (media_icon == NULL)
            {
              media_icon = drive_icon;
              drive_icon = NULL;
            }
          if (media_symbolic_icon == NULL)
            {
              media_symbolic_icon = drive_symbolic_icon;
              drive_symbolic_icon = NULL;
            }

          /* Override name for blank and audio discs */
          if (udisks_drive_get_optical_blank (udisks_drive))
            {
              g_free (volume->name);
              volume->name = g_strdup (media_desc);
            }
#ifdef HAVE_CDDA
          else if (volume->activation_root != NULL && g_file_has_uri_scheme (volume->activation_root, "cdda"))
            {
              g_free (volume->name);
              volume->name = g_strdup (_("Audio Disc"));
            }
#endif

          volume->icon = media_icon != NULL ? g_object_ref (media_icon) : NULL;
          volume->symbolic_icon = media_symbolic_icon != NULL ? g_object_ref (media_symbolic_icon) : NULL;

          /* use media_desc if we haven't figured out a name yet (applies to e.g.
           * /dev/fd0 since its size is 0)
           */
          if (volume->name == NULL)
            {
              volume->name = media_desc;
              media_desc = NULL;
            }

          g_free (media_desc);
          g_clear_object (&media_icon);
          g_clear_object (&media_symbolic_icon);
          g_free (drive_desc);
          g_clear_object (&drive_icon);
          g_clear_object (&drive_symbolic_icon);

          /* Only automount drives attached to the same seat as we're running on
           */
          if (gvfs_udisks2_utils_is_drive_on_our_seat (udisks_drive))
            {
              /* Only automount filesystems from drives of known types/interconnects:
               *
               *  - USB
               *  - Firewire
               *  - sdio
               *  - optical discs
               *
               * The mantra here is "be careful" - we really don't want to
               * automount filesystems from all devices in a SAN etc - We
               * REALLY need to be CAREFUL here.
               *
               * Fortunately udisks provides a property just for this.
               */
              if (udisks_block_get_hint_auto (volume->block))
                {
                  gboolean just_plugged_in = FALSE;
                  /* Also, if a volume (partition) appear _much later_ than when media was inserted it
                   * can only be because the media was repartitioned. We don't want to automount
                   * such volumes. So only mark volumes appearing just after their drive.
                   *
                   * There's a catch here - if the volume was discovered at coldplug-time (typically
                   * when the user desktop session started), we can't use this heuristic
                   */
                  if (g_get_real_time () - udisks_drive_get_time_media_detected (udisks_drive) < 5 * G_USEC_PER_SEC)
                    just_plugged_in = TRUE;
                  if (volume->coldplug || just_plugged_in)
                    volume->should_automount = TRUE;
                }
            }
          g_object_unref (udisks_drive);
        }
      else
        {
          /* No UDisksDrive, but we do have a UDisksBlock (example: /dev/loop0). Use
           * that to get the icons via UDisksObjectInfo
           */
#if UDISKS_CHECK_VERSION(2,0,90)
          {
            UDisksObject *object = (UDisksObject *) g_dbus_interface_get_object (G_DBUS_INTERFACE (volume->block));
            if (object != NULL)
              {
                UDisksObjectInfo *info = udisks_client_get_object_info (udisks_client, object);
                if (info != NULL)
                  {
                    volume->icon = _g_object_ref0 (udisks_object_info_get_icon (info));
                    volume->symbolic_icon = _g_object_ref0 (udisks_object_info_get_icon_symbolic (info));
                    g_object_unref (info);
                  }
              }
          }
#endif
        }

      /* Also automount loop devices set up by the user himself - e.g. via the
       * udisks interfaces or the gnome-disk-image-mounter(1) command
       */
      if (loop != NULL)
        {
          if (udisks_loop_get_setup_by_uid (loop) == getuid ())
            {
              volume->should_automount = TRUE;
            }
        }

      /* Use hints, if available */
      hint = udisks_block_get_hint_name (volume->block);
      if (hint != NULL && strlen (hint) > 0)
        {
          g_free (volume->name);
          volume->name = g_strdup (hint);
        }
      hint = udisks_block_get_hint_icon_name (volume->block);
      if (hint != NULL && strlen (hint) > 0)
        {
          g_clear_object (&volume->icon);
          volume->icon = g_themed_icon_new_with_default_fallbacks (hint);
        }
#if UDISKS_CHECK_VERSION(2,0,90)
      hint = udisks_block_get_hint_symbolic_icon_name (volume->block);
      if (hint != NULL && strlen (hint) > 0)
        {
          g_clear_object (&volume->symbolic_icon);
          volume->symbolic_icon = g_themed_icon_new_with_default_fallbacks (hint);
        }
#endif

      /* Use x-gvfs-name=The%20Name, x-gvfs-icon=foo-name, x-gvfs-icon=foo-name-symbolic, if available */
      g_variant_iter_init (&iter, udisks_block_get_configuration (block));
      while (g_variant_iter_next (&iter, "(&s@a{sv})", &configuration_type, &configuration_value))
        {
          if (g_strcmp0 (configuration_type, "fstab") == 0)
            {
              const gchar *fstab_options;
              if (g_variant_lookup (configuration_value, "opts", "^&ay", &fstab_options))
                apply_options_from_fstab (volume, fstab_options);
            }
          g_variant_unref (configuration_value);
        }

      /* Add an emblem, depending on whether the encrypted volume is locked or unlocked */
      if (g_strcmp0 (udisks_block_get_id_usage (volume->block), "crypto") == 0)
        {
          GEmblem *emblem;
          GIcon *padlock;
          GIcon *emblemed_icon;

          if (volume->icon == NULL)
            volume->icon = g_themed_icon_new ("drive-removable-media");

          if (cleartext_block != NULL)
            padlock = g_themed_icon_new ("changes-allow");
          else
            padlock = g_themed_icon_new ("changes-prevent");
          emblem = g_emblem_new_with_origin (padlock, G_EMBLEM_ORIGIN_DEVICE);
          emblemed_icon = g_emblemed_icon_new (volume->icon, emblem);
          g_object_unref (padlock);
          g_object_unref (emblem);

          g_object_unref (volume->icon);
          volume->icon = emblemed_icon;
        }

      g_clear_object (&cleartext_block);
      g_clear_object (&loop);
    }
  else
    {
      apply_options_from_fstab (volume, g_unix_mount_point_get_options (volume->mount_point));
      if (volume->name == NULL)
        volume->name = g_unix_mount_point_guess_name (volume->mount_point);
      if (volume->icon == NULL)
        volume->icon = gvfs_udisks2_utils_icon_from_fs_type (g_unix_mount_point_get_fs_type (volume->mount_point));
      if (volume->symbolic_icon == NULL)
        volume->symbolic_icon = gvfs_udisks2_utils_symbolic_icon_from_fs_type (g_unix_mount_point_get_fs_type (volume->mount_point));
    }

  if (volume->mount == NULL)
    volume->can_mount = TRUE;

  /* ---------------------------------------------------------------------------------------------------- */
  /* fallbacks */

  if (volume->name == NULL)
    {
      /* Translators: Name used for volume */
      volume->name = g_strdup (_("Volume"));
    }
  if (volume->icon == NULL)
    volume->icon = g_themed_icon_new ("drive-removable-media");
  if (volume->symbolic_icon == NULL)
    volume->symbolic_icon = g_themed_icon_new ("drive-removable-media-symbolic");

  /* ---------------------------------------------------------------------------------------------------- */
  /* compute whether something changed */

  changed = !((old_can_mount == volume->can_mount) &&
              (old_should_automount == volume->should_automount) &&
              (g_strcmp0 (old_name, volume->name) == 0) &&
              (g_strcmp0 (old_device_file, volume->device_file) == 0) &&
              (g_strcmp0 (old_uuid, volume->uuid) == 0) &&
              (old_dev == volume->dev) &&
              g_icon_equal (old_icon, volume->icon)
              );

  /* ---------------------------------------------------------------------------------------------------- */
  /* free old values */

  g_free (old_name);
  g_free (old_device_file);
  g_free (old_uuid);
  if (old_icon != NULL)
    g_object_unref (old_icon);

  return changed;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_volume_on_event (GVfsUDisks2Volume *volume)
{
  if (update_volume (volume))
    {
      emit_changed (volume);
      /* It could be that volume->dev changed (cryptotext volume morphing into a cleartext
       * volume)... since this is used to associated mounts with volumes (see the loop over
       * @unchanged in gvfsudisks2volumemonitor.c:update_mounts()) we need to over
       * the volume monitor
       */
      gvfs_udisks2_volume_monitor_update (volume->monitor);
    }
}

static void
on_block_changed (GObject    *object,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (user_data);
  update_volume_on_event (volume);
}

static void
on_udisks_client_changed (UDisksClient *client,
                          gpointer      user_data)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (user_data);
  MountData *data = NULL;

  /* This is needed as gvfs_udisks2_volume_monitor_update may remove this volume. */
  g_object_ref (volume);

  update_volume_on_event (volume);

  if (volume->mount_pending_op)
    data = g_task_get_task_data (volume->mount_pending_op);

  if (data && data->mount_operation_aborted_handler_id && data->encrypted_to_unlock)
    {
      UDisksBlock *cleartext_block;

      cleartext_block = udisks_client_get_cleartext_block (client, volume->block);
      if (cleartext_block != NULL)
        {
          g_object_set_data (G_OBJECT (data->mount_operation), "x-udisks2-is-unlocked", GINT_TO_POINTER (1));
          g_signal_emit_by_name (data->mount_operation, "aborted");
          g_object_unref (cleartext_block);
        }
    }

  g_object_unref (volume);
}

/* takes ownership of @mount_point if not NULL */
GVfsUDisks2Volume *
gvfs_udisks2_volume_new (GVfsUDisks2VolumeMonitor   *monitor,
                         UDisksBlock                *block,
                         GUnixMountPoint            *mount_point,
                         GVfsUDisks2Drive           *drive,
                         GFile                      *activation_root,
                         gboolean                    coldplug)
{
  GVfsUDisks2Volume *volume;

  volume = g_object_new (GVFS_TYPE_UDISKS2_VOLUME, NULL);
  volume->monitor = monitor;
  volume->coldplug = coldplug;

  volume->sort_key = g_strdup_printf ("gvfs.time_detected_usec.%" G_GINT64_FORMAT, g_get_real_time ());

  if (block != NULL)
    {
      volume->block = g_object_ref (block);
      g_signal_connect (volume->block, "notify", G_CALLBACK (on_block_changed), volume);
    }
  else if (mount_point != NULL)
    {
      volume->mount_point = mount_point;
    }
  else
    {
      g_assert_not_reached ();
    }

  volume->activation_root = activation_root != NULL ? g_object_ref (activation_root) : NULL;

  volume->drive = drive;
  if (drive != NULL)
    gvfs_udisks2_drive_set_volume (drive, volume);

  update_volume (volume);

  /* For encrypted devices, we also need to listen for changes on any possible cleartext device */
  if (volume->block != NULL && g_strcmp0 (udisks_block_get_id_usage (volume->block), "crypto") == 0)
    {
      g_signal_connect (gvfs_udisks2_volume_monitor_get_udisks_client (volume->monitor),
                        "changed",
                        G_CALLBACK (on_udisks_client_changed),
                        volume);
    }

  return volume;
}

/* ---------------------------------------------------------------------------------------------------- */

void
gvfs_udisks2_volume_removed (GVfsUDisks2Volume *volume)
{
  if (volume->mount_pending_op != NULL)
    mount_cancel_pending_op (volume->mount_pending_op);

  if (volume->mount != NULL)
    {
      gvfs_udisks2_mount_unset_volume (volume->mount, volume);
      volume->mount = NULL;
    }

  if (volume->drive != NULL)
    {
      gvfs_udisks2_drive_unset_volume (volume->drive, volume);
      volume->drive = NULL;
    }
}

void
gvfs_udisks2_volume_set_mount (GVfsUDisks2Volume *volume,
                               GVfsUDisks2Mount  *mount)
{
  if (volume->mount != mount)
    {
      if (volume->mount != NULL)
        gvfs_udisks2_mount_unset_volume (volume->mount, volume);

      volume->mount = mount;
      update_volume (volume);
      emit_changed (volume);
    }
}

void
gvfs_udisks2_volume_unset_mount (GVfsUDisks2Volume *volume,
                                 GVfsUDisks2Mount  *mount)
{
  if (volume->mount == mount)
    {
      volume->mount = NULL;
      update_volume (volume);
      emit_changed (volume);
    }
}

void
gvfs_udisks2_volume_set_drive (GVfsUDisks2Volume *volume,
                               GVfsUDisks2Drive  *drive)
{
  if (volume->drive != drive)
    {
      if (volume->drive != NULL)
        gvfs_udisks2_drive_unset_volume (volume->drive, volume);
      volume->drive = drive;
      emit_changed (volume);
    }
}

void
gvfs_udisks2_volume_unset_drive (GVfsUDisks2Volume *volume,
                                 GVfsUDisks2Drive  *drive)
{
  if (volume->drive == drive)
    {
      volume->drive = NULL;
      emit_changed (volume);
    }
}

static GIcon *
gvfs_udisks2_volume_get_icon (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  return volume->icon != NULL ? g_object_ref (volume->icon) : NULL;
}

static GIcon *
gvfs_udisks2_volume_get_symbolic_icon (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  return volume->symbolic_icon != NULL ? g_object_ref (volume->symbolic_icon) : NULL;
}

static char *
gvfs_udisks2_volume_get_name (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  return g_strdup (volume->name);
}

static char *
gvfs_udisks2_volume_get_uuid (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  return g_strdup (volume->uuid);
}

static gboolean
gvfs_udisks2_volume_can_mount (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  return volume->can_mount;
}

static gboolean
gvfs_udisks2_volume_can_eject (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  gboolean can_eject = FALSE;

  if (volume->drive != NULL)
    can_eject = g_drive_can_eject (G_DRIVE (volume->drive));
  return can_eject;
}

static gboolean
gvfs_udisks2_volume_should_automount (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  return volume->should_automount;
}

static GDrive *
gvfs_udisks2_volume_get_drive (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  GDrive *drive = NULL;

  if (volume->drive != NULL)
    drive = G_DRIVE (g_object_ref (volume->drive));
  return drive;
}

static GMount *
gvfs_udisks2_volume_get_mount (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  GMount *mount = NULL;

  if (volume->mount != NULL)
    mount = G_MOUNT (g_object_ref (volume->mount));
  return mount;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
gvfs_udisks2_volume_is_network_class (GVfsUDisks2Volume *volume)
{
  gboolean ret = FALSE;
  if (volume->mount_point != NULL)
    {
      const gchar *fstype = g_unix_mount_point_get_fs_type (volume->mount_point);
      if (g_strcmp0 (fstype, "nfs") == 0 ||
          g_strcmp0 (fstype, "nfs4") == 0 ||
          g_strcmp0 (fstype, "cifs") == 0 ||
          g_strcmp0 (fstype, "smbfs") == 0 ||
          g_strcmp0 (fstype, "ncpfs") == 0)
        ret = TRUE;
    }
  return ret;
}

static gboolean
gvfs_udisks2_volume_is_loop_class (GVfsUDisks2Volume *volume)
{
  UDisksClient *client;
  UDisksLoop *loop;
  UDisksObject *object;

  if (volume->block == NULL)
    return FALSE;

  client = gvfs_udisks2_volume_monitor_get_udisks_client (volume->monitor);

  /* Check if the volume is a loop device */
  loop = udisks_client_get_loop_for_block (client, volume->block);
  if (loop != NULL)
    return TRUE;

  /* Check if the volume is an unlocked encrypted loop device */
  object = udisks_client_get_object (client, udisks_block_get_crypto_backing_device (volume->block));
  if (object != NULL && udisks_object_get_loop (object) != NULL)
    return TRUE;

  return FALSE;
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
gvfs_udisks2_volume_get_identifier (GVolume      *_volume,
                                    const gchar  *kind)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  const gchar *label;
  const gchar *uuid;
  gchar *ret = NULL;

  if (volume->block != NULL)
    {
      label = udisks_block_get_id_label (volume->block);
      uuid = udisks_block_get_id_uuid (volume->block);

      if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE) == 0)
        ret = g_strdup (volume->device_file);
      else if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_LABEL) == 0)
        ret = strlen (label) > 0 ? g_strdup (label) : NULL;
      else if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_UUID) == 0)
        ret = strlen (uuid) > 0 ? g_strdup (uuid) : NULL;
    }
  if (strcmp (kind, G_VOLUME_IDENTIFIER_KIND_CLASS) == 0)
    {
      if (gvfs_udisks2_volume_is_network_class (volume))
        ret = g_strdup ("network");
      else if (gvfs_udisks2_volume_is_loop_class (volume))
        ret = g_strdup ("loop");
      else
        ret = g_strdup ("device");
    }

  return ret;
}

static gchar **
gvfs_udisks2_volume_enumerate_identifiers (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  GPtrArray *p;

  p = g_ptr_array_new ();
  g_ptr_array_add (p, g_strdup (G_VOLUME_IDENTIFIER_KIND_CLASS));
  if (volume->block != NULL)
    {
      const gchar *label;
      const gchar *uuid;
      label = udisks_block_get_id_label (volume->block);
      uuid = udisks_block_get_id_uuid (volume->block);
      g_ptr_array_add (p, g_strdup (G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));
      if (strlen (label) > 0)
        g_ptr_array_add (p, g_strdup (G_VOLUME_IDENTIFIER_KIND_LABEL));
      if (strlen (uuid) > 0)
        g_ptr_array_add (p, g_strdup (G_VOLUME_IDENTIFIER_KIND_UUID));
    }
  g_ptr_array_add (p, NULL);
  return (gchar **) g_ptr_array_free (p, FALSE);
}

static GFile *
gvfs_udisks2_volume_get_activation_root (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  return volume->activation_root != NULL ? g_object_ref (volume->activation_root) : NULL;
}

/* ---------------------------------------------------------------------------------------------------- */

#ifdef HAVE_KEYRING
static SecretSchema luks_passphrase_schema =
{
  "org.gnome.GVfs.Luks.Password",
  SECRET_SCHEMA_DONT_MATCH_NAME,
  {
    { "gvfs-luks-uuid", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 },
  }
};
#endif

static void
mount_data_free (MountData *data)
{
  if (data->mount_operation != NULL)
    {
      if (data->mount_operation_reply_handler_id != 0)
        g_signal_handler_disconnect (data->mount_operation, data->mount_operation_reply_handler_id);
      if (data->mount_operation_aborted_handler_id != 0)
        g_signal_handler_disconnect (data->mount_operation, data->mount_operation_aborted_handler_id);
      g_object_unref (data->mount_operation);
    }

#ifdef HAVE_KEYRING
  secret_password_free (data->passphrase);
  secret_password_free (data->passphrase_from_keyring);
#else
  g_free (data->passphrase);
  g_free (data->passphrase_from_keyring);
#endif

  g_free (data->uuid_of_encrypted_to_unlock);
  g_free (data->desc_of_encrypted_to_unlock);
  g_clear_object (&data->encrypted_to_unlock);
  g_clear_object (&data->filesystem_to_mount);
  g_free (data);
}

static void
mount_cancel_pending_op (GTask *task)
{
  MountData *data = g_task_get_task_data (task);

  g_cancellable_cancel (g_task_get_cancellable (task));
  /* send an ::aborted signal to make the dialog go away */
  if (data->mount_operation != NULL)
    g_signal_emit_by_name (data->mount_operation, "aborted");
}

/* ------------------------------ */

static void
mount_command_cb (GObject       *source_object,
                  GAsyncResult  *res,
                  gpointer       user_data)
{
  GTask *task = G_TASK (user_data);
  GVfsUDisks2Volume *volume = g_task_get_source_object (task);
  GError *error;
  gint exit_status;
  gchar *standard_error = NULL;

  /* NOTE: for e.g. NFS and CIFS mounts we could do GMountOperation stuff and pipe a
   * password to mount(8)'s stdin channel
   *
   * NOTE: if this fails because the user is not authorized (e.g. EPERM), we could
   * run it through a polkit-ified setuid root helper
   */

  error = NULL;
  if (!gvfs_udisks2_utils_spawn_finish (res,
                                        &exit_status,
                                        NULL, /* gchar **out_standard_output */
                                        &standard_error,
                                        &error))
    {
      g_task_return_error (task, error);
    }
  else if (WIFEXITED (exit_status) && WEXITSTATUS (exit_status) == 0)
    {
      gvfs_udisks2_volume_monitor_update (volume->monitor);
      g_task_return_boolean (task, TRUE);
    }
  else
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "%s", standard_error);

  g_object_unref (task);
  g_free (standard_error);
}

/* ------------------------------ */

static void
ensure_autoclear (GTask *task)
{
  GVfsUDisks2Volume *volume = g_task_get_source_object (task);
  UDisksLoop *loop;
  loop = udisks_client_get_loop_for_block (gvfs_udisks2_volume_monitor_get_udisks_client (volume->monitor),
                                           volume->block);
  if (loop != NULL)
    {
      if (!udisks_loop_get_autoclear (loop) && udisks_loop_get_setup_by_uid (loop) == getuid ())
        {
          /* we don't care about the result */
          udisks_loop_call_set_autoclear (loop, TRUE,
                                          g_variant_new ("a{sv}", NULL), /* options */
                                          NULL, NULL, NULL);
        }
      g_object_unref (loop);
    }
}

/* ------------------------------ */


static void
mount_cb (GObject       *source_object,
          GAsyncResult  *res,
          gpointer       user_data)
{
  GTask *task = G_TASK (user_data);
  GVfsUDisks2Volume *volume = g_task_get_source_object (task);
  gchar *mount_path;
  GError *error;

  error = NULL;
  if (!udisks_filesystem_call_mount_finish (UDISKS_FILESYSTEM (source_object),
                                            &mount_path,
                                            res,
                                            &error))
    {
      gvfs_udisks2_utils_udisks_error_to_gio_error (error);
      g_task_return_error (task, error);
    }
  else
    {
      /* if mounting worked, ensure that the loop device goes away when unmounted */
      ensure_autoclear (task);

      gvfs_udisks2_volume_monitor_update (volume->monitor);
      g_task_return_boolean (task, TRUE);
      g_free (mount_path);
    }
  g_object_unref (task);
}

static void
do_mount (GTask *task)
{
  MountData *data = g_task_get_task_data (task);
  GVariantBuilder builder;
  GVfsUDisks2Volume *volume = g_task_get_source_object (task);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (data->mount_operation == NULL)
    {
      g_variant_builder_add (&builder,
                             "{sv}",
                             "auth.no_user_interaction", g_variant_new_boolean (TRUE));
    }
  if (gvfs_udisks2_volume_monitor_get_readonly_lockdown (volume->monitor))
    {
      g_variant_builder_add (&builder,
                             "{sv}",
                             "options", g_variant_new_string ("ro"));

    }
  udisks_filesystem_call_mount (data->filesystem_to_mount,
                                g_variant_builder_end (&builder),
                                g_task_get_cancellable (task),
                                mount_cb,
                                task);
}

/* ------------------------------ */

#ifdef HAVE_KEYRING
static void
luks_store_passphrase_cb (GObject      *source,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  GTask *task = G_TASK (user_data);
  GError *error = NULL;

  if (secret_password_store_finish (result, &error))
    {
      /* everything is good */
      do_mount (task);
    }
  else
    {
      /* report failure */
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               _("Error storing passphrase in keyring (%s)"),
                               error->message);
      g_object_unref (task);
    }
}
#endif


static void do_unlock (GTask *task);


#ifdef HAVE_KEYRING
static void
luks_delete_passphrase_cb (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  GTask *task = G_TASK (user_data);
  MountData *data = g_task_get_task_data (task);
  GError *error = NULL;

  secret_password_clear_finish (result, &error);
  if (!error)
    {
      /* with the bad passphrase out of the way, try again */
      g_free (data->passphrase);
      data->passphrase = NULL;
      do_unlock (task);
    }
  else
    {
      /* report failure */
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               _("Error deleting invalid passphrase from keyring (%s)"),
                               error->message);
      g_object_unref (task);
    }
}
#endif

static void
unlock_cb (GObject       *source_object,
           GAsyncResult  *res,
           gpointer       user_data)
{
  GTask *task = G_TASK (user_data);
  MountData *data = g_task_get_task_data (task);
  GVfsUDisks2Volume *volume = g_task_get_source_object (task);
  gchar *cleartext_device = NULL;
  GError *error;

  error = NULL;
  if (!udisks_encrypted_call_unlock_finish (UDISKS_ENCRYPTED (source_object),
                                            &cleartext_device,
                                            res,
                                            &error))
    {
#ifdef HAVE_KEYRING
      /* If this failed with a passphrase read from the keyring, try again
       * this time prompting the user...
       *
       * TODO: ideally check against something like UDISKS_ERROR_PASSPHRASE_INVALID
       * when such a thing is available in udisks
       */
      if (data->passphrase_from_keyring != NULL &&
          g_strcmp0 (data->passphrase, data->passphrase_from_keyring) == 0)
        {
          /* nuke the invalid passphrase from keyring... */
          secret_password_clear (&luks_passphrase_schema, g_task_get_cancellable (task),
                                 luks_delete_passphrase_cb, task,
                                 "gvfs-luks-uuid", data->uuid_of_encrypted_to_unlock,
                                 NULL); /* sentinel */
          goto out;
        }
#endif
      gvfs_udisks2_utils_udisks_error_to_gio_error (error);
      g_task_return_error (task, error);
      g_object_unref (task);
      goto out;
    }
  else
    {
      UDisksObject *object;

      /* if unlocking worked, ensure that the loop device goes away when locked */
      ensure_autoclear (task);

      gvfs_udisks2_volume_monitor_update (volume->monitor);

      object = udisks_client_peek_object (gvfs_udisks2_volume_monitor_get_udisks_client (volume->monitor),
                                          cleartext_device);
      data->filesystem_to_mount = object != NULL ? udisks_object_get_filesystem (object) : NULL;
      if (data->filesystem_to_mount == NULL)
        {
          g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   _("The unlocked device does not have a recognizable file system on it"));
          g_object_unref (task);
          goto out;
        }

#ifdef HAVE_KEYRING
      /* passphrase worked - save it in the keyring if requested */
      if (data->password_save != G_PASSWORD_SAVE_NEVER)
        {
          const gchar *keyring;
          gchar *display_name;

          switch (data->password_save)
            {
            case G_PASSWORD_SAVE_NEVER:
              g_assert_not_reached ();
              break;
            case G_PASSWORD_SAVE_FOR_SESSION:
              keyring = SECRET_COLLECTION_SESSION;
              break;
            case G_PASSWORD_SAVE_PERMANENTLY:
              keyring = SECRET_COLLECTION_DEFAULT;
              break;
            default:
              keyring = SECRET_COLLECTION_DEFAULT;
              break;
            }

          display_name = g_strdup_printf (_("Encryption passphrase for %s"),
                                          data->desc_of_encrypted_to_unlock);

          secret_password_store (&luks_passphrase_schema,
                                 keyring, display_name, data->passphrase,
                                 g_task_get_cancellable (task),
                                 luks_store_passphrase_cb, task,
                                 "gvfs-luks-uuid", data->uuid_of_encrypted_to_unlock,
                                 NULL); /* sentinel */
          goto out;
        }
#endif

      /* OK, ready to rock */
      do_mount (task);
    }

 out:
  g_free (cleartext_device);
}

static void
on_mount_operation_reply (GMountOperation       *mount_operation,
                          GMountOperationResult result,
                          gpointer              user_data)
{
  GTask *task = G_TASK (user_data);
  MountData *data = g_task_get_task_data (task);
  GVfsUDisks2Volume *volume = g_task_get_source_object (task);

  /* we got what we wanted; don't listen to any other signals from the mount operation */
  if (data->mount_operation_reply_handler_id != 0)
    {
      g_signal_handler_disconnect (data->mount_operation, data->mount_operation_reply_handler_id);
      data->mount_operation_reply_handler_id = 0;
    }
  if (data->mount_operation_aborted_handler_id != 0)
    {
      g_signal_handler_disconnect (data->mount_operation, data->mount_operation_aborted_handler_id);
      data->mount_operation_aborted_handler_id = 0;
    }

  if (result != G_MOUNT_OPERATION_HANDLED)
    {
      if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (data->mount_operation), "x-udisks2-is-unlocked")) == 1)
        {
          UDisksClient *client;
          UDisksBlock *cleartext_block;

          g_object_set_data (G_OBJECT (data->mount_operation), "x-udisks2-is-unlocked", GINT_TO_POINTER (0));

          client = gvfs_udisks2_volume_monitor_get_udisks_client (volume->monitor);
          cleartext_block = udisks_client_get_cleartext_block (client, volume->block);
          if (cleartext_block != NULL)
            {
              UDisksObject *object;

              object = UDISKS_OBJECT (g_dbus_interface_get_object (G_DBUS_INTERFACE (cleartext_block)));
              g_object_unref (cleartext_block);
              if (object != NULL)
                {
                  data->filesystem_to_mount = udisks_object_get_filesystem (object);
                  if (data->filesystem_to_mount != NULL)
                    {
                      do_mount (task);
                      return;
                    }
                  else
                    {
                      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                               "No filesystem interface on D-Bus object for cleartext device");
                    }
                }
              else
                {
                  g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                           "No object for D-Bus interface");
                }
            }
        }
      else if (result == G_MOUNT_OPERATION_ABORTED)
        {
          /* The user aborted the operation so consider it "handled" */
          g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED,
                                   "Password dialog aborted");
        }
      else
        {
          g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                   "Expected G_MOUNT_OPERATION_HANDLED but got %d", result);
        }
      g_object_unref (task);
      return;
    }

  data->passphrase = g_strdup (g_mount_operation_get_password (mount_operation));
  data->password_save = g_mount_operation_get_password_save (mount_operation);
  data->hidden_volume = g_mount_operation_get_is_tcrypt_hidden_volume (mount_operation);
  data->system_volume = g_mount_operation_get_is_tcrypt_system_volume (mount_operation);
  data->pim = g_mount_operation_get_pim (mount_operation);

  /* Don't save password in keyring just yet - check if it works first */

  do_unlock (task);
}

static void
on_mount_operation_aborted (GMountOperation       *mount_operation,
                            gpointer              user_data)
{
  on_mount_operation_reply (mount_operation, G_MOUNT_OPERATION_ABORTED, user_data);
}

static gboolean
has_crypttab_passphrase (GTask *task)
{
  GVfsUDisks2Volume *volume = g_task_get_source_object (task);
  gboolean ret = FALSE;
  GVariantIter iter;
  GVariant *configuration_value;
  const gchar *configuration_type;

  g_variant_iter_init (&iter, udisks_block_get_configuration (volume->block));
  while (g_variant_iter_next (&iter, "(&s@a{sv})", &configuration_type, &configuration_value))
    {
      if (g_strcmp0 (configuration_type, "crypttab") == 0)
        {
          const gchar *passphrase_path;
          if (g_variant_lookup (configuration_value, "passphrase-path", "^&ay", &passphrase_path))
            {
              if (passphrase_path != NULL && strlen (passphrase_path) > 0)
                {
                  ret = TRUE;
                  g_variant_unref (configuration_value);
                  goto out;
                }
            }
        }
      g_variant_unref (configuration_value);
    }
 out:
  return ret;
}

#ifdef HAVE_KEYRING
static void
luks_find_passphrase_cb (GObject      *source,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  GTask *task = G_TASK (user_data);
  MountData *data = g_task_get_task_data (task);
  gchar *password;

  password = secret_password_lookup_finish (result, NULL);

  /* Don't fail if a keyring error occured - just continue and request
   * the passphrase from the user...
   */
  if (password)
    {
      data->passphrase = password;
      data->passphrase_from_keyring = g_strdup (password);
    }
  /* try again */
  do_unlock (task);
}
#endif

static void
do_unlock (GTask *task)
{
  MountData *data = g_task_get_task_data (task);
  GVfsUDisks2Volume *volume = g_task_get_source_object (task);
  GVariantBuilder builder;
  gboolean handle_as_tcrypt;
  const gchar *type = udisks_block_get_id_type (volume->block);

  handle_as_tcrypt = (g_strcmp0 (type, "crypto_TCRYPT") == 0 ||
                      g_strcmp0 (type, "crypto_unknown") == 0);

  if (data->passphrase == NULL)
    {
      /* If the passphrase is in the crypttab file, no need to ask the user, just use a blank passphrase */
      if (has_crypttab_passphrase (task))
        {
          data->passphrase = g_strdup ("");
        }
      else
        {
          gchar *message;
          GAskPasswordFlags pw_ask_flags;

#ifdef HAVE_KEYRING
          /* check if the passphrase is in the user's keyring */
          if (!data->checked_keyring)
            {
              data->checked_keyring = TRUE;
              secret_password_lookup (&luks_passphrase_schema, g_task_get_cancellable (task),
                                      luks_find_passphrase_cb, task,
                                      "gvfs-luks-uuid", data->uuid_of_encrypted_to_unlock,
                                      NULL); /* sentinel */
              goto out;
            }
#endif

          if (data->mount_operation == NULL)
            {
              g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                       _("A passphrase is required to access the volume"));
              g_object_unref (task);
              goto out;
            }

          data->mount_operation_reply_handler_id = g_signal_connect (data->mount_operation,
                                                                     "reply",
                                                                     G_CALLBACK (on_mount_operation_reply),
                                                                     task);
          data->mount_operation_aborted_handler_id = g_signal_connect (data->mount_operation,
                                                                       "aborted",
                                                                       G_CALLBACK (on_mount_operation_aborted),
                                                                       task);
          if (g_strcmp0 (type, "crypto_unknown") == 0)
            /* Translators: %s is the description of the volume that is being unlocked */
            message = g_strdup_printf (_("Authentication Required\n"
                                         "A passphrase is needed to access encrypted data on “%s”.\n"
                                         "The volume might be a VeraCrypt volume as it contains random data."),
                                       data->desc_of_encrypted_to_unlock);
          else
            /* Translators: %s is the description of the volume that is being unlocked */
            message = g_strdup_printf (_("Authentication Required\n"
                                         "A passphrase is needed to access encrypted data on “%s”."),
                                       data->desc_of_encrypted_to_unlock);

          pw_ask_flags = G_ASK_PASSWORD_NEED_PASSWORD | G_ASK_PASSWORD_SAVING_SUPPORTED;
          if (handle_as_tcrypt)
            pw_ask_flags |= G_ASK_PASSWORD_TCRYPT;

          /* NOTE: We (currently) don't offer the user to save the
           * passphrase in the keyring or /etc/crypttab - compared to
           * the gdu volume monitor (that used the keyring for this)
           * this is a "regression" but it's done this way on purpose
           * because
           *
           *  - if the device is encrypted, it was probably the intent
           *    that the passphrase is to be used every time it's used
           *
           *  - supporting both /etc/crypttab and the keyring is confusing
           *    and leaves the user to wonder where the key is stored.
           *
           *  - the user can add an /etc/crypttab entry himself either
           *    manually or through palimpsest
           */
          g_signal_emit_by_name (data->mount_operation,
                                 "ask-password",
                                 message,
                                 NULL,
                                 NULL,
                                 pw_ask_flags);
          g_free (message);
          goto out;
        }
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (data->mount_operation == NULL)
    {
      g_variant_builder_add (&builder,
                             "{sv}",
                             "auth.no_user_interaction", g_variant_new_boolean (TRUE));
    }

  if (handle_as_tcrypt)
    {
      g_variant_builder_add (&builder,
                             "{sv}",
                             "hidden", g_variant_new_boolean (data->hidden_volume));
      g_variant_builder_add (&builder,
                             "{sv}",
                             "system", g_variant_new_boolean (data->system_volume));
      g_variant_builder_add (&builder,
                             "{sv}",
                             "pim", g_variant_new_uint32 (data->pim));
    }

  udisks_encrypted_call_unlock (data->encrypted_to_unlock,
                                data->passphrase,
                                g_variant_builder_end (&builder),
                                g_task_get_cancellable (task),
                                unlock_cb,
                                task);
 out:
  ;
}


static void
gvfs_udisks2_volume_mount (GVolume             *_volume,
                           GMountMountFlags     flags,
                           GMountOperation     *mount_operation,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  GDBusObject *object;
  UDisksBlock *block;
  UDisksClient *client;
  UDisksFilesystem *filesystem;
  MountData *data;
  GTask *task;

  client = gvfs_udisks2_volume_monitor_get_udisks_client (volume->monitor);

  task = g_task_new (volume, cancellable, callback, user_data);
  g_task_set_source_tag (task, gvfs_udisks2_volume_mount);

  data = g_new0 (MountData, 1);
  data->mount_operation = mount_operation != NULL ? g_object_ref (mount_operation) : NULL;

  g_task_set_task_data (task, data, (GDestroyNotify)mount_data_free);

  if (volume->mount_pending_op != NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "A mount operation is already pending");
      g_object_unref (task);
      return;
    }
  volume->mount_pending_op = task;

  /* Use the mount(8) command if there is no block device */
  if (volume->block == NULL)
    {
      gchar *escaped_mount_path;
      escaped_mount_path = g_strescape (g_unix_mount_point_get_mount_path (volume->mount_point), NULL);
      gvfs_udisks2_utils_spawn (10, /* timeout in seconds */
                                cancellable,
                                mount_command_cb,
                                task,
                                "mount \"%s\"",
                                escaped_mount_path);
      g_free (escaped_mount_path);
      return;
    }

  /* if encrypted and already unlocked, just mount the cleartext block device */
  block = udisks_client_get_cleartext_block (client, volume->block);
  if (block != NULL)
    g_object_unref (block);
  else
    block = volume->block;

  object = g_dbus_interface_get_object (G_DBUS_INTERFACE (block));
  if (object == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "No object for D-Bus interface");
      g_object_unref (task);
      return;
    }

  filesystem = udisks_object_peek_filesystem (UDISKS_OBJECT (object));
  if (filesystem == NULL)
    {
      data->encrypted_to_unlock = udisks_object_get_encrypted (UDISKS_OBJECT (object));
      if (data->encrypted_to_unlock != NULL)
        {
          UDisksDrive *udisks_drive;

          /* This description is used in both the prompt and the display-name of
           * the key stored in the user's keyring ...
           *
           * NOTE: we want a little bit more detail than what g_drive_get_name()
           * gives us, since this is going to be used to refer to the device even
           * when not plugged in
           */
          udisks_drive = udisks_client_get_drive_for_block (client, block);
          if (udisks_drive != NULL)
            {
              gchar *drive_name = NULL;
              gchar *drive_desc = NULL;

#if UDISKS_CHECK_VERSION(2,0,90)
              {
                UDisksObject *object = (UDisksObject *) g_dbus_interface_get_object (G_DBUS_INTERFACE (udisks_drive));
                if (object != NULL)
                  {
                    UDisksObjectInfo *info = udisks_client_get_object_info (client, object);
                    if (info != NULL)
                      {
                        drive_name = g_strdup (udisks_object_info_get_name (info));
                        drive_desc = g_strdup (udisks_object_info_get_description (info));
                        g_object_unref (info);
                      }
                  }
              }
#else
              udisks_client_get_drive_info (client,
                                            udisks_drive,
                                            &drive_name,
                                            &drive_desc,
                                            NULL,  /* drive_icon */
                                            NULL,  /* media_desc */
                                            NULL); /* media_icon */
#endif
              /* Translators: this is used to describe the drive the encrypted media
               * is on - the first %s is the name (such as 'WD 2500JB External'), the
               * second %s is the description ('250 GB Hard Disk').
               */
              data->desc_of_encrypted_to_unlock = g_strdup_printf (_("%s (%s)"),
                                                                   drive_name,
                                                                   drive_desc);
              g_free (drive_desc);
              g_free (drive_name);
              g_object_unref (udisks_drive);
            }
          else
            {
              UDisksLoop *loop = udisks_client_get_loop_for_block (client, block);
              if (loop != NULL)
                data->desc_of_encrypted_to_unlock = udisks_loop_dup_backing_file (loop);
              else
                data->desc_of_encrypted_to_unlock = udisks_block_dup_preferred_device (block);
            }
          data->uuid_of_encrypted_to_unlock = udisks_block_dup_id_uuid (block);

          do_unlock (task);
          return;
        }

      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "No .Filesystem or .Encrypted interface on D-Bus object");
      g_object_unref (task);
      return;
    }

  data->filesystem_to_mount = g_object_ref (filesystem);
  do_mount (task);
}

static gboolean
gvfs_udisks2_volume_mount_finish (GVolume       *_volume,
                                  GAsyncResult  *result,
                                  GError       **error)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);

  g_return_val_if_fail (g_task_is_valid (result, volume), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, gvfs_udisks2_volume_mount), FALSE);

  if (volume->mount_pending_op == G_TASK (result))
    volume->mount_pending_op = NULL;

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
eject_wrapper_callback (GObject      *source_object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  GTask *task = G_TASK (user_data);
  GError *error = NULL;

  if (g_drive_eject_with_operation_finish (G_DRIVE (source_object), res, &error))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}

static void
gvfs_udisks2_volume_eject_with_operation (GVolume              *_volume,
                                          GMountUnmountFlags   flags,
                                          GMountOperation     *mount_operation,
                                          GCancellable        *cancellable,
                                          GAsyncReadyCallback  callback,
                                          gpointer             user_data)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  GTask *task;

  task = g_task_new (_volume, cancellable, callback, user_data);
  g_task_set_source_tag (task, gvfs_udisks2_volume_eject_with_operation);

  if (volume->drive != NULL)
    {
      g_drive_eject_with_operation (G_DRIVE (volume->drive), flags, mount_operation, cancellable, eject_wrapper_callback, task);
    }
  else
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               _("Operation not supported by backend"));
      g_object_unref (task);
    }
}

static gboolean
gvfs_udisks2_volume_eject_with_operation_finish (GVolume        *_volume,
                                                 GAsyncResult  *result,
                                                 GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, _volume), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, gvfs_udisks2_volume_eject_with_operation), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
gvfs_udisks2_volume_eject (GVolume              *volume,
                           GMountUnmountFlags   flags,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
  gvfs_udisks2_volume_eject_with_operation (volume, flags, NULL, cancellable, callback, user_data);
}

static gboolean
gvfs_udisks2_volume_eject_finish (GVolume        *volume,
                                  GAsyncResult  *result,
                                  GError       **error)
{
  return gvfs_udisks2_volume_eject_with_operation_finish (volume, result, error);
}

/* ---------------------------------------------------------------------------------------------------- */

static const gchar *
gvfs_udisks2_volume_get_sort_key (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  return volume->sort_key;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
gvfs_udisks2_volume_volume_iface_init (GVolumeIface *iface)
{
  iface->get_name = gvfs_udisks2_volume_get_name;
  iface->get_icon = gvfs_udisks2_volume_get_icon;
  iface->get_symbolic_icon = gvfs_udisks2_volume_get_symbolic_icon;
  iface->get_uuid = gvfs_udisks2_volume_get_uuid;
  iface->get_drive = gvfs_udisks2_volume_get_drive;
  iface->get_mount = gvfs_udisks2_volume_get_mount;
  iface->can_mount = gvfs_udisks2_volume_can_mount;
  iface->can_eject = gvfs_udisks2_volume_can_eject;
  iface->should_automount = gvfs_udisks2_volume_should_automount;
  iface->get_activation_root = gvfs_udisks2_volume_get_activation_root;
  iface->enumerate_identifiers = gvfs_udisks2_volume_enumerate_identifiers;
  iface->get_identifier = gvfs_udisks2_volume_get_identifier;

  iface->mount_fn = gvfs_udisks2_volume_mount;
  iface->mount_finish = gvfs_udisks2_volume_mount_finish;
  iface->eject = gvfs_udisks2_volume_eject;
  iface->eject_finish = gvfs_udisks2_volume_eject_finish;
  iface->eject_with_operation = gvfs_udisks2_volume_eject_with_operation;
  iface->eject_with_operation_finish = gvfs_udisks2_volume_eject_with_operation_finish;
  iface->get_sort_key = gvfs_udisks2_volume_get_sort_key;
}

/* ---------------------------------------------------------------------------------------------------- */

UDisksBlock *
gvfs_udisks2_volume_get_block (GVfsUDisks2Volume *volume)
{
  g_return_val_if_fail (GVFS_IS_UDISKS2_VOLUME (volume), NULL);
  return volume->block;
}

GUnixMountPoint *
gvfs_udisks2_volume_get_mount_point (GVfsUDisks2Volume *volume)
{
  g_return_val_if_fail (GVFS_IS_UDISKS2_VOLUME (volume), NULL);
  return volume->mount_point;
}

dev_t
gvfs_udisks2_volume_get_dev (GVfsUDisks2Volume *volume)
{
  g_return_val_if_fail (GVFS_IS_UDISKS2_VOLUME (volume), 0);
  return volume->dev;
}

gboolean
gvfs_udisks2_volume_has_uuid (GVfsUDisks2Volume *volume,
                              const gchar       *uuid)
{
  g_return_val_if_fail (GVFS_IS_UDISKS2_VOLUME (volume), FALSE);
  return g_strcmp0 (volume->uuid, uuid) == 0;
}
