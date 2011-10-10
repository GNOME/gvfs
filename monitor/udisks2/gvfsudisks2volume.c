/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2011 Red Hat, Inc.
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

#include "gvfsudisks2drive.h"
#include "gvfsudisks2volume.h"
#include "gvfsudisks2mount.h"
#include "gvfsudisks2utils.h"

typedef struct _GVfsUDisks2VolumeClass GVfsUDisks2VolumeClass;

struct _GVfsUDisks2VolumeClass
{
  GObjectClass parent_class;
};

struct MountData;
typedef struct MountData MountData;

static void mount_cancel_pending_op (MountData *data);

struct _GVfsUDisks2Volume
{
  GObject parent;

  GVfsUDisks2VolumeMonitor *monitor; /* owned by volume monitor */
  GVfsUDisks2Mount         *mount;   /* owned by volume monitor */
  GVfsUDisks2Drive         *drive;   /* owned by volume monitor */

  /* exactly one of these are set */
  UDisksBlock *block;
  GUnixMountPoint *mount_point;

  /* set in update_volume() */
  GIcon *icon;
  GFile *activation_root;
  gchar *name;
  gchar *device_file;
  dev_t dev;
  gchar *uuid;
  gboolean can_mount;
  gboolean should_automount;

  /* If a mount operation is in progress, then pending_mount_op is != NULL. This
   * is used to cancel the operation to make possible authentication dialogs go
   * away.
   */
  MountData *mount_pending_op;
};

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

  if (volume->icon != NULL)
    g_object_unref (volume->icon);
  if (volume->activation_root != NULL)
    g_object_unref (volume->activation_root);

  g_free (volume->name);
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

  name = gvfs_udisks2_utils_lookup_fstab_options_value (fstab_options, "comment=gvfs.name=");
  if (name != NULL)
    {
      g_free (volume->name);
      volume->name = name;
    }

  icon_name = gvfs_udisks2_utils_lookup_fstab_options_value (fstab_options, "comment=gvfs.icon_name=");
  if (icon_name != NULL)
    {
      volume->icon = g_themed_icon_new_with_default_fallbacks (icon_name);
      g_free (icon_name);
    }
}

static gboolean
update_volume (GVfsUDisks2Volume *volume)
{
  gboolean changed;
  gboolean old_can_mount;
  gboolean old_should_automount;
  gchar *old_name;
  gchar *old_device_file;
  dev_t old_dev;
  GIcon *old_icon;
  UDisksDrive *udisks_drive;
  gchar *s;

  /* ---------------------------------------------------------------------------------------------------- */
  /* save old values */

  old_can_mount = volume->can_mount;
  old_should_automount = volume->should_automount;
  old_name = g_strdup (volume->name);
  old_device_file = g_strdup (volume->device_file);
  old_dev = volume->dev;
  old_icon = volume->icon != NULL ? g_object_ref (volume->icon) : NULL;

  /* ---------------------------------------------------------------------------------------------------- */
  /* reset */

  volume->can_mount = volume->should_automount = FALSE;
  g_free (volume->name); volume->name = NULL;
  g_free (volume->device_file); volume->device_file = NULL;
  volume->dev = 0;
  g_clear_object (&volume->icon);

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

      /* If unlocked, use the values from the unlocked block device for presentation */
      cleartext_block = udisks_client_get_cleartext_block (gvfs_udisks2_volume_monitor_get_udisks_client (volume->monitor),
                                                           volume->block);
      if (cleartext_block != NULL)
        block = cleartext_block;
      else
        block = volume->block;

      volume->dev = makedev (udisks_block_get_major (block), udisks_block_get_minor (block));
      volume->device_file = udisks_block_dup_device (block);

      if (strlen (udisks_block_get_id_label (block)) > 0)
        {
          volume->name = g_strdup (udisks_block_get_id_label (block));
        }
      else if (g_strcmp0 (udisks_block_get_id_type (block), "crypto_LUKS") == 0)
        {
          s = udisks_util_get_size_for_display (udisks_block_get_size (volume->block), FALSE, FALSE);
          /* Translators: This is used for encrypted volumes.
           *              The first %s is the formatted size (e.g. "42.0 MB").
           */
          volume->name = g_strdup_printf (_("%s Encrypted"), s);
          g_free (s);
        }
      else
        {
          guint64 size = udisks_block_get_size (block);
          if (size > 0)
            {
              s = udisks_util_get_size_for_display (size, FALSE, FALSE);
              /* Translators: This is used for volume with no filesystem label.
               *              The first %s is the formatted size (e.g. "42.0 MB").
               */
              volume->name = g_strdup_printf (_("%s Volume"), s);
              g_free (s);
            }
        }

      udisks_drive = udisks_client_get_drive_for_block (gvfs_udisks2_volume_monitor_get_udisks_client (volume->monitor),
                                                        volume->block);
      if (udisks_drive != NULL)
        {
          gchar *drive_desc;
          GIcon *drive_icon;
          gchar *media_desc;
          GIcon *media_icon;
          udisks_client_get_drive_info (gvfs_udisks2_volume_monitor_get_udisks_client (volume->monitor),
                                        udisks_drive,
                                        NULL, /* drive_name */
                                        &drive_desc,
                                        &drive_icon,
                                        &media_desc,
                                        &media_icon);
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

          /* Override name for blank and audio discs */
          if (udisks_drive_get_optical_blank (udisks_drive))
            {
              g_free (volume->name);
              volume->name = g_strdup (media_desc);
            }
          else if (volume->activation_root != NULL && g_file_has_uri_scheme (volume->activation_root, "cdda"))
            {
              g_free (volume->name);
              volume->name = g_strdup (_("Audio Disc"));
            }

          volume->icon = media_icon != NULL ? g_object_ref (media_icon) : NULL;

          /* use media_desc if we haven't figured out a name yet (applies to e.g.
           * /dev/fd0 since its size is 0)
           */
          if (volume->name == NULL)
            {
              volume->name = media_desc;
              media_desc = NULL;
            }

          g_free (media_desc);
          if (media_icon != NULL)
            g_object_unref (media_icon);

          g_object_unref (udisks_drive);
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

      /* Use comment=gvfs.name=The%20Name and comment=gvfs.icon_name=foo-name, if available */
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
      if (g_strcmp0 (udisks_block_get_id_type (volume->block), "crypto_LUKS") == 0 && volume->icon != NULL)
        {
          GEmblem *emblem;
          GIcon *padlock;
          GIcon *emblemed_icon;
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
    }
  else
    {
      apply_options_from_fstab (volume, g_unix_mount_point_get_options (volume->mount_point));
      if (volume->name == NULL)
        volume->name = g_unix_mount_point_guess_name (volume->mount_point);
      if (volume->icon == NULL)
        volume->icon = gvfs_udisks2_utils_icon_from_fs_type (g_unix_mount_point_get_fs_type (volume->mount_point));
    }

  /* ---------------------------------------------------------------------------------------------------- */
  /* fallbacks */

  if (volume->name == NULL)
    {
      /* Translators: Name used for volume */
      volume->name = g_strdup (_("Volume"));
    }
  if (volume->icon == NULL)
    volume->icon = g_themed_icon_new ("drive-removable-media");

  /* ---------------------------------------------------------------------------------------------------- */
  /* compute whether something changed */

  changed = !((old_can_mount == volume->can_mount) &&
              (old_should_automount == volume->should_automount) &&
              (g_strcmp0 (old_name, volume->name) == 0) &&
              (g_strcmp0 (old_device_file, volume->device_file) == 0) &&
              (old_dev == volume->dev) &&
              g_icon_equal (old_icon, volume->icon)
              );

  /* ---------------------------------------------------------------------------------------------------- */
  /* free old values */

  g_free (old_name);
  g_free (old_device_file);
  if (old_icon != NULL)
    g_object_unref (old_icon);

  return changed;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_block_changed (GObject    *object,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (user_data);
  if (update_volume (volume))
    emit_changed (volume);
}

static void
on_udisks_client_changed (UDisksClient *client,
                          gpointer      user_data)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (user_data);
  /* This is a little too broad - technically we only need to do this if our block
   * device has gained or lost a cleartext device...
   */
  if (update_volume (volume))
    emit_changed (volume);
}

/* takes ownership of @mount_point if not NULL */
GVfsUDisks2Volume *
gvfs_udisks2_volume_new (GVfsUDisks2VolumeMonitor   *monitor,
                         UDisksBlock                *block,
                         GUnixMountPoint            *mount_point,
                         GVfsUDisks2Drive           *drive,
                         GFile                      *activation_root)
{
  GVfsUDisks2Volume *volume;

  volume = g_object_new (GVFS_TYPE_UDISKS2_VOLUME, NULL);
  volume->monitor = monitor;

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

  /* For LUKS devices, we also need to listen for changes on any possible cleartext device */
  if (volume->block != NULL && g_strcmp0 (udisks_block_get_id_type (volume->block), "crypto_LUKS") == 0)
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
gvfs_udisks2_volume_get_drive (GVolume *volume)
{
  GVfsUDisks2Volume *gdu_volume = GVFS_UDISKS2_VOLUME (volume);
  GDrive *drive = NULL;

  if (gdu_volume->drive != NULL)
    drive = g_object_ref (gdu_volume->drive);
  return drive;
}

static GMount *
gvfs_udisks2_volume_get_mount (GVolume *volume)
{
  GVfsUDisks2Volume *gdu_volume = GVFS_UDISKS2_VOLUME (volume);
  GMount *mount = NULL;

  if (gdu_volume->mount != NULL)
    mount = g_object_ref (gdu_volume->mount);
  return mount;
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

  return ret;
}

static gchar **
gvfs_udisks2_volume_enumerate_identifiers (GVolume *_volume)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  GPtrArray *p;

  p = g_ptr_array_new ();
  g_ptr_array_add (p, g_strdup (G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE));

  if (volume->block != NULL)
    {
      const gchar *label;
      const gchar *uuid;
      label = udisks_block_get_id_label (volume->block);
      uuid = udisks_block_get_id_uuid (volume->block);
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

struct MountData
{
  GSimpleAsyncResult *simple;

  GVfsUDisks2Volume *volume;
  GCancellable *cancellable;

  gulong mount_operation_reply_handler_id;
  gulong mount_operation_aborted_handler_id;
  GMountOperation *mount_operation;

  gchar *passphrase;

  UDisksEncrypted *encrypted_to_unlock;
  UDisksFilesystem *filesystem_to_mount;

};

static void
mount_data_free (MountData *data)
{
  if (data->volume->mount_pending_op == data)
    data->volume->mount_pending_op = NULL;

  g_object_unref (data->simple);

  g_clear_object (&data->volume);
  g_clear_object (&data->cancellable);

  if (data->mount_operation != NULL)
    {
      if (data->mount_operation_reply_handler_id != 0)
        g_signal_handler_disconnect (data->mount_operation, data->mount_operation_reply_handler_id);
      if (data->mount_operation_aborted_handler_id != 0)
        g_signal_handler_disconnect (data->mount_operation, data->mount_operation_aborted_handler_id);
      g_object_unref (data->mount_operation);
    }

  g_free (data->passphrase);

  g_clear_object (&data->encrypted_to_unlock);
  g_clear_object (&data->filesystem_to_mount);
  g_free (data);
}

static void
mount_cancel_pending_op (MountData *data)
{
  g_cancellable_cancel (data->cancellable);
  /* send an ::aborted signal to make the dialog go away */
  if (data->mount_operation != NULL)
    g_signal_emit_by_name (data->mount_operation, "aborted");
}

/* ------------------------------ */

static void
do_mount_command (MountData *data)
{
  GError *error;
  gint exit_status;
  gchar *standard_error = NULL;
  const gchar *mount_argv[3] = {"mount", NULL, NULL};

  mount_argv[1] = g_unix_mount_point_get_mount_path (data->volume->mount_point);

  /* TODO: we could do this async but it's probably not worth the effort */
  error = NULL;
  if (!g_spawn_sync (NULL,            /* working dir */
                     (gchar **) mount_argv,
                     NULL,            /* envp */
                     G_SPAWN_SEARCH_PATH,
                     NULL,            /* child_setup */
                     NULL,            /* user_data for child_setup */
                     NULL,            /* standard_output */
                     &standard_error, /* standard_error */
                     &exit_status,
                     &error))
    {
      g_prefix_error (&error, "Error running mount: ");
      g_simple_async_result_take_error (data->simple, error);
      g_simple_async_result_complete (data->simple);
      mount_data_free (data);
      goto out;
    }

  /* TODO: for e.g. NFS and CIFS mounts we could do GMountOperation stuff and pipe a
   * password to mount(8)'s stdin channel
   *
   * TODO: if this fails because the user is not authorized (e.g. EPERM), we could
   * run it through a polkit-ified setuid root helper
   */

  if (WIFEXITED (exit_status) && WEXITSTATUS (exit_status) == 0)
    {
      gvfs_udisks2_volume_monitor_update (data->volume->monitor);
      g_simple_async_result_complete (data->simple);
      mount_data_free (data);
      goto out;
    }

  g_simple_async_result_set_error (data->simple,
                                   G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   standard_error);
  g_simple_async_result_complete (data->simple);
  mount_data_free (data);

 out:
  g_free (standard_error);
}

/* ------------------------------ */

static void
mount_cb (GObject       *source_object,
          GAsyncResult  *res,
          gpointer       user_data)
{
  MountData *data = user_data;
  gchar *mount_path;
  GError *error;

  error = NULL;
  if (!udisks_filesystem_call_mount_finish (UDISKS_FILESYSTEM (source_object),
                                            &mount_path,
                                            res,
                                            &error))
    {
      gvfs_udisks2_utils_udisks_error_to_gio_error (error);
      g_simple_async_result_take_error (data->simple, error);
      g_simple_async_result_complete (data->simple);
    }
  else
    {
      gvfs_udisks2_volume_monitor_update (data->volume->monitor);
      g_simple_async_result_complete (data->simple);
      g_free (mount_path);
    }
  mount_data_free (data);
}

static void
do_mount (MountData *data)
{
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (data->mount_operation == NULL)
    {
      g_variant_builder_add (&builder,
                             "{sv}",
                             "auth.no_user_interaction", g_variant_new_boolean (TRUE));
    }
  udisks_filesystem_call_mount (data->filesystem_to_mount,
                                g_variant_builder_end (&builder),
                                data->cancellable,
                                mount_cb,
                                data);
}

/* ------------------------------ */

static void
unlock_cb (GObject       *source_object,
           GAsyncResult  *res,
           gpointer       user_data)
{
  MountData *data = user_data;
  gchar *cleartext_device = NULL;
  GError *error;

  error = NULL;
  if (!udisks_encrypted_call_unlock_finish (UDISKS_ENCRYPTED (source_object),
                                            &cleartext_device,
                                            res,
                                            &error))
    {
      gvfs_udisks2_utils_udisks_error_to_gio_error (error);
      g_simple_async_result_take_error (data->simple, error);
      g_simple_async_result_complete (data->simple);
      mount_data_free (data);
      goto out;
    }
  else
    {
      UDisksObject *object;

      gvfs_udisks2_volume_monitor_update (data->volume->monitor);

      object = udisks_client_peek_object (gvfs_udisks2_volume_monitor_get_udisks_client (data->volume->monitor),
                                          cleartext_device);
      data->filesystem_to_mount = object != NULL ? udisks_object_get_filesystem (object) : NULL;
      if (data->filesystem_to_mount == NULL)
        {
          g_simple_async_result_set_error (data->simple,
                                           G_IO_ERROR,
                                           G_IO_ERROR_FAILED,
                                           _("The unlocked device does not have a recognizable filesystem on it"));
          g_simple_async_result_complete (data->simple);
          mount_data_free (data);
          goto out;
        }

      /* TODO: save in keyring depending on ASK_SAVE */

      /* OK, ready to rock */
      do_mount (data);
    }

 out:
  g_free (cleartext_device);
}

static void do_unlock (MountData *data);

static void
on_mount_operation_reply (GMountOperation       *mount_operation,
                          GMountOperationResult result,
                          gpointer              user_data)
{
  MountData *data = user_data;

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
      mount_data_free (data);
      goto out;
    }

  data->passphrase = g_strdup (g_mount_operation_get_password (mount_operation));

  /* TODO: check ASK_SAVE */
  do_unlock (data);

 out:
  ;
}

static void
on_mount_operation_aborted (GMountOperation       *mount_operation,
                            gpointer              user_data)
{
  on_mount_operation_reply (mount_operation, G_MOUNT_OPERATION_ABORTED, user_data);
}

static void
do_unlock (MountData *data)
{
  GVariantBuilder builder;

  /* TODO: lookup passphrase in keyring */

  if (data->passphrase == NULL)
    {
      gchar *message;

      if (data->mount_operation == NULL)
        {
          g_simple_async_result_set_error (data->simple,
                                           G_IO_ERROR,
                                           G_IO_ERROR_FAILED,
                                           _("A passphrase is required to access the volume"));
          g_simple_async_result_complete (data->simple);
          mount_data_free (data);
          goto out;
        }

      data->mount_operation_reply_handler_id = g_signal_connect (data->mount_operation,
                                                                 "reply",
                                                                 G_CALLBACK (on_mount_operation_reply),
                                                                 data);
      data->mount_operation_aborted_handler_id = g_signal_connect (data->mount_operation,
                                                                   "aborted",
                                                                   G_CALLBACK (on_mount_operation_aborted),
                                                                   data);
      message = g_strdup_printf (_("Enter a password to unlock the volume\n"
                                   "The device %s contains encrypted data."),
                                 udisks_block_get_device (data->volume->block));
      g_signal_emit_by_name (data->mount_operation,
                             "ask-password",
                             message,
                             NULL,
                             NULL,
                             G_ASK_PASSWORD_NEED_PASSWORD |
                             0/*G_ASK_PASSWORD_SAVING_SUPPORTED*/);
      g_free (message);
      goto out;
    }


  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  if (data->mount_operation == NULL)
    {
      g_variant_builder_add (&builder,
                             "{sv}",
                             "auth.no_user_interaction", g_variant_new_boolean (TRUE));
    }
  udisks_encrypted_call_unlock (data->encrypted_to_unlock,
                                data->passphrase,
                                g_variant_builder_end (&builder),
                                data->cancellable,
                                unlock_cb,
                                data);
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
  UDisksFilesystem *filesystem;
  MountData *data;

  data = g_new0 (MountData, 1);
  data->simple = g_simple_async_result_new (G_OBJECT (volume),
                                            callback,
                                            user_data,
                                            gvfs_udisks2_volume_mount);
  data->volume = g_object_ref (volume);
  data->cancellable = cancellable != NULL ? g_object_ref (cancellable) : NULL;
  data->mount_operation = mount_operation != NULL ? g_object_ref (mount_operation) : NULL;

  if (volume->mount_pending_op != NULL)
    {
      g_simple_async_result_set_error (data->simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_FAILED,
                                       "A mount operation is already pending");
      g_simple_async_result_complete (data->simple);
      mount_data_free (data);
      goto out;
    }
  volume->mount_pending_op = data;

  if (volume->block == NULL)
    {
      do_mount_command (data);
      goto out;
    }

  /* if encrypted and already unlocked, just mount the cleartext block device */
  block = udisks_client_get_cleartext_block (gvfs_udisks2_volume_monitor_get_udisks_client (volume->monitor),
                                             volume->block);
  if (block != NULL)
    g_object_unref (block);
  else
    block = volume->block;

  object = g_dbus_interface_get_object (G_DBUS_INTERFACE (block));
  if (object == NULL)
    {
      g_simple_async_result_set_error (data->simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_FAILED,
                                       "No object for D-Bus interface");
      g_simple_async_result_complete (data->simple);
      mount_data_free (data);
      goto out;
    }

  filesystem = udisks_object_peek_filesystem (UDISKS_OBJECT (object));
  if (filesystem == NULL)
    {
      data->encrypted_to_unlock = udisks_object_get_encrypted (UDISKS_OBJECT (object));
      if (data->encrypted_to_unlock != NULL)
        {
          do_unlock (data);
          goto out;
        }

      g_simple_async_result_set_error (data->simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_FAILED,
                                       "No .Filesystem or .Encrypted interface on D-Bus object");
      g_simple_async_result_complete (data->simple);
      mount_data_free (data);
      goto out;
    }

  data->filesystem_to_mount = g_object_ref (filesystem);
  do_mount (data);

 out:
  ;
}

static gboolean
gvfs_udisks2_volume_mount_finish (GVolume       *volume,
                                  GAsyncResult  *result,
                                  GError       **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
  return !g_simple_async_result_propagate_error (simple, error);
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
} EjectWrapperOp;

static void
eject_wrapper_callback (GObject      *source_object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  EjectWrapperOp *data  = user_data;
  data->callback (data->object, res, data->user_data);
  g_object_unref (data->object);
  g_free (data);
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
  GVfsUDisks2Drive *drive;

  drive = NULL;
  if (volume->drive != NULL)
    drive = g_object_ref (volume->drive);

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
gvfs_udisks2_volume_eject_with_operation_finish (GVolume        *_volume,
                                                 GAsyncResult  *result,
                                                 GError       **error)
{
  GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (_volume);
  gboolean ret = TRUE;

  if (volume->drive != NULL)
    {
      ret = g_drive_eject_with_operation_finish (G_DRIVE (volume->drive), result, error);
    }
  else
    {
      g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);
      ret = FALSE;
    }
  return ret;
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

static void
gvfs_udisks2_volume_volume_iface_init (GVolumeIface *iface)
{
  iface->get_name = gvfs_udisks2_volume_get_name;
  iface->get_icon = gvfs_udisks2_volume_get_icon;
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
