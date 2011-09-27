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

#include "gvfsudisks2volumemonitor.h"
#include "gvfsudisks2drive.h"
#include "gvfsudisks2volume.h"

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

  UDisksDrive *udisks_drive;

  GIcon *icon;
  gchar *name;
  gchar *device_file;
  dev_t dev;
  gboolean is_media_removable;
  gboolean has_media;
  gboolean can_eject;
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

  if (drive->icon != NULL)
    g_object_unref (drive->icon);
  g_free (drive->name);
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
gvfs_udisks2_drive_init (GVfsUDisks2Drive *gdu_drive)
{
}

static void
emit_changed (GVfsUDisks2Drive *drive)
{
  g_signal_emit_by_name (drive, "changed");
  g_signal_emit_by_name (drive->monitor, "drive-changed", drive);
}

static gboolean
update_drive (GVfsUDisks2Drive *drive)
{
  gboolean changed;
  GIcon *old_icon;
  gchar *old_name;
  gchar *old_device_file;
  dev_t old_dev;
  gboolean old_is_media_removable;
  gboolean old_has_media;
  gboolean old_can_eject;
  UDisksBlock *block;

  /* ---------------------------------------------------------------------------------------------------- */
  /* save old values */

  old_is_media_removable = drive->is_media_removable;
  old_has_media = drive->has_media;
  old_can_eject = drive->can_eject;

  old_name = g_strdup (drive->name);
  old_device_file = g_strdup (drive->device_file);
  old_dev = drive->dev;
  old_icon = drive->icon != NULL ? g_object_ref (drive->icon) : NULL;

  /* ---------------------------------------------------------------------------------------------------- */
  /* reset */

  drive->is_media_removable = drive->has_media = drive->can_eject = FALSE;
  g_free (drive->name); drive->name = NULL;
  g_free (drive->device_file); drive->device_file = NULL;
  drive->dev = 0;
  g_clear_object (&drive->icon);

  /* ---------------------------------------------------------------------------------------------------- */
  /* in with the new */

  block = udisks_client_get_block_for_drive (gvfs_udisks2_volume_monitor_get_udisks_client (drive->monitor),
                                             drive->udisks_drive,
                                             FALSE);
  if (block != NULL)
    {
      drive->device_file = udisks_block_dup_device (block);
      drive->dev = makedev (udisks_block_get_major (block), udisks_block_get_minor (block));
      g_object_unref (block);
    }

  drive->is_media_removable = udisks_drive_get_media_removable (drive->udisks_drive);
  if (drive->is_media_removable)
    {
      drive->has_media = (udisks_drive_get_size (drive->udisks_drive) > 0);
      drive->can_eject = TRUE;
    }
  else
    {
      drive->has_media = TRUE;
      drive->can_eject = FALSE;
    }

  udisks_util_get_drive_info (drive->udisks_drive,
                              NULL,         /* drive_name */
                              &drive->name,
                              &drive->icon,
                              NULL,         /* media_desc */
                              NULL);        /* media_icon */

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

  /* ---------------------------------------------------------------------------------------------------- */
  /* compute whether something changed */
  changed = !((old_is_media_removable == drive->is_media_removable) &&
              (old_has_media == drive->has_media) &&
              (old_can_eject == drive->can_eject) &&
              (g_strcmp0 (old_name, drive->name) == 0) &&
              (g_strcmp0 (old_device_file, drive->device_file) == 0) &&
              (old_dev == drive->dev) &&
              g_icon_equal (old_icon, drive->icon)
              );

  /* free old values */
  g_free (old_name);
  g_free (old_device_file);
  if (old_icon != NULL)
    g_object_unref (old_icon);

  /*g_debug ("in update_drive(); has_media=%d changed=%d", drive->has_media, changed);*/

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
                        UDisksDrive               *udisks_drive)
{
  GVfsUDisks2Drive *drive;

  drive = g_object_new (GVFS_TYPE_UDISKS2_DRIVE, NULL);
  drive->monitor = monitor;

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
  return FALSE;
}

static GDriveStartStopType
gvfs_udisks2_drive_get_start_stop_type (GDrive *_drive)
{
  return G_DRIVE_START_STOP_TYPE_UNKNOWN;
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

static void
gvfs_udisks2_drive_drive_iface_init (GDriveIface *iface)
{
  iface->get_name = gvfs_udisks2_drive_get_name;
  iface->get_icon = gvfs_udisks2_drive_get_icon;
  iface->has_volumes = gvfs_udisks2_drive_has_volumes;
  iface->get_volumes = gvfs_udisks2_drive_get_volumes;
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

#if 0
  iface->eject = gvfs_udisks2_drive_eject;
  iface->eject_finish = gvfs_udisks2_drive_eject_finish;
  iface->eject_with_operation = gvfs_udisks2_drive_eject_with_operation;
  iface->eject_with_operation_finish = gvfs_udisks2_drive_eject_with_operation_finish;
  iface->poll_for_media = gvfs_udisks2_drive_poll_for_media;
  iface->poll_for_media_finish = gvfs_udisks2_drive_poll_for_media_finish;
  iface->start = gvfs_udisks2_drive_start;
  iface->start_finish = gvfs_udisks2_drive_start_finish;
  iface->stop = gvfs_udisks2_drive_stop;
  iface->stop_finish = gvfs_udisks2_drive_stop_finish;
#endif
}

UDisksDrive *
gvfs_udisks2_drive_get_udisks_drive (GVfsUDisks2Drive *drive)
{
  return drive->udisks_drive;
}
