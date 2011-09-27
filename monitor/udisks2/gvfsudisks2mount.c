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
#include <stdio.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include <gvfsmountinfo.h>

#ifdef HAVE_GUDEV
#include <gudev/gudev.h>
#endif

#include "gvfsudisks2volumemonitor.h"
#include "gvfsudisks2mount.h"
#include "gvfsudisks2volume.h"

#define BUSY_UNMOUNT_NUM_ATTEMPTS              5
#define BUSY_UNMOUNT_MS_DELAY_BETWEEN_ATTEMPTS 100

typedef struct _GVfsUDisks2MountClass GVfsUDisks2MountClass;
struct _GVfsUDisks2MountClass
{
  GObjectClass parent_class;
};

struct _GVfsUDisks2Mount
{
  GObject parent;

  GVfsUDisks2VolumeMonitor *monitor; /* owned by volume monitor */
  GVfsUDisks2Volume        *volume;  /* owned by volume monitor */

  /* the following members are set in update_mount() */
  GFile *root;
  GIcon *icon;
  gchar *name;
  gchar *uuid;
  gchar *device_file;
  gchar *mount_path;
  gboolean can_unmount;
  gchar *mount_entry_name;
  GIcon *mount_entry_icon;

  gboolean is_burn_mount;

  GIcon *autorun_icon;
  gboolean searched_for_autorun;

  gchar *xdg_volume_info_name;
  GIcon *xdg_volume_info_icon;
  gboolean searched_for_xdg_volume_info;

  gchar *bdmv_volume_info_name;
  GIcon *bdmv_volume_info_icon;
  gboolean searched_for_bdmv_volume_info;
};

static gboolean update_mount (GVfsUDisks2Mount *mount);

static void gvfs_udisks2_mount_mount_iface_init (GMountIface *iface);

G_DEFINE_TYPE_EXTENDED (GVfsUDisks2Mount, gvfs_udisks2_mount, G_TYPE_OBJECT, 0,
                        G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT,
                                               gvfs_udisks2_mount_mount_iface_init))

static void on_volume_changed (GVolume *volume, gpointer user_data);

static void
gvfs_udisks2_mount_finalize (GObject *object)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (object);

  if (mount->volume != NULL)
    {
      g_signal_handlers_disconnect_by_func (mount->volume, on_volume_changed, mount);
      gvfs_udisks2_volume_unset_mount (mount->volume, mount);
    }

  if (mount->root != NULL)
    g_object_unref (mount->root);
  if (mount->icon != NULL)
    g_object_unref (mount->icon);
  g_free (mount->name);
  g_free (mount->uuid);
  g_free (mount->device_file);
  g_free (mount->mount_path);

  g_free (mount->mount_entry_name);
  if (mount->mount_entry_icon != NULL)
    g_object_unref (mount->mount_entry_icon);

  if (mount->autorun_icon != NULL)
    g_object_unref (mount->autorun_icon);

  g_free (mount->xdg_volume_info_name);
  if (mount->xdg_volume_info_icon != NULL)
    g_object_unref (mount->xdg_volume_info_icon);

  G_OBJECT_CLASS (gvfs_udisks2_mount_parent_class)->finalize (object);
}

static void
gvfs_udisks2_mount_class_init (GVfsUDisks2MountClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = gvfs_udisks2_mount_finalize;
}

static void
gvfs_udisks2_mount_init (GVfsUDisks2Mount *mount)
{
}

static void
emit_changed (GVfsUDisks2Mount *mount)
{
  g_signal_emit_by_name (mount, "changed");
  g_signal_emit_by_name (mount->monitor, "mount-changed", mount);
}

static void
got_autorun_info_cb (GObject      *source_object,
                     GAsyncResult *res,
                     gpointer      user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (user_data);
  mount->autorun_icon = g_vfs_mount_info_query_autorun_info_finish (G_FILE (source_object), res, NULL);
  if (update_mount (mount))
    emit_changed (mount);
  g_object_unref (mount);
}

static void
got_xdg_volume_info_cb (GObject      *source_object,
                        GAsyncResult *res,
                        gpointer      user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (user_data);
  mount->xdg_volume_info_icon = g_vfs_mount_info_query_xdg_volume_info_finish (G_FILE (source_object),
                                                                               res,
                                                                               &(mount->xdg_volume_info_name),
                                                                               NULL);
  if (update_mount (mount))
    emit_changed (mount);
  g_object_unref (mount);
}

static void
got_bdmv_volume_info_cb (GObject      *source_object,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (user_data);
  mount->bdmv_volume_info_icon = g_vfs_mount_info_query_bdmv_volume_info_finish (G_FILE (source_object),
                                                                                 res,
                                                                                 &(mount->bdmv_volume_info_name),
                                                                                 NULL);
  if (update_mount (mount))
    emit_changed (mount);
  g_object_unref (mount);
}

static gboolean
update_mount (GVfsUDisks2Mount *mount)
{
  gboolean changed;
  gboolean old_can_unmount;
  gchar *old_name;
  GIcon *old_icon;

  /* save old values */
  old_can_unmount = mount->can_unmount;
  old_name = g_strdup (mount->name);
  old_icon = mount->icon != NULL ? g_object_ref (mount->icon) : NULL;

  /* in with the new */
  if (mount->volume != NULL)
    {
      mount->can_unmount = TRUE;

      if (mount->icon != NULL)
        g_object_unref (mount->icon);

      /* icon order of preference: bdmv, xdg, autorun, probed */
      if (mount->bdmv_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->bdmv_volume_info_icon);
      else if (mount->xdg_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->xdg_volume_info_icon);
      else if (mount->autorun_icon != NULL)
        mount->icon = g_object_ref (mount->autorun_icon);
      else
        mount->icon = g_volume_get_icon (G_VOLUME (mount->volume));

      g_free (mount->name);

      /* name order of preference : bdmv, xdg, probed */
      if (mount->bdmv_volume_info_name != NULL)
        mount->name = g_strdup (mount->bdmv_volume_info_name);
      else if (mount->xdg_volume_info_name != NULL)
        mount->name = g_strdup (mount->xdg_volume_info_name);
      else
        mount->name = g_volume_get_name (G_VOLUME (mount->volume));
    }
  else
    {
      mount->can_unmount = TRUE;

      if (mount->icon != NULL)
        g_object_unref (mount->icon);

      /* icon order of preference: bdmv, xdg, autorun, probed */
      if (mount->bdmv_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->bdmv_volume_info_icon);
      else if (mount->xdg_volume_info_icon != NULL)
        mount->icon = g_object_ref (mount->xdg_volume_info_icon);
      else if (mount->autorun_icon != NULL)
        mount->icon = g_object_ref (mount->autorun_icon);
      else
        mount->icon = mount->mount_entry_icon != NULL ? g_object_ref (mount->mount_entry_icon) : NULL;

      g_free (mount->name);

      /* name order of preference: bdmv, xdg, probed */
      if (mount->bdmv_volume_info_name != NULL)
        mount->name = g_strdup (mount->bdmv_volume_info_name);
      else if (mount->xdg_volume_info_name != NULL)
        mount->name = g_strdup (mount->xdg_volume_info_name);
      else
        mount->name = g_strdup (mount->mount_entry_name);
    }

  /* compute whether something changed */
  changed = !((old_can_unmount == mount->can_unmount) &&
              (g_strcmp0 (old_name, mount->name) == 0) &&
              g_icon_equal (old_icon, mount->icon));

  /* free old values */
  g_free (old_name);
  if (old_icon != NULL)
    g_object_unref (old_icon);

  /*g_debug ("in update_mount(), changed=%d", changed);*/

  /* search for BDMV */
  if (!mount->searched_for_bdmv_volume_info)
    {
      mount->searched_for_bdmv_volume_info = TRUE;
      g_vfs_mount_info_query_bdmv_volume_info (mount->root,
      					       NULL,
      					       got_bdmv_volume_info_cb,
      					       g_object_ref (mount));
    }

  /* search for .xdg-volume-info */
  if (!mount->searched_for_xdg_volume_info)
    {
      mount->searched_for_xdg_volume_info = TRUE;
      g_vfs_mount_info_query_xdg_volume_info (mount->root,
                                              NULL,
                                              got_xdg_volume_info_cb,
                                              g_object_ref (mount));
    }

  /* search for autorun.inf */
  if (!mount->searched_for_autorun)
    {
      mount->searched_for_autorun = TRUE;
      g_vfs_mount_info_query_autorun_info (mount->root,
                                           NULL,
                                           got_autorun_info_cb,
                                           g_object_ref (mount));
    }

  return changed;
}

static void
on_volume_changed (GVolume  *volume,
                   gpointer  user_data)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (user_data);
  if (update_mount (mount))
    emit_changed (mount);
}

GVfsUDisks2Mount *
gvfs_udisks2_mount_new (GVfsUDisks2VolumeMonitor *monitor,
                        GUnixMountEntry          *mount_entry,
                        GVfsUDisks2Volume        *volume)
{
  GVfsUDisks2Mount *mount = NULL;

  /* Ignore internal mounts unless there's a volume */
  if (volume == NULL && (mount_entry != NULL && !g_unix_mount_guess_should_display (mount_entry)))
    goto out;

  mount = g_object_new (GVFS_TYPE_UDISKS2_MOUNT, NULL);
  mount->monitor = monitor;

  if (mount_entry != NULL)
    {
      /* No ref on GUnixMountEntry so save values for later use */
      mount->mount_entry_name = g_unix_mount_guess_name (mount_entry);
      mount->mount_entry_icon = g_unix_mount_guess_icon (mount_entry);
      mount->device_file = g_strdup (g_unix_mount_get_device_path (mount_entry));
      mount->mount_path = g_strdup (g_unix_mount_get_mount_path (mount_entry));
      mount->root = g_file_new_for_path (mount->mount_path);
    }
  else
    {
      /* burn:/// mount (the only mounts we support with mount_entry == NULL) */
      mount->device_file = NULL;
      mount->mount_path = NULL;
      mount->root = g_file_new_for_uri ("burn:///");
      mount->is_burn_mount = TRUE;
    }

  /* need to set the volume only when the mount is fully constructed */
  mount->volume = volume;
  if (mount->volume != NULL)
    {
      gvfs_udisks2_volume_set_mount (volume, mount);
      /* this is for piggy backing on the name and icon of the associated volume */
      g_signal_connect (mount->volume, "changed", G_CALLBACK (on_volume_changed), mount);
    }

  update_mount (mount);

 out:

  return mount;
}

void
gvfs_udisks2_mount_unmounted (GVfsUDisks2Mount *mount)
{
  if (mount->volume != NULL)
    {
      gvfs_udisks2_volume_unset_mount (mount->volume, mount);
      g_signal_handlers_disconnect_by_func (mount->volume, on_volume_changed, mount);
      mount->volume = NULL;
      emit_changed (mount);
    }
}

void
gvfs_udisks2_mount_unset_volume (GVfsUDisks2Mount   *mount,
                                 GVfsUDisks2Volume  *volume)
{
  if (mount->volume == volume)
    {
      g_signal_handlers_disconnect_by_func (mount->volume, on_volume_changed, mount);
      mount->volume = NULL;
      emit_changed (mount);
    }
}

static GFile *
gvfs_udisks2_mount_get_root (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->root != NULL ? g_object_ref (mount->root) : NULL;
}

static GIcon *
gvfs_udisks2_mount_get_icon (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->icon != NULL ? g_object_ref (mount->icon) : NULL;
}

static gchar *
gvfs_udisks2_mount_get_uuid (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return g_strdup (mount->uuid);
}

static gchar *
gvfs_udisks2_mount_get_name (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return g_strdup (mount->name);
}

gboolean
gvfs_udisks2_mount_has_uuid (GVfsUDisks2Mount *_mount,
                             const gchar      *uuid)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return g_strcmp0 (mount->uuid, uuid) == 0;
}

gboolean
gvfs_udisks2_mount_has_mount_path (GVfsUDisks2Mount *_mount,
                                   const gchar      *mount_path)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return g_strcmp0 (mount->mount_path, mount_path) == 0;
}

static GDrive *
gvfs_udisks2_mount_get_drive (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  GDrive *drive = NULL;

  if (mount->volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (mount->volume));
  return drive;
}

static GVolume *
gvfs_udisks2_mount_get_volume (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  GVolume *volume = NULL;

  if (mount->volume)
    volume = G_VOLUME (g_object_ref (mount->volume));
  return volume;
}

static gboolean
gvfs_udisks2_mount_can_unmount (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  return mount->can_unmount;
}

static gboolean
gvfs_udisks2_mount_can_eject (GMount *_mount)
{
  GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (_mount);
  GDrive *drive;
  gboolean can_eject;

  can_eject = FALSE;
  if (mount->volume != NULL)
    {
      drive = g_volume_get_drive (G_VOLUME (mount->volume));
      if (drive != NULL)
        can_eject = g_drive_can_eject (drive);
    }

  return can_eject;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
gvfs_udisks2_mount_mount_iface_init (GMountIface *iface)
{
  iface->get_root = gvfs_udisks2_mount_get_root;
  iface->get_name = gvfs_udisks2_mount_get_name;
  iface->get_icon = gvfs_udisks2_mount_get_icon;
  iface->get_uuid = gvfs_udisks2_mount_get_uuid;
  iface->get_drive = gvfs_udisks2_mount_get_drive;
  iface->get_volume = gvfs_udisks2_mount_get_volume;
  iface->can_unmount = gvfs_udisks2_mount_can_unmount;
  iface->can_eject = gvfs_udisks2_mount_can_eject;
#if 0
  iface->unmount = gvfs_udisks2_mount_unmount;
  iface->unmount_finish = gvfs_udisks2_mount_unmount_finish;
  iface->unmount_with_operation = gvfs_udisks2_mount_unmount_with_operation;
  iface->unmount_with_operation_finish = gvfs_udisks2_mount_unmount_with_operation_finish;
  iface->eject = gvfs_udisks2_mount_eject;
  iface->eject_finish = gvfs_udisks2_mount_eject_finish;
  iface->eject_with_operation = gvfs_udisks2_mount_eject_with_operation;
  iface->eject_with_operation_finish = gvfs_udisks2_mount_eject_with_operation_finish;
  iface->guess_content_type = gvfs_udisks2_mount_guess_content_type;
  iface->guess_content_type_finish = gvfs_udisks2_mount_guess_content_type_finish;
  iface->guess_content_type_sync = gvfs_udisks2_mount_guess_content_type_sync;
#endif
}

gboolean
gvfs_udisks2_mount_has_volume (GVfsUDisks2Mount   *mount,
                               GVfsUDisks2Volume  *volume)
{
  return mount->volume == volume;
}
