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

#include <limits.h>
#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "ggduvolumemonitor.h"
#include "ggdumount.h"
#include "ggduvolume.h"
#include "ggdudrive.h"

static GGduVolumeMonitor *the_volume_monitor = NULL;

struct _GGduVolumeMonitor {
  GNativeVolumeMonitor parent;

  GUnixMountMonitor *mount_monitor;

  GduPool *pool;

  GList *last_optical_disc_devices;
  GList *last_mountpoints;
  GList *last_mounts;

  GList *drives;
  GList *volumes;
  GList *fstab_volumes;
  GList *mounts;

  /* we keep volumes/mounts for blank and audio discs separate to handle e.g. mixed discs properly */
  GList *disc_volumes;
  GList *disc_mounts;

};

static void mountpoints_changed      (GUnixMountMonitor  *mount_monitor,
                                      gpointer            user_data);
static void mounts_changed           (GUnixMountMonitor  *mount_monitor,
                                      gpointer            user_data);

static void presentable_added (GduPool        *pool,
                               GduPresentable *presentable,
                               gpointer        user_data);
static void presentable_removed (GduPool        *pool,
                                 GduPresentable *presentable,
                                 gpointer        user_data);

static void update_all               (GGduVolumeMonitor *monitor,
                                      gboolean emit_changes);

static void update_drives            (GGduVolumeMonitor *monitor,
                                      GList **added_drives,
                                      GList **removed_drives);
static void update_volumes           (GGduVolumeMonitor *monitor,
                                      GList **added_volumes,
                                      GList **removed_volumes);
static void update_fstab_volumes     (GGduVolumeMonitor *monitor,
                                      GList **added_volumes,
                                      GList **removed_volumes);
static void update_mounts            (GGduVolumeMonitor *monitor,
                                      GList **added_mounts,
                                      GList **removed_mounts);
static void update_discs             (GGduVolumeMonitor *monitor,
                                      GList **added_volumes,
                                      GList **removed_volumes,
                                      GList **added_mounts,
                                      GList **removed_mounts);


G_DEFINE_TYPE (GGduVolumeMonitor, g_gdu_volume_monitor, G_TYPE_NATIVE_VOLUME_MONITOR)

static void
list_free (GList *objects)
{
  g_list_foreach (objects, (GFunc)g_object_unref, NULL);
  g_list_free (objects);
}

static void
g_gdu_volume_monitor_dispose (GObject *object)
{
  GGduVolumeMonitor *monitor;

  monitor = G_GDU_VOLUME_MONITOR (object);

  the_volume_monitor = NULL;

  if (G_OBJECT_CLASS (g_gdu_volume_monitor_parent_class)->dispose)
    (*G_OBJECT_CLASS (g_gdu_volume_monitor_parent_class)->dispose) (object);
}

static void
g_gdu_volume_monitor_finalize (GObject *object)
{
  GGduVolumeMonitor *monitor;

  monitor = G_GDU_VOLUME_MONITOR (object);

  g_signal_handlers_disconnect_by_func (monitor->mount_monitor, mountpoints_changed, monitor);
  g_signal_handlers_disconnect_by_func (monitor->mount_monitor, mounts_changed, monitor);
  g_signal_handlers_disconnect_by_func (monitor->mount_monitor, presentable_added, monitor);
  g_signal_handlers_disconnect_by_func (monitor->mount_monitor, presentable_removed, monitor);

  g_object_unref (monitor->mount_monitor);

  g_object_unref (monitor->pool);

  list_free (monitor->last_optical_disc_devices);
  list_free (monitor->last_mountpoints);
  g_list_foreach (monitor->last_mounts,
                  (GFunc)g_unix_mount_free, NULL);
  g_list_free (monitor->last_mounts);

  list_free (monitor->drives);
  list_free (monitor->fstab_volumes);
  list_free (monitor->volumes);
  list_free (monitor->mounts);

  list_free (monitor->disc_volumes);
  list_free (monitor->disc_mounts);

  if (G_OBJECT_CLASS (g_gdu_volume_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_gdu_volume_monitor_parent_class)->finalize) (object);
}

static GList *
get_mounts (GVolumeMonitor *volume_monitor)
{
  GGduVolumeMonitor *monitor;
  GList *l, *ll;

  monitor = G_GDU_VOLUME_MONITOR (volume_monitor);

  l = g_list_copy (monitor->mounts);
  ll = g_list_copy (monitor->disc_mounts);
  l = g_list_concat (l, ll);

  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  return l;
}

static GList *
get_volumes (GVolumeMonitor *volume_monitor)
{
  GGduVolumeMonitor *monitor;
  GList *l, *ll;

  monitor = G_GDU_VOLUME_MONITOR (volume_monitor);

  l = g_list_copy (monitor->volumes);
  ll = g_list_copy (monitor->fstab_volumes);
  l = g_list_concat (l, ll);
  ll = g_list_copy (monitor->disc_volumes);
  l = g_list_concat (l, ll);

  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  return l;
}

static GList *
get_connected_drives (GVolumeMonitor *volume_monitor)
{
  GGduVolumeMonitor *monitor;
  GList *l;

  monitor = G_GDU_VOLUME_MONITOR (volume_monitor);

  l = g_list_copy (monitor->drives);
  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  return l;
}

static GVolume *
get_volume_for_uuid (GVolumeMonitor *volume_monitor, const char *uuid)
{
  GGduVolumeMonitor *monitor;
  GGduVolume *volume;
  GList *l;

  monitor = G_GDU_VOLUME_MONITOR (volume_monitor);

  volume = NULL;

  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      volume = l->data;
      if (g_gdu_volume_has_uuid (volume, uuid))
        goto found;
    }

  for (l = monitor->fstab_volumes; l != NULL; l = l->next)
    {
      volume = l->data;
      if (g_gdu_volume_has_uuid (volume, uuid))
        goto found;
    }

  for (l = monitor->disc_volumes; l != NULL; l = l->next)
    {
      volume = l->data;
      if (g_gdu_volume_has_uuid (volume, uuid))
        goto found;
    }

  return NULL;

 found:

  g_object_ref (volume);

  return (GVolume *)volume;
}

static GMount *
get_mount_for_uuid (GVolumeMonitor *volume_monitor, const char *uuid)
{
  GGduVolumeMonitor *monitor;
  GGduMount *mount;
  GList *l;

  monitor = G_GDU_VOLUME_MONITOR (volume_monitor);

  mount = NULL;

  for (l = monitor->mounts; l != NULL; l = l->next)
    {
      mount = l->data;
      if (g_gdu_mount_has_uuid (mount, uuid))
        goto found;
    }

  for (l = monitor->disc_mounts; l != NULL; l = l->next)
    {
      mount = l->data;
      if (g_gdu_mount_has_uuid (mount, uuid))
        goto found;
    }

  return NULL;

 found:

  g_object_ref (mount);

  return (GMount *)mount;
}

static GMount *
get_mount_for_mount_path (const char *mount_path,
                          GCancellable *cancellable)
{
  GMount *mount;
  GGduMount *gdu_mount;
  GGduVolumeMonitor *volume_monitor;

  if (the_volume_monitor == NULL)
    {
      /* Dammit, no monitor is set up.. so we have to create one, find
       * what the user asks for and throw it away again.
       *
       * What a waste - especially considering that there's IO
       * involved in doing this: connect to the system message bus;
       * IPC to DeviceKit-disks etc etc
       */
      volume_monitor = G_GDU_VOLUME_MONITOR (g_gdu_volume_monitor_new ());
    }
  else
    {
      volume_monitor = g_object_ref (the_volume_monitor);
    }

  mount = NULL;

  /* creation of the volume monitor might actually fail */
  if (volume_monitor != NULL)
    {
      GList *l;

      for (l = volume_monitor->mounts; l != NULL; l = l->next)
        {
          gdu_mount = l->data;

          if (g_gdu_mount_has_mount_path (gdu_mount, mount_path))
            {
              mount = g_object_ref (gdu_mount);
              break;
            }
        }
    }

  g_object_unref (volume_monitor);

  return (GMount *) mount;
}

static void
mountpoints_changed (GUnixMountMonitor *mount_monitor,
                     gpointer           user_data)
{
  GGduVolumeMonitor *monitor = G_GDU_VOLUME_MONITOR (user_data);

  update_all (monitor, TRUE);
}

static void
mounts_changed (GUnixMountMonitor *mount_monitor,
                gpointer           user_data)
{
  GGduVolumeMonitor *monitor = G_GDU_VOLUME_MONITOR (user_data);

  update_all (monitor, TRUE);
}

static void
presentable_added (GduPool        *pool,
                   GduPresentable *presentable,
                   gpointer        user_data)
{
  GGduVolumeMonitor *monitor = G_GDU_VOLUME_MONITOR (user_data);

  /*g_debug ("presentable_added %p: %s", presentable, gdu_presentable_get_id (presentable));*/

  update_all (monitor, TRUE);
}

static void
presentable_removed (GduPool        *pool,
                     GduPresentable *presentable,
                     gpointer        user_data)
{
  GGduVolumeMonitor *monitor = G_GDU_VOLUME_MONITOR (user_data);

  /*g_debug ("presentable_removed %p: %s", presentable, gdu_presentable_get_id (presentable));*/

  update_all (monitor, TRUE);
}

static void
presentable_changed (GduPool        *pool,
                     GduPresentable *presentable,
                     gpointer        user_data)
{
  GGduVolumeMonitor *monitor = G_GDU_VOLUME_MONITOR (user_data);

  /*g_debug ("presentable_changed %p: %s", presentable, gdu_presentable_get_id (presentable));*/

  update_all (monitor, TRUE);
}

static void
presentable_job_changed (GduPool        *pool,
                         GduPresentable *presentable,
                         gpointer        user_data)
{
  GGduVolumeMonitor *monitor = G_GDU_VOLUME_MONITOR (user_data);

  /*g_debug ("presentable_job_changed %p: %s", presentable, gdu_presentable_get_id (presentable));*/

  update_all (monitor, TRUE);
}

static GObject *
g_gdu_volume_monitor_constructor (GType                  type,
                                  guint                  n_construct_properties,
                                  GObjectConstructParam *construct_properties)
{
  GObject *object;
  GGduVolumeMonitor *monitor;
  GGduVolumeMonitorClass *klass;
  GObjectClass *parent_class;

  if (the_volume_monitor != NULL)
    {
      object = g_object_ref (the_volume_monitor);
      return object;
    }

  /*g_warning ("creating gdu vm");*/

  object = NULL;

  /* Invoke parent constructor. */
  klass = G_GDU_VOLUME_MONITOR_CLASS (g_type_class_peek (G_TYPE_GDU_VOLUME_MONITOR));
  parent_class = G_OBJECT_CLASS (g_type_class_peek_parent (klass));
  object = parent_class->constructor (type,
                                      n_construct_properties,
                                      construct_properties);

  monitor = G_GDU_VOLUME_MONITOR (object);

  monitor->mount_monitor = g_unix_mount_monitor_new ();

  g_signal_connect (monitor->mount_monitor,
                    "mounts_changed",
                    G_CALLBACK (mounts_changed),
                    monitor);

  g_signal_connect (monitor->mount_monitor,
                    "mountpoints_changed",
                    G_CALLBACK (mountpoints_changed),
                    monitor);

  monitor->pool = gdu_pool_new ();

  g_signal_connect (monitor->pool,
                    "presentable_added",
                    G_CALLBACK (presentable_added),
                    monitor);

  g_signal_connect (monitor->pool,
                    "presentable_removed",
                    G_CALLBACK (presentable_removed),
                    monitor);

  g_signal_connect (monitor->pool,
                    "presentable_changed",
                    G_CALLBACK (presentable_changed),
                    monitor);

  g_signal_connect (monitor->pool,
                    "presentable_job_changed",
                    G_CALLBACK (presentable_job_changed),
                    monitor);

  update_all (monitor, FALSE);

  the_volume_monitor = monitor;

  return object;
}

static void
g_gdu_volume_monitor_init (GGduVolumeMonitor *monitor)
{
}

static gboolean
is_supported (void)
{
  /* TODO: return FALSE if DeviceKit-disks is not available */
  return TRUE;
}

static void
g_gdu_volume_monitor_class_init (GGduVolumeMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVolumeMonitorClass *monitor_class = G_VOLUME_MONITOR_CLASS (klass);
  GNativeVolumeMonitorClass *native_class = G_NATIVE_VOLUME_MONITOR_CLASS (klass);

  gobject_class->constructor = g_gdu_volume_monitor_constructor;
  gobject_class->finalize = g_gdu_volume_monitor_finalize;
  gobject_class->dispose = g_gdu_volume_monitor_dispose;

  monitor_class->get_mounts = get_mounts;
  monitor_class->get_volumes = get_volumes;
  monitor_class->get_connected_drives = get_connected_drives;
  monitor_class->get_volume_for_uuid = get_volume_for_uuid;
  monitor_class->get_mount_for_uuid = get_mount_for_uuid;
  monitor_class->is_supported = is_supported;

  native_class->get_mount_for_mount_path = get_mount_for_mount_path;
}

/**
 * g_gdu_volume_monitor_new:
 *
 * Returns:  a new #GVolumeMonitor.
 **/
GVolumeMonitor *
g_gdu_volume_monitor_new (void)
{
  GGduVolumeMonitor *monitor;

  monitor = g_object_new (G_TYPE_GDU_VOLUME_MONITOR, NULL);

  return G_VOLUME_MONITOR (monitor);
}

static void
diff_sorted_lists (GList         *list1,
                   GList         *list2,
                   GCompareFunc   compare,
                   GList        **added,
                   GList        **removed)
{
  int order;

  *added = *removed = NULL;

  while (list1 != NULL &&
         list2 != NULL)
    {
      order = (*compare) (list1->data, list2->data);
      if (order < 0)
        {
          *removed = g_list_prepend (*removed, list1->data);
          list1 = list1->next;
        }
      else if (order > 0)
        {
          *added = g_list_prepend (*added, list2->data);
          list2 = list2->next;
        }
      else
        { /* same item */
          list1 = list1->next;
          list2 = list2->next;
        }
    }

  while (list1 != NULL)
    {
      *removed = g_list_prepend (*removed, list1->data);
      list1 = list1->next;
    }
  while (list2 != NULL)
    {
      *added = g_list_prepend (*added, list2->data);
      list2 = list2->next;
    }
}

static GGduVolume *
find_volume_for_mount_path (GGduVolumeMonitor *monitor,
                            const char         *mount_path)
{
  GList *l;
  GGduVolume *found;

  found = NULL;

  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      GGduVolume *volume = l->data;
      if (g_gdu_volume_has_mount_path (volume, mount_path))
        {
          found = volume;
          goto out;
        }
    }

  for (l = monitor->fstab_volumes; l != NULL; l = l->next)
    {
      GGduVolume *volume = l->data;
      if (g_gdu_volume_has_mount_path (volume, mount_path))
        {
          found = volume;
          goto out;
        }
    }

 out:
  return found;
}

static GGduVolume *
find_volume_for_unix_mount_point (GGduVolumeMonitor *monitor,
                                  GUnixMountPoint   *unix_mount_point)
{
  GList *l;
  GGduVolume *found;

  found = NULL;
  for (l = monitor->fstab_volumes; l != NULL; l = l->next)
    {
      GGduVolume *volume = l->data;
      GUnixMountPoint *volume_mount_point;

      volume_mount_point = g_gdu_volume_get_unix_mount_point (volume);
      if (g_unix_mount_point_compare (unix_mount_point, volume_mount_point) == 0)
        {
          found = volume;
          goto out;
        }
    }

 out:
  return found;
}

static GGduMount *
find_mount_by_mount_path (GGduVolumeMonitor *monitor,
                          const char *mount_path)
{
  GList *l;

  for (l = monitor->mounts; l != NULL; l = l->next)
    {
      GGduMount *mount = l->data;

      if (g_gdu_mount_has_mount_path (mount, mount_path))
        return mount;
    }

  return NULL;
}

/* TODO: move to gio */
static gboolean
_g_unix_mount_point_guess_should_display (GUnixMountPoint *mount_point)
{
  const char *mount_path;

  mount_path = g_unix_mount_point_get_mount_path (mount_point);

  /* Never display internal mountpoints */
  if (g_unix_is_mount_path_system_internal (mount_path))
    return FALSE;

  /* Only display things in /media (which are generally user mountable)
     and home dir (fuse stuff) */
  if (g_str_has_prefix (mount_path, "/media/"))
    return TRUE;

  if (g_str_has_prefix (mount_path, g_get_home_dir ()))
    return TRUE;

  return FALSE;
}

static GUnixMountPoint *
get_mount_point_for_device (GduDevice *d, GList *fstab_mount_points)
{
  GList *l;
  const gchar *device_file;
  const gchar *mount_path;
  GUnixMountPoint *ret;

  ret = NULL;

  mount_path = gdu_device_get_mount_path (d);

  device_file = gdu_device_get_device_file (d);

  for (l = fstab_mount_points; l != NULL; l = l->next)
    {
      GUnixMountPoint *mount_point = l->data;
      const gchar *fstab_device_file;
      const gchar *fstab_mount_path;

      fstab_mount_path = g_unix_mount_point_get_mount_path (mount_point);
      if (g_strcmp0 (mount_path, fstab_mount_path) == 0)
        {
          ret = mount_point;
          goto out;
        }

      fstab_device_file = g_unix_mount_point_get_device_path (mount_point);
      if (g_str_has_prefix (fstab_device_file, "LABEL="))
        {
          if (g_strcmp0 (fstab_device_file + 6, gdu_device_id_get_label (d)) == 0)
            {
              ret = mount_point;
              goto out;
            }
        }
      else if (g_str_has_prefix (fstab_device_file, "UUID="))
        {
          if (g_ascii_strcasecmp (fstab_device_file + 5, gdu_device_id_get_uuid (d)) == 0)
            {
              ret = mount_point;
              goto out;
            }
        }
      else
        {
          char resolved_fstab_device_file[PATH_MAX];

          /* handle symlinks such as /dev/disk/by-uuid/47C2-1994 */
          if (realpath (fstab_device_file, resolved_fstab_device_file) != NULL &&
              g_strcmp0 (resolved_fstab_device_file, device_file) == 0)
            {
              ret = mount_point;
              goto out;
            }
        }
    }

 out:
  return ret;
}

static gboolean
should_mount_be_ignored (GduPool *pool, GduDevice *d)
{
  gboolean ret;
  const gchar *mount_path;
  GUnixMountEntry *mount_entry;

  ret = FALSE;

  if (gdu_device_get_presentation_hide (d))
    {
      ret = TRUE;
      goto out;
    }

  mount_path = gdu_device_get_mount_path (d);
  if (mount_path == NULL || strlen (mount_path) == 0)
    goto out;

  mount_entry = g_unix_mount_at (mount_path, NULL);
  if (mount_entry != NULL)
    {
      if (!g_unix_mount_guess_should_display (mount_entry))
        {
          ret = TRUE;
        }
      g_unix_mount_free (mount_entry);
    }

 out:
  return ret;
}

gboolean
_is_pc_floppy_drive (GduDevice *device)
{
  gboolean ret;
  gchar **drive_media_compat;
  const gchar *drive_connection_interface;

  ret = FALSE;

  if (device != NULL)
    {
      drive_media_compat = gdu_device_drive_get_media_compatibility (device);
      drive_connection_interface = gdu_device_drive_get_connection_interface (device);

      if (g_strcmp0 (drive_connection_interface, "platform") == 0 &&
          (drive_media_compat != NULL &&
           g_strv_length (drive_media_compat) > 0 &&
           g_strcmp0 (drive_media_compat[0], "floppy") == 0))
        {
          ret = TRUE;
        }
    }

  return ret;
}

static gboolean
should_volume_be_ignored (GduPool *pool, GduVolume *volume, GList *fstab_mount_points)
{
  GduDevice *device;
  gboolean ret;
  const gchar *usage;
  const gchar *type;

  ret = TRUE;

  device = gdu_presentable_get_device (GDU_PRESENTABLE (volume));
  if (device == NULL)
    goto out;

  if (gdu_device_get_presentation_hide (device))
    goto out;

  usage = gdu_device_id_get_usage (device);
  type = gdu_device_id_get_type (device);

  if (_is_pc_floppy_drive (device) || g_strcmp0 (usage, "filesystem") == 0)
    {
      GUnixMountPoint *mount_point;

      /* don't ignore volumes with a mountable filesystem unless
       *
       *  - volume is referenced in /etc/fstab and deemed to be ignored
       *
       *  - volume is mounted and should_mount_be_ignored() deems it should be ignored
       *
       *  - volume is a cleartext LUKS device as the cryptotext LUKS volume will morph
       *    into the cleartext volume when unlocked (see ggduvolume.c)
       */

      if (gdu_device_is_luks_cleartext (device))
        goto out;

      mount_point = get_mount_point_for_device (device, fstab_mount_points);
      if (mount_point != NULL && !_g_unix_mount_point_guess_should_display (mount_point))
        goto out;

      if (gdu_device_is_mounted (device))
        {
          ret = should_mount_be_ignored (pool, device);
          goto out;
        }

      ret = FALSE;

    }
  else if (g_strcmp0 (usage, "crypto") == 0 && g_strcmp0 (type, "crypto_LUKS") == 0)
    {
      /* don't ignore LUKS volumes */
      ret = FALSE;
    }

 out:

  if (device != NULL)
    g_object_unref (device);
  return ret;
}

static gboolean
should_drive_be_ignored (GduPool *pool, GduDrive *d, GList *fstab_mount_points)
{
  GduDevice *device;
  gboolean ignored;
  gboolean have_volumes;
  gboolean all_volumes_are_ignored;
  GList *enclosed;
  GList *l;

  ignored = FALSE;
  enclosed = NULL;

  device = gdu_presentable_get_device (GDU_PRESENTABLE (d));

  /* If there is no GduDevice for a drive, then ignore it unless
   * we know how to start it (e.g. RAID arrays, Volume Groups)
   */
  if (device == NULL)
    {
      if (!gdu_drive_is_activatable (d))
        {
          ignored = TRUE;
          goto out;
        }
    }

  if (device != NULL && gdu_device_get_presentation_hide (device)) {
    ignored = TRUE;
    goto out;
  }

  have_volumes = FALSE;
  all_volumes_are_ignored = TRUE;

  /* never ignore a drive if it has volumes that we don't want to ignore */
  enclosed = gdu_pool_get_enclosed_presentables (pool, GDU_PRESENTABLE (d));
  for (l = enclosed; l != NULL && all_volumes_are_ignored; l = l->next)
    {
      GduPresentable *enclosed_presentable = GDU_PRESENTABLE (l->data);

      /* There might be other presentables that GduVolume objects; for example GduVolumeHole */
      if (GDU_IS_VOLUME (enclosed_presentable))
        {
          GduVolume *volume = GDU_VOLUME (enclosed_presentable);
          GduDevice *volume_device;

          have_volumes = TRUE;

          if (!should_volume_be_ignored (pool, volume, fstab_mount_points))
            {
              all_volumes_are_ignored = FALSE;
              break;
            }

          /* The volume may be an extended partition - we need to check all logical
           * partitions as well (#597041)
           */
          volume_device = gdu_presentable_get_device (GDU_PRESENTABLE (volume));
          if (volume_device != NULL)
            {
              if (g_strcmp0 (gdu_device_partition_get_scheme (volume_device), "mbr") == 0)
                {
                  gint type;

                  type = strtol (gdu_device_partition_get_type (volume_device), NULL, 0);
                  if (type == 0x05 || type == 0x0f || type == 0x85)
                    {
                      GList *enclosed_logical;
                      GList *ll;

                      enclosed_logical = gdu_pool_get_enclosed_presentables (pool, GDU_PRESENTABLE (volume));
                      for (ll = enclosed_logical; ll != NULL && all_volumes_are_ignored; ll = ll->next)
                        {
                          GduPresentable *enclosed_logical_presentable = GDU_PRESENTABLE (ll->data);

                          if (GDU_IS_VOLUME (enclosed_logical_presentable))
                            {
                              if (!should_volume_be_ignored (pool,
                                                             GDU_VOLUME (enclosed_logical_presentable),
                                                             fstab_mount_points))
                                {
                                  all_volumes_are_ignored = FALSE;
                                }
                            }
                        }
                      g_list_foreach (enclosed_logical, (GFunc) g_object_unref, NULL);
                      g_list_free (enclosed_logical);
                    }
                }
              g_object_unref (volume_device);
            }
        }
    }

  /* we ignore a drive if
   *
   * a) no volumes are available AND media is available; OR
   *
   * b) the volumes of the drive are all ignored
   */
  if (device != NULL)
    {
      if (!have_volumes)
        {
          if (gdu_device_is_media_available (device))
            ignored = TRUE;
        }
      else
        {
          if (all_volumes_are_ignored)
            ignored = TRUE;
        }

      /* special case for audio and blank discs: don't ignore the drive since we'll create
       * a cdda:// or burn:// mount for the drive
       */
      if (gdu_device_is_optical_disc (device) && (gdu_device_optical_disc_get_num_audio_tracks (device) > 0 ||
                                                  gdu_device_optical_disc_get_is_blank (device)))
        {
          ignored = FALSE;
        }
    }

 out:
  g_list_foreach (enclosed, (GFunc) g_object_unref, NULL);
  g_list_free (enclosed);

  if (device != NULL)
    g_object_unref (device);

  return ignored;
}

static void
list_emit (GGduVolumeMonitor *monitor,
           const char *monitor_signal,
           const char *object_signal,
           GList *objects)
{
  GList *l;

  for (l = objects; l != NULL; l = l->next)
    {
      g_signal_emit_by_name (monitor, monitor_signal, l->data);
      if (object_signal)
        g_signal_emit_by_name (l->data, object_signal);
    }
}

static void
update_all (GGduVolumeMonitor *monitor,
            gboolean emit_changes)
{
  GList *added_drives, *removed_drives;
  GList *added_volumes, *removed_volumes;
  GList *added_mounts, *removed_mounts;

  added_drives = NULL;
  removed_drives = NULL;
  added_volumes = NULL;
  removed_volumes = NULL;
  added_mounts = NULL;
  removed_mounts = NULL;

  update_drives (monitor, &added_drives, &removed_drives);
  update_volumes (monitor, &added_volumes, &removed_volumes);
  update_fstab_volumes (monitor, &added_volumes, &removed_volumes);
  update_mounts (monitor, &added_mounts, &removed_mounts);
  update_discs (monitor,
                &added_volumes, &removed_volumes,
                &added_mounts, &removed_mounts);

  if (emit_changes)
    {
      list_emit (monitor,
                 "drive_disconnected", NULL,
                 removed_drives);
      list_emit (monitor,
                 "drive_connected", NULL,
                 added_drives);

      list_emit (monitor,
                 "volume_removed", "removed",
                 removed_volumes);
      list_emit (monitor,
                 "volume_added", NULL,
                 added_volumes);

      list_emit (monitor,
                 "mount_removed", "unmounted",
                 removed_mounts);
      list_emit (monitor,
                 "mount_added", NULL,
                 added_mounts);
    }

  list_free (removed_drives);
  list_free (added_drives);
  list_free (removed_volumes);
  list_free (added_volumes);
  list_free (removed_mounts);
  list_free (added_mounts);
}

static GGduMount *
find_disc_mount_for_volume (GGduVolumeMonitor *monitor,
                            GGduVolume        *volume)
{
  GList *l;

  for (l = monitor->disc_mounts; l != NULL; l = l->next)
    {
      GGduMount *mount = G_GDU_MOUNT (l->data);

      if (g_gdu_mount_has_volume (mount, volume))
        return mount;
    }

  return NULL;
}

static GGduVolume *
find_disc_volume_for_device_file (GGduVolumeMonitor *monitor,
                                  const gchar       *device_file)
{
  GList *l;
  GGduVolume *ret;
  struct stat stat_buf;

  ret = NULL;

  if (stat (device_file, &stat_buf) == 0)
    {
      for (l = monitor->disc_volumes; l != NULL; l = l->next)
        {
          GGduVolume *volume = G_GDU_VOLUME (l->data);
          if (g_gdu_volume_has_dev (volume, stat_buf.st_rdev))
            {
              ret = volume;
              goto out;
            }
        }
    }

  for (l = monitor->disc_volumes; l != NULL; l = l->next)
    {
      GGduVolume *volume = G_GDU_VOLUME (l->data);

      if (g_gdu_volume_has_device_file (volume, device_file))
        {
          ret = volume;
          goto out;
        }
    }

 out:
  return ret;
}

static GGduVolume *
find_volume_for_device_file (GGduVolumeMonitor *monitor,
                             const gchar       *device_file)
{
  GList *l;
  GGduVolume *ret;
  struct stat stat_buf;

  ret = NULL;

  if (stat (device_file, &stat_buf) == 0)
    {
      for (l = monitor->volumes; l != NULL; l = l->next)
        {
          GGduVolume *volume = G_GDU_VOLUME (l->data);
          if (g_gdu_volume_has_dev (volume, stat_buf.st_rdev))
            {
              ret = volume;
              goto out;
            }
        }
    }

  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      GGduVolume *volume = G_GDU_VOLUME (l->data);

      if (g_gdu_volume_has_device_file (volume, device_file))
        {
          ret = volume;
          goto out;
        }
    }

  ret = NULL;
  for (l = monitor->fstab_volumes; l != NULL; l = l->next)
    {
      GGduVolume *volume = G_GDU_VOLUME (l->data);

      if (g_gdu_volume_has_device_file (volume, device_file))
        {
          ret = volume;
          goto out;
        }
    }

 out:
  return ret;
}

static GGduVolume *
find_volume_for_presentable (GGduVolumeMonitor *monitor,
                             GduPresentable    *presentable)
{
  GList *l;

  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      GGduVolume *volume = G_GDU_VOLUME (l->data);

      if (g_gdu_volume_has_presentable (volume, presentable))
        return volume;
    }

  return NULL;
}

static GGduDrive *
find_drive_by_device_file (GGduVolumeMonitor *monitor,
                           const gchar       *device_file)
{
  GList *l;
  struct stat stat_buf;

  if (stat (device_file, &stat_buf) != 0)
    {
      g_warning ("%s:%s: Error statting %s: %m", G_STRLOC, G_STRFUNC, device_file);
      return NULL;
    }

  for (l = monitor->drives; l != NULL; l = l->next)
    {
      GGduDrive *drive = G_GDU_DRIVE (l->data);

      if (g_gdu_drive_has_dev (drive, stat_buf.st_rdev))
        return drive;
    }

  return NULL;
}

static GGduDrive *
find_drive_by_presentable (GGduVolumeMonitor *monitor,
                           GduPresentable    *presentable)
{
  GList *l;

  for (l = monitor->drives; l != NULL; l = l->next)
    {
      GGduDrive *drive = G_GDU_DRIVE (l->data);

      if (g_gdu_drive_has_presentable (drive, presentable))
        return drive;
    }

  return NULL;
}

static void
update_drives (GGduVolumeMonitor *monitor,
               GList **added_drives,
               GList **removed_drives)
{
  GList *cur_drives;
  GList *new_drives;
  GList *removed, *added;
  GList *l, *ll;
  GGduDrive *drive;
  GList *fstab_mount_points;

  fstab_mount_points = g_unix_mount_points_get (NULL);

  cur_drives = NULL;
  for (l = monitor->drives; l != NULL; l = l->next)
    cur_drives = g_list_prepend (cur_drives, g_gdu_drive_get_presentable (G_GDU_DRIVE (l->data)));

  /* remove devices we want to ignore - we do it here so we get to reevaluate
   * on the next update whether they should still be ignored
   */
  new_drives = gdu_pool_get_presentables (monitor->pool);
  for (l = new_drives; l != NULL; l = ll)
    {
      GduPresentable *p = GDU_PRESENTABLE (l->data);
      ll = l->next;
      if (!GDU_IS_DRIVE (p) || should_drive_be_ignored (monitor->pool, GDU_DRIVE (p), fstab_mount_points))
        {
          g_object_unref (p);
          new_drives = g_list_delete_link (new_drives, l);
        }
    }

  cur_drives = g_list_sort (cur_drives, (GCompareFunc) gdu_presentable_compare);
  new_drives = g_list_sort (new_drives, (GCompareFunc) gdu_presentable_compare);
  diff_sorted_lists (cur_drives,
                     new_drives, (GCompareFunc) gdu_presentable_compare,
                     &added, &removed);

  for (l = removed; l != NULL; l = l->next)
    {
      GduPresentable *p = GDU_PRESENTABLE (l->data);

      drive = find_drive_by_presentable (monitor, p);
      if (drive != NULL)
        {
          /*g_debug ("removing drive %s", gdu_presentable_get_id (p));*/
          g_gdu_drive_disconnected (drive);
          monitor->drives = g_list_remove (monitor->drives, drive);
          *removed_drives = g_list_prepend (*removed_drives, drive);
        }
    }

  for (l = added; l != NULL; l = l->next)
    {
      GduPresentable *p = GDU_PRESENTABLE (l->data);

      drive = find_drive_by_presentable (monitor, p);
      if (drive == NULL)
        {
          /*g_debug ("adding drive %s", gdu_presentable_get_id (p));*/
          drive = g_gdu_drive_new (G_VOLUME_MONITOR (monitor), p);
          if (drive != NULL)
            {
              monitor->drives = g_list_prepend (monitor->drives, drive);
              *added_drives = g_list_prepend (*added_drives, g_object_ref (drive));
            }
        }
    }

  g_list_free (added);
  g_list_free (removed);

  g_list_free (cur_drives);

  g_list_foreach (new_drives, (GFunc) g_object_unref, NULL);
  g_list_free (new_drives);

  g_list_foreach (fstab_mount_points, (GFunc) g_unix_mount_point_free, NULL);
  g_list_free (fstab_mount_points);
}

static void
update_volumes (GGduVolumeMonitor *monitor,
                GList **added_volumes,
                GList **removed_volumes)
{
  GList *cur_volumes;
  GList *new_volumes;
  GList *removed, *added;
  GList *l, *ll;
  GGduVolume *volume;
  GGduDrive *drive;
  GList *fstab_mount_points;

  fstab_mount_points = g_unix_mount_points_get (NULL);

  cur_volumes = NULL;
  for (l = monitor->volumes; l != NULL; l = l->next)
    cur_volumes = g_list_prepend (cur_volumes, g_gdu_volume_get_presentable (G_GDU_VOLUME (l->data)));

  /* remove devices we want to ignore - we do it here so we get to reevaluate
   * on the next update whether they should still be ignored
   */
  new_volumes = gdu_pool_get_presentables (monitor->pool);
  for (l = new_volumes; l != NULL; l = ll)
    {
      GduPresentable *p = GDU_PRESENTABLE (l->data);
      ll = l->next;
      if (!GDU_IS_VOLUME (p) || should_volume_be_ignored (monitor->pool, GDU_VOLUME (p), fstab_mount_points))
        {
          g_object_unref (p);
          new_volumes = g_list_delete_link (new_volumes, l);
        }
    }

  cur_volumes = g_list_sort (cur_volumes, (GCompareFunc) gdu_presentable_compare);
  new_volumes = g_list_sort (new_volumes, (GCompareFunc) gdu_presentable_compare);
  diff_sorted_lists (cur_volumes,
                     new_volumes, (GCompareFunc) gdu_presentable_compare,
                     &added, &removed);

  for (l = removed; l != NULL; l = l->next)
    {
      GduPresentable *p = GDU_PRESENTABLE (l->data);

      volume = find_volume_for_presentable (monitor, p);
      if (volume != NULL)
        {
          /*g_debug ("removing volume %s", gdu_device_get_device_file (d));*/
          g_gdu_volume_removed (volume);
          monitor->volumes = g_list_remove (monitor->volumes, volume);
          *removed_volumes = g_list_prepend (*removed_volumes, volume);
        }
    }

  for (l = added; l != NULL; l = l->next)
    {
      GduPresentable *p = GDU_PRESENTABLE (l->data);
      GduDevice *d;

      volume = NULL;
      d = gdu_presentable_get_device (p);

      if (d != NULL)
        volume = find_volume_for_device_file (monitor, gdu_device_get_device_file (d));

      if (volume == NULL)
        {
          GduDrive *gdu_drive;

          drive = NULL;
          gdu_drive = gdu_volume_get_drive (GDU_VOLUME (p));
          if (gdu_drive != NULL)
            {
              drive = find_drive_by_presentable (monitor, GDU_PRESENTABLE (gdu_drive));
              g_object_unref (gdu_drive);
            }

          volume = g_gdu_volume_new (G_VOLUME_MONITOR (monitor),
                                     GDU_VOLUME (p),
                                     drive,
                                     NULL);
          if (volume != NULL)
            {
              monitor->volumes = g_list_prepend (monitor->volumes, volume);
              *added_volumes = g_list_prepend (*added_volumes, g_object_ref (volume));
            }
         }

       if (d != NULL)
         g_object_unref (d);
    }

  g_list_free (added);
  g_list_free (removed);

  g_list_foreach (new_volumes, (GFunc) g_object_unref, NULL);
  g_list_free (new_volumes);

  g_list_free (cur_volumes);

  g_list_foreach (fstab_mount_points, (GFunc) g_unix_mount_point_free, NULL);
  g_list_free (fstab_mount_points);
}

static void
update_fstab_volumes (GGduVolumeMonitor *monitor,
                      GList **added_volumes,
                      GList **removed_volumes)
{
  GList *fstab_mount_points;
  GList *cur_fstab_mount_points;
  GList *new_fstab_mount_points;
  GList *removed, *added;
  GList *l;
  GGduVolume *volume;

  fstab_mount_points = g_unix_mount_points_get (NULL);

  cur_fstab_mount_points = NULL;
  for (l = monitor->fstab_volumes; l != NULL; l = l->next)
    cur_fstab_mount_points = g_list_prepend (cur_fstab_mount_points, g_gdu_volume_get_unix_mount_point (G_GDU_VOLUME (l->data)));

  new_fstab_mount_points = NULL;
  for (l = fstab_mount_points; l != NULL; l = l->next)
    {
      GUnixMountPoint *mount_point = l->data;
      const gchar *device_file;

      /* only show user mountable mount points */
      if (!g_unix_mount_point_is_user_mountable (mount_point))
        continue;

      /* only show stuff that can be mounted in user-visible locations */
      if (!_g_unix_mount_point_guess_should_display (mount_point))
        continue;

      /* ignore mount point if the device doesn't exist or is handled by DeviceKit-disks */
      device_file = g_unix_mount_point_get_device_path (mount_point);
      if (g_str_has_prefix (device_file, "/dev/"))
        {
          gchar resolved_path[PATH_MAX];
          GduDevice *device;

          /* doesn't exist */
          if (realpath (device_file, resolved_path) != 0)
            continue;

          /* is handled by DKD */
          device = gdu_pool_get_by_device_file (monitor->pool, resolved_path);
          if (device != NULL)
            {
              g_object_unref (device);
              continue;
            }
        }

      new_fstab_mount_points = g_list_prepend (new_fstab_mount_points, mount_point);
    }

  diff_sorted_lists (cur_fstab_mount_points,
                     new_fstab_mount_points, (GCompareFunc) g_unix_mount_point_compare,
                     &added, &removed);

  for (l = removed; l != NULL; l = l->next)
    {
      GUnixMountPoint *mount_point = l->data;
      volume = find_volume_for_unix_mount_point (monitor, mount_point);
      if (volume != NULL)
        {
          g_gdu_volume_removed (volume);
          monitor->fstab_volumes = g_list_remove (monitor->fstab_volumes, volume);
          *removed_volumes = g_list_prepend (*removed_volumes, volume);
          /*g_debug ("removed volume for /etc/fstab mount point %s", g_unix_mount_point_get_mount_path (mount_point));*/
        }
    }

  for (l = added; l != NULL; l = l->next)
    {
      GUnixMountPoint *mount_point = l->data;

      volume = g_gdu_volume_new_for_unix_mount_point (G_VOLUME_MONITOR (monitor), mount_point);
      if (volume != NULL)
        {
          /* steal mount_point since g_gdu_volume_new_for_unix_mount_point() takes ownership of it */
          fstab_mount_points = g_list_remove (fstab_mount_points, mount_point);
          monitor->fstab_volumes = g_list_prepend (monitor->fstab_volumes, volume);
          *added_volumes = g_list_prepend (*added_volumes, g_object_ref (volume));
          /*g_debug ("added volume for /etc/fstab mount point %s", g_unix_mount_point_get_mount_path (mount_point));*/
        }
      else
        {
          g_unix_mount_point_free (mount_point);
        }
    }

  g_list_free (added);
  g_list_free (removed);

  g_list_free (cur_fstab_mount_points);

  g_list_foreach (fstab_mount_points, (GFunc) g_unix_mount_point_free, NULL);
  g_list_free (fstab_mount_points);
}

static void
update_mounts (GGduVolumeMonitor *monitor,
               GList **added_mounts,
               GList **removed_mounts)
{
  GList *new_mounts;
  GList *removed, *added;
  GList *l, *ll;
  GGduMount *mount;
  GGduVolume *volume;
  const char *device_file;
  const char *mount_path;

  new_mounts = g_unix_mounts_get (NULL);

  /* remove mounts we want to ignore - we do it here so we get to reevaluate
   * on the next update whether they should still be ignored
   */
  for (l = new_mounts; l != NULL; l = ll)
    {
      GUnixMountEntry *mount_entry = l->data;
      ll = l->next;

      /* keep in sync with should_mount_be_ignored() */
      if (!g_unix_mount_guess_should_display (mount_entry))
        {
          g_unix_mount_free (mount_entry);
          new_mounts = g_list_delete_link (new_mounts, l);
        }
    }

  new_mounts = g_list_sort (new_mounts, (GCompareFunc) g_unix_mount_compare);

  diff_sorted_lists (monitor->last_mounts,
                     new_mounts, (GCompareFunc) g_unix_mount_compare,
                     &added, &removed);

  for (l = removed; l != NULL; l = l->next)
    {
      GUnixMountEntry *mount_entry = l->data;

      mount = find_mount_by_mount_path (monitor, g_unix_mount_get_mount_path (mount_entry));
      if (mount)
        {
          /*g_debug ("removing mount %s", g_unix_mount_get_device_path (mount_entry));*/
          g_gdu_mount_unmounted (mount);
          monitor->mounts = g_list_remove (monitor->mounts, mount);

          *removed_mounts = g_list_prepend (*removed_mounts, mount);
        }
    }

  for (l = added; l != NULL; l = l->next)
    {
      GUnixMountEntry *mount_entry = l->data;

      device_file = g_unix_mount_get_device_path (mount_entry);
      mount_path = g_unix_mount_get_mount_path (mount_entry);
      volume = find_volume_for_device_file (monitor, device_file);
      if (volume == NULL)
        volume = find_volume_for_mount_path (monitor, mount_path);

      /*g_debug ("adding mount %s (vol %p) (device %s, mount_path %s)", g_unix_mount_get_device_path (mount_entry), volume, device_file, mount_path);*/
      mount = g_gdu_mount_new (G_VOLUME_MONITOR (monitor), mount_entry, volume);
      if (mount)
        {
          monitor->mounts = g_list_prepend (monitor->mounts, mount);
          *added_mounts = g_list_prepend (*added_mounts, g_object_ref (mount));
        }
    }

  g_list_free (added);
  g_list_free (removed);
  g_list_foreach (monitor->last_mounts,
                  (GFunc)g_unix_mount_free, NULL);
  g_list_free (monitor->last_mounts);
  monitor->last_mounts = new_mounts;
}

static void
update_discs (GGduVolumeMonitor *monitor,
              GList **added_volumes,
              GList **removed_volumes,
              GList **added_mounts,
              GList **removed_mounts)
{
  GList *cur_discs;
  GList *new_discs;
  GList *removed, *added;
  GList *l, *ll;
  GGduDrive *drive;
  GGduVolume *volume;
  GGduMount *mount;

  /* we also need to generate GVolume + GMount objects for
   *
   * - optical discs that have audio
   * - optical discs that are blank
   *
   */

  cur_discs = NULL;
  for (l = monitor->disc_volumes; l != NULL; l = l->next)
    cur_discs = g_list_prepend (cur_discs, g_gdu_volume_get_presentable (G_GDU_VOLUME (l->data)));

  new_discs = gdu_pool_get_presentables (monitor->pool);
  for (l = new_discs; l != NULL; l = ll)
    {
      GduPresentable *p = GDU_PRESENTABLE (l->data);
      GduDevice *d;
      gboolean ignore;

      ll = l->next;
      ignore = TRUE;

      /* filter out everything but discs that are blank or has audio */
      d = gdu_presentable_get_device (p);
      if (GDU_IS_VOLUME (p) && d != NULL && gdu_device_is_optical_disc (d))
        {
          if (gdu_device_optical_disc_get_num_audio_tracks (d) > 0 || gdu_device_optical_disc_get_is_blank (d))
            ignore = FALSE;
        }

      if (ignore)
        {
          g_object_unref (p);
          new_discs = g_list_delete_link (new_discs, l);
        }

      if (d != NULL)
        g_object_unref (d);
    }

  cur_discs = g_list_sort (cur_discs, (GCompareFunc) gdu_presentable_compare);
  new_discs = g_list_sort (new_discs, (GCompareFunc) gdu_presentable_compare);
  diff_sorted_lists (cur_discs, new_discs, (GCompareFunc) gdu_presentable_compare, &added, &removed);

  for (l = removed; l != NULL; l = l->next)
    {
      GduPresentable *p = GDU_PRESENTABLE (l->data);
      GduDevice *d;

      volume = NULL;
      mount = NULL;
      d = gdu_presentable_get_device (p);

      if (d != NULL)
        {
          volume = find_disc_volume_for_device_file (monitor, gdu_device_get_device_file (d));
          mount = find_disc_mount_for_volume (monitor, volume);
        }

      if (mount != NULL)
        {
          /*g_debug ("removing disc mount %s", gdu_device_get_device_file (d));*/
          g_gdu_mount_unmounted (mount);
          monitor->disc_mounts = g_list_remove (monitor->disc_mounts, mount);
          *removed_mounts = g_list_prepend (*removed_mounts, mount);
        }

      if (volume != NULL)
        {
          /*g_debug ("removing disc volume %s", gdu_device_get_device_file (d));*/
          g_gdu_volume_removed (volume);
          monitor->disc_volumes = g_list_remove (monitor->disc_volumes, volume);
          *removed_volumes = g_list_prepend (*removed_volumes, volume);
        }

      if (d != NULL)
        g_object_unref (d);
    }

  for (l = added; l != NULL; l = l->next)
    {
      GduPresentable *p = GDU_PRESENTABLE (l->data);
      GduDevice *d;
      gboolean is_blank;

      volume = NULL;
      is_blank = TRUE;
      d = gdu_presentable_get_device (p);

      if (d != NULL)
        {
          is_blank = gdu_device_optical_disc_get_is_blank (d);
          volume = find_disc_volume_for_device_file (monitor, gdu_device_get_device_file (d));
        }

      if (volume == NULL)
        {
          GduPresentable *toplevel_drive;

          toplevel_drive = gdu_presentable_get_enclosing_presentable (p);
          /* handle logical partitions enclosed by an extented partition */
          if (GDU_IS_VOLUME (toplevel_drive))
            {
              GduPresentable *temp;
              temp = toplevel_drive;
              toplevel_drive = gdu_presentable_get_enclosing_presentable (toplevel_drive);
              g_object_unref (temp);
            }

          if (toplevel_drive != NULL)
            {
              if (GDU_IS_DRIVE (toplevel_drive))
                {
                  GduDevice *toplevel_drive_device;

                  drive = NULL;
                  toplevel_drive_device = gdu_presentable_get_device (toplevel_drive);
                  if (toplevel_drive_device != NULL)
                    {
                      drive = find_drive_by_device_file (monitor, gdu_device_get_device_file (toplevel_drive_device));
                      /*g_debug ("adding volume %s (drive %s)",
                        gdu_device_get_device_file (d),
                        gdu_device_get_device_file (toplevel_device));*/
                      g_object_unref (toplevel_drive_device);
                    }
                }
              g_object_unref (toplevel_drive);
            }
          else
            {
              drive = NULL;
              /*g_debug ("adding volume %s (no drive)", gdu_device_get_device_file (d));*/
            }

          mount = NULL;
          if (is_blank)
            {
              volume = g_gdu_volume_new (G_VOLUME_MONITOR (monitor),
                                         GDU_VOLUME (p),
                                         drive,
                                         NULL);
              mount = g_gdu_mount_new (G_VOLUME_MONITOR (monitor),
                                       NULL,
                                       volume);
            }
          else
            {
              gchar *uri;
              gchar *device_basename;
              GFile *activation_root;

              /* the gvfsd-cdda backend uses URI's like these */
              device_basename = g_path_get_basename (gdu_device_get_device_file (d));
              uri = g_strdup_printf ("cdda://%s", device_basename);
              activation_root = g_file_new_for_uri (uri);
              g_free (device_basename);
              g_free (uri);

              volume = g_gdu_volume_new (G_VOLUME_MONITOR (monitor),
                                         GDU_VOLUME (p),
                                         drive,
                                         activation_root);

              g_object_unref (activation_root);
            }

          if (volume != NULL)
            {
              monitor->disc_volumes = g_list_prepend (monitor->disc_volumes, volume);
              *added_volumes = g_list_prepend (*added_volumes, g_object_ref (volume));

              if (mount != NULL)
                {
                  monitor->disc_mounts = g_list_prepend (monitor->disc_mounts, mount);
                  *added_mounts = g_list_prepend (*added_mounts, g_object_ref (mount));
                }
            }
        }

      if (d != NULL)
        g_object_unref (d);
    }

  g_list_free (added);
  g_list_free (removed);

  g_list_foreach (new_discs, (GFunc) g_object_unref, NULL);
  g_list_free (new_discs);

  g_list_free (cur_discs);
}
