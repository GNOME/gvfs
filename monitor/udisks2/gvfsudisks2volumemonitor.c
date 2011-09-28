/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2011 Red Hat, Inc.
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

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "gvfsudisks2volumemonitor.h"
#include "gvfsudisks2drive.h"
#include "gvfsudisks2volume.h"
#include "gvfsudisks2mount.h"

static GVfsUDisks2VolumeMonitor *the_volume_monitor = NULL;

typedef struct _GVfsUDisks2VolumeMonitorClass GVfsUDisks2VolumeMonitorClass;

struct _GVfsUDisks2VolumeMonitorClass
{
  GNativeVolumeMonitorClass parent_class;
};

struct _GVfsUDisks2VolumeMonitor
{
  GNativeVolumeMonitor parent;

  UDisksClient *client;
  GUdevClient *gudev_client;

  GList *last_mounts;

  GList *drives;
  GList *volumes;
  GList *mounts;
  /* we keep volumes/mounts for blank and audio discs separate to handle e.g. mixed discs properly */
  GList *disc_volumes;
  GList *disc_mounts;
};

static void object_list_free (GList *objects);
static UDisksClient *get_udisks_client_sync (GError **error);

static void update_all               (GVfsUDisks2VolumeMonitor  *monitor,
                                      gboolean                   emit_changes);
static void update_drives            (GVfsUDisks2VolumeMonitor  *monitor,
                                      GList                    **added_drives,
                                      GList                    **removed_drives);
static void update_volumes           (GVfsUDisks2VolumeMonitor  *monitor,
                                      GList                    **added_volumes,
                                      GList                    **removed_volumes);
static void update_mounts            (GVfsUDisks2VolumeMonitor  *monitor,
                                      GList                    **added_mounts,
                                      GList                    **removed_mounts);
static void update_discs             (GVfsUDisks2VolumeMonitor  *monitor,
                                      GList                    **added_volumes,
                                      GList                    **removed_volumes,
                                      GList                    **added_mounts,
                                      GList                    **removed_mounts);


static void on_object_added (GDBusObjectManager  *manager,
                             GDBusObject         *object,
                             gpointer             user_data);

static void on_object_removed (GDBusObjectManager  *manager,
                               GDBusObject         *object,
                               gpointer             user_data);

static void on_interface_added (GDBusObjectManager  *manager,
                                GDBusObject         *object,
                                GDBusInterface      *interface,
                                gpointer             user_data);

static void on_interface_removed (GDBusObjectManager  *manager,
                                  GDBusObject         *object,
                                  GDBusInterface      *interface,
                                  gpointer             user_data);

static void on_interface_proxy_properties_changed (GDBusObjectManagerClient   *manager,
                                                   GDBusObjectProxy           *object_proxy,
                                                   GDBusProxy                 *interface_proxy,
                                                   GVariant                   *changed_properties,
                                                   const gchar *const         *invalidated_properties,
                                                   gpointer                    user_data);

G_DEFINE_TYPE (GVfsUDisks2VolumeMonitor, gvfs_udisks2_volume_monitor, G_TYPE_NATIVE_VOLUME_MONITOR)

static void
gvfs_udisks2_volume_monitor_dispose (GObject *object)
{
  the_volume_monitor = NULL;

  if (G_OBJECT_CLASS (gvfs_udisks2_volume_monitor_parent_class)->dispose != NULL)
    G_OBJECT_CLASS (gvfs_udisks2_volume_monitor_parent_class)->dispose (object);
}

static void
gvfs_udisks2_volume_monitor_finalize (GObject *object)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (object);
  GDBusObjectManager *object_manager;

  object_manager = udisks_client_get_object_manager (monitor->client);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_object_added),
                                        monitor);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_object_removed),
                                        monitor);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_interface_added),
                                        monitor);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_interface_removed),
                                        monitor);
  g_signal_handlers_disconnect_by_func (object_manager,
                                        G_CALLBACK (on_interface_proxy_properties_changed),
                                        monitor);

  g_clear_object (&monitor->client);
  g_clear_object (&monitor->gudev_client);

  g_list_foreach (monitor->last_mounts, (GFunc) g_unix_mount_free, NULL);
  g_list_free (monitor->last_mounts);

  object_list_free (monitor->drives);
  object_list_free (monitor->volumes);
  object_list_free (monitor->mounts);

  object_list_free (monitor->disc_volumes);
  object_list_free (monitor->disc_mounts);

  G_OBJECT_CLASS (gvfs_udisks2_volume_monitor_parent_class)->finalize (object);
}

static GList *
get_mounts (GVolumeMonitor *_monitor)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (_monitor);
  GList *ret;

  ret = g_list_copy (monitor->mounts);
  ret = g_list_concat (ret, g_list_copy (monitor->disc_mounts));
  g_list_foreach (ret, (GFunc) g_object_ref, NULL);
  return ret;
}

static GList *
get_volumes (GVolumeMonitor *_monitor)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (_monitor);
  GList *ret;

  ret = g_list_copy (monitor->volumes);
  ret = g_list_concat (ret, g_list_copy (monitor->disc_volumes));
  g_list_foreach (ret, (GFunc) g_object_ref, NULL);
  return ret;
}

static GList *
get_connected_drives (GVolumeMonitor *_monitor)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (_monitor);
  GList *ret;

  ret = g_list_copy (monitor->drives);
  g_list_foreach (ret, (GFunc) g_object_ref, NULL);
  return ret;
}

static GVolume *
get_volume_for_uuid (GVolumeMonitor *_monitor,
                     const gchar    *uuid)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (_monitor);
  GVfsUDisks2Volume *volume;
  GList *l;

  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      volume = l->data;
      if (gvfs_udisks2_volume_has_uuid (l->data, uuid))
        goto found;
    }
  for (l = monitor->disc_volumes; l != NULL; l = l->next)
    {
      volume = l->data;
      if (gvfs_udisks2_volume_has_uuid (volume, uuid))
        goto found;
    }

  return NULL;

 found:
  return G_VOLUME (g_object_ref (volume));
}

static GMount *
get_mount_for_uuid (GVolumeMonitor *_monitor,
                    const gchar    *uuid)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (_monitor);
  GVfsUDisks2Mount *mount;
  GList *l;

  for (l = monitor->mounts; l != NULL; l = l->next)
    {
      mount = l->data;
      if (gvfs_udisks2_mount_has_uuid (l->data, uuid))
        goto found;
    }
  for (l = monitor->disc_mounts; l != NULL; l = l->next)
    {
      mount = l->data;
      if (gvfs_udisks2_mount_has_uuid (mount, uuid))
        goto found;
    }

  return NULL;

 found:
  return G_MOUNT (g_object_ref (mount));
}

static GMount *
get_mount_for_mount_path (const gchar  *mount_path,
                          GCancellable *cancellable)
{
  GVfsUDisks2VolumeMonitor *monitor = NULL;
  GMount *ret = NULL;

  if (the_volume_monitor == NULL)
    {
      /* Bah, no monitor is set up.. so we have to create one, find
       * what the user asks for and throw it away again.
       */
      monitor = GVFS_UDISKS2_VOLUME_MONITOR (gvfs_udisks2_volume_monitor_new ());
    }
  else
    {
      monitor = g_object_ref (the_volume_monitor);
    }

  /* creation of the volume monitor could fail */
  if (monitor != NULL)
    {
      GList *l;
      for (l = monitor->mounts; l != NULL; l = l->next)
        {
          GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (l->data);
          if (g_strcmp0 (gvfs_udisks2_mount_get_mount_path (mount), mount_path) == 0)
            {
              ret = g_object_ref (mount);
              goto out;
            }
        }
    }

 out:
  if (monitor != NULL)
    g_object_unref (monitor);
  return ret;
}

static GObject *
gvfs_udisks2_volume_monitor_constructor (GType                  type,
                                         guint                  n_construct_properties,
                                         GObjectConstructParam *construct_properties)
{
  GObject *ret = NULL;
  GObjectClass *parent_class;

  if (the_volume_monitor != NULL)
    {
      ret = g_object_ref (the_volume_monitor);
      goto out;
    }

  parent_class = G_OBJECT_CLASS (g_type_class_peek_parent (g_type_class_peek (type)));
  ret = parent_class->constructor (type,
                                   n_construct_properties,
                                   construct_properties);

  the_volume_monitor = GVFS_UDISKS2_VOLUME_MONITOR (ret);

 out:
  return ret;
}

static void
gvfs_udisks2_volume_monitor_init (GVfsUDisks2VolumeMonitor *monitor)
{
  GDBusObjectManager *object_manager;

  monitor->client = get_udisks_client_sync (NULL);
  monitor->gudev_client = g_udev_client_new (NULL); /* don't listen to any changes */

  object_manager = udisks_client_get_object_manager (monitor->client);
  g_signal_connect (object_manager,
                    "object-added",
                    G_CALLBACK (on_object_added),
                    monitor);
  g_signal_connect (object_manager,
                    "object-removed",
                    G_CALLBACK (on_object_removed),
                    monitor);
  g_signal_connect (object_manager,
                    "interface-added",
                    G_CALLBACK (on_interface_added),
                    monitor);
  g_signal_connect (object_manager,
                    "interface-removed",
                    G_CALLBACK (on_interface_removed),
                    monitor);
  g_signal_connect (object_manager,
                    "interface-proxy-properties-changed",
                    G_CALLBACK (on_interface_proxy_properties_changed),
                    monitor);

  update_all (monitor, FALSE);
}

static gboolean
is_supported (void)
{
  if (get_udisks_client_sync (NULL) != NULL)
    return TRUE;
  return FALSE;
}

static void
gvfs_udisks2_volume_monitor_class_init (GVfsUDisks2VolumeMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVolumeMonitorClass *monitor_class = G_VOLUME_MONITOR_CLASS (klass);
  GNativeVolumeMonitorClass *native_class = G_NATIVE_VOLUME_MONITOR_CLASS (klass);

  gobject_class->constructor = gvfs_udisks2_volume_monitor_constructor;
  gobject_class->finalize = gvfs_udisks2_volume_monitor_finalize;
  gobject_class->dispose = gvfs_udisks2_volume_monitor_dispose;

  monitor_class->get_mounts = get_mounts;
  monitor_class->get_volumes = get_volumes;
  monitor_class->get_connected_drives = get_connected_drives;
  monitor_class->get_volume_for_uuid = get_volume_for_uuid;
  monitor_class->get_mount_for_uuid = get_mount_for_uuid;
  monitor_class->is_supported = is_supported;

  native_class->get_mount_for_mount_path = get_mount_for_mount_path;
}

/**
 * gvfs_udisks2_volume_monitor_new:
 *
 * Returns:  a new #GVolumeMonitor.
 **/
GVolumeMonitor *
gvfs_udisks2_volume_monitor_new (void)
{
  return G_VOLUME_MONITOR (g_object_new (GVFS_TYPE_UDISKS2_VOLUME_MONITOR, NULL));
}

/* ---------------------------------------------------------------------------------------------------- */

UDisksClient *
gvfs_udisks2_volume_monitor_get_udisks_client (GVfsUDisks2VolumeMonitor *monitor)
{
  g_return_val_if_fail (GVFS_IS_UDISKS2_VOLUME_MONITOR (monitor), NULL);
  return monitor->client;
}

/* ---------------------------------------------------------------------------------------------------- */

GUdevClient *
gvfs_udisks2_volume_monitor_get_gudev_client (GVfsUDisks2VolumeMonitor *monitor)
{
  g_return_val_if_fail (GVFS_IS_UDISKS2_VOLUME_MONITOR (monitor), NULL);
  return monitor->gudev_client;
}

/* ---------------------------------------------------------------------------------------------------- */

void
gvfs_udisks2_volume_monitor_update (GVfsUDisks2VolumeMonitor *monitor)
{
  g_return_if_fail (GVFS_IS_UDISKS2_VOLUME_MONITOR (monitor));
  udisks_client_settle (monitor->client);
  update_all (monitor, TRUE);
}

/* ---------------------------------------------------------------------------------------------------- */

static UDisksClient *
get_udisks_client_sync (GError **error)
{
  static UDisksClient *_client = NULL;
  static GError *_error = NULL;
  static volatile gsize initialized = 0;

  if (g_once_init_enter (&initialized))
    {
      _client = udisks_client_new_sync (NULL, &_error);
      g_once_init_leave (&initialized, 1);
    }

  if (_error != NULL && error != NULL)
    *error = g_error_copy (_error);

  return _client;
}

/* ---------------------------------------------------------------------------------------------------- */

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

/* ---------------------------------------------------------------------------------------------------- */

static void
object_list_free (GList *objects)
{
  g_list_foreach (objects, (GFunc)g_object_unref, NULL);
  g_list_free (objects);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
object_list_emit (GVfsUDisks2VolumeMonitor *monitor,
                  const gchar              *monitor_signal,
                  const gchar              *object_signal,
                  GList                    *objects)
{
  GList *l;
  for (l = objects; l != NULL; l = l->next)
    {
      g_signal_emit_by_name (monitor, monitor_signal, l->data);
      if (object_signal)
        g_signal_emit_by_name (l->data, object_signal);
    }
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_object_added (GDBusObjectManager  *manager,
                 GDBusObject         *object,
                 gpointer             user_data)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (user_data);
  // g_debug ("on_object_added %s", g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
  update_all (monitor, TRUE);
}

static void
on_object_removed (GDBusObjectManager  *manager,
                   GDBusObject         *object,
                   gpointer             user_data)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (user_data);
  // g_debug ("on_object_removed %s", g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
  update_all (monitor, TRUE);
}

static void
on_interface_added (GDBusObjectManager  *manager,
                    GDBusObject         *object,
                    GDBusInterface      *interface,
                    gpointer             user_data)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (user_data);
  // g_debug ("on_interface_added %s", g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
  update_all (monitor, TRUE);
}

static void
on_interface_removed (GDBusObjectManager  *manager,
                      GDBusObject         *object,
                      GDBusInterface      *interface,
                      gpointer             user_data)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (user_data);
  // g_debug ("on_interface_removed %s", g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
  update_all (monitor, TRUE);
}

static void
on_interface_proxy_properties_changed (GDBusObjectManagerClient   *manager,
                                       GDBusObjectProxy           *object_proxy,
                                       GDBusProxy                 *interface_proxy,
                                       GVariant                   *changed_properties,
                                       const gchar *const         *invalidated_properties,
                                       gpointer                    user_data)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (user_data);
  // g_debug ("on_interface_proxy_properties_changed %s", g_dbus_object_get_object_path (G_DBUS_OBJECT (object_proxy)));
  update_all (monitor, TRUE);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_all (GVfsUDisks2VolumeMonitor *monitor,
            gboolean                  emit_changes)
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
  update_mounts (monitor, &added_mounts, &removed_mounts);
  update_discs (monitor,
                &added_volumes, &removed_volumes,
                &added_mounts, &removed_mounts);

  if (emit_changes)
    {
      object_list_emit (monitor,
                        "drive-disconnected", NULL,
                        removed_drives);
      object_list_emit (monitor,
                        "drive-connected", NULL,
                        added_drives);

      object_list_emit (monitor,
                        "volume-removed", "removed",
                        removed_volumes);
      object_list_emit (monitor,
                        "volume-added", NULL,
                        added_volumes);

      object_list_emit (monitor,
                        "mount-removed", "unmounted",
                        removed_mounts);
      object_list_emit (monitor,
                        "mount-added", NULL,
                        added_mounts);
    }

  object_list_free (removed_drives);
  object_list_free (added_drives);
  object_list_free (removed_volumes);
  object_list_free (added_volumes);
  object_list_free (removed_mounts);
  object_list_free (added_mounts);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
should_include_mount (GVfsUDisks2VolumeMonitor *monitor,
                      GUnixMountEntry          *mount_entry)
{
  gboolean ret = FALSE;
  ret = g_unix_mount_guess_should_display (mount_entry);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
should_include_volume_check_mount_points (GVfsUDisks2VolumeMonitor *monitor,
                                          UDisksBlock              *block)
{
  gboolean ret = TRUE;
  GDBusObject *obj;
  UDisksFilesystem *fs;
  const gchar* const *mount_points;
  guint n;

  obj = g_dbus_interface_get_object (G_DBUS_INTERFACE (block));
  if (obj == NULL)
    goto out;

  fs = udisks_object_peek_filesystem (UDISKS_OBJECT (obj));
  if (fs == NULL)
    goto out;

  mount_points = udisks_filesystem_get_mount_points (fs);
  for (n = 0; mount_points != NULL && mount_points[n] != NULL; n++)
    {
      const gchar *mount_point = mount_points[n];
      GUnixMountEntry *mount_entry;

      mount_entry = g_unix_mount_at (mount_point, NULL);
      if (mount_entry != NULL)
        {
          if (!should_include_mount (monitor, mount_entry))
            {
              g_unix_mount_free (mount_entry);
              ret = FALSE;
              goto out;
            }
          g_unix_mount_free (mount_entry);
        }
    }

 out:
  return ret;
}

static gboolean
should_include_volume (GVfsUDisks2VolumeMonitor *monitor,
                       UDisksBlock              *block)
{
  gboolean ret = FALSE;

  /* Check should_include_mount() for all mount points, if any - e.g. if a volume
   * is mounted in a place where the mount is to be ignored, we ignore the volume
   * as well
   */
  if (!should_include_volume_check_mount_points (monitor, block))
    goto out;

  /* TODO: handle crypto, fstab and a bunch of other stuff */
  if (g_strcmp0 (udisks_block_get_id_usage (block), "filesystem") != 0)
    goto out;

  ret = TRUE;

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
should_include_drive (GVfsUDisks2VolumeMonitor *monitor,
                      UDisksDrive              *drive)
{
  gboolean ret = TRUE;
  /* TODO */
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gint
udisks_drive_compare (UDisksDrive *a, UDisksDrive *b)
{
  return g_strcmp0 (g_dbus_object_get_object_path (g_dbus_interface_get_object (G_DBUS_INTERFACE (a))),
                    g_dbus_object_get_object_path (g_dbus_interface_get_object (G_DBUS_INTERFACE (b))));
}

static gint
block_compare (UDisksBlock *a, UDisksBlock *b)
{
  return g_strcmp0 (udisks_block_get_device (a), udisks_block_get_device (b));
}

/* ---------------------------------------------------------------------------------------------------- */

static GVfsUDisks2Drive *
find_drive_for_udisks_drive (GVfsUDisks2VolumeMonitor *monitor,
                             UDisksDrive              *udisks_drive)
{
  GVfsUDisks2Drive *ret = NULL;
  GList *l;

  for (l = monitor->drives; l != NULL; l = l->next)
    {
      GVfsUDisks2Drive *drive = GVFS_UDISKS2_DRIVE (l->data);
      if (gvfs_udisks2_drive_get_udisks_drive (drive) == udisks_drive)
        {
          ret = drive;
          goto out;
        }
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static GVfsUDisks2Volume *
find_volume_for_block (GVfsUDisks2VolumeMonitor *monitor,
                       UDisksBlock              *block)
{
  GVfsUDisks2Volume *ret = NULL;
  GList *l;

  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (l->data);
      if (gvfs_udisks2_volume_get_block (volume) == block)
        {
          ret = volume;
          goto out;
        }
    }

  for (l = monitor->disc_volumes; l != NULL; l = l->next)
    {
      GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (l->data);
      if (gvfs_udisks2_volume_get_block (volume) == block)
        {
          ret = volume;
          goto out;
        }
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static GVfsUDisks2Volume *
find_volume_for_device (GVfsUDisks2VolumeMonitor *monitor,
                        const gchar              *device)
{
  GVfsUDisks2Volume *ret = NULL;
  GList *l;
  struct stat statbuf;

  if (stat (device, &statbuf) != 0)
    goto out;

  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (l->data);
      if (gvfs_udisks2_volume_get_dev (volume) == statbuf.st_rdev)
        {
          ret = volume;
          goto out;
        }
    }

  for (l = monitor->disc_volumes; l != NULL; l = l->next)
    {
      GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (l->data);
      if (gvfs_udisks2_volume_get_dev (volume) == statbuf.st_rdev)
        {
          ret = volume;
          goto out;
        }
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static GVfsUDisks2Mount *
find_mount_by_mount_path (GVfsUDisks2VolumeMonitor *monitor,
                          const gchar              *mount_path)
{
  GVfsUDisks2Mount *ret = NULL;
  GList *l;

  for (l = monitor->mounts; l != NULL; l = l->next)
    {
      GVfsUDisks2Mount *mount = GVFS_UDISKS2_MOUNT (l->data);
      if (g_strcmp0 (gvfs_udisks2_mount_get_mount_path (mount), mount_path) == 0)
        {
          ret = mount;
          goto out;
        }
    }
 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_drives (GVfsUDisks2VolumeMonitor  *monitor,
               GList                    **added_drives,
               GList                    **removed_drives)
{
  GList *cur_udisks_drives;
  GList *new_udisks_drives;
  GList *removed, *added;
  GList *l;
  GVfsUDisks2Drive *drive;
  GList *objects;

  objects = g_dbus_object_manager_get_objects (udisks_client_get_object_manager (monitor->client));

  cur_udisks_drives = NULL;
  for (l = monitor->drives; l != NULL; l = l->next)
    {
      cur_udisks_drives = g_list_prepend (cur_udisks_drives,
                                          gvfs_udisks2_drive_get_udisks_drive (GVFS_UDISKS2_DRIVE (l->data)));
    }

  /* remove devices we want to ignore - we do it here so we get to reevaluate
   * on the next update whether they should still be ignored
   */
  new_udisks_drives = NULL;
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksDrive *udisks_drive = udisks_object_peek_drive (UDISKS_OBJECT (l->data));
      if (udisks_drive == NULL)
        continue;
      if (should_include_drive (monitor, udisks_drive))
        new_udisks_drives = g_list_prepend (new_udisks_drives, udisks_drive);
    }

  cur_udisks_drives = g_list_sort (cur_udisks_drives, (GCompareFunc) udisks_drive_compare);
  new_udisks_drives = g_list_sort (new_udisks_drives, (GCompareFunc) udisks_drive_compare);
  diff_sorted_lists (cur_udisks_drives,
                     new_udisks_drives, (GCompareFunc) udisks_drive_compare,
                     &added, &removed);

  for (l = removed; l != NULL; l = l->next)
    {
      UDisksDrive *udisks_drive = UDISKS_DRIVE (l->data);

      drive = find_drive_for_udisks_drive (monitor, udisks_drive);
      if (drive != NULL)
        {
          /*g_debug ("removing drive %s", gdu_presentable_get_id (p));*/
          gvfs_udisks2_drive_disconnected (drive);
          monitor->drives = g_list_remove (monitor->drives, drive);
          *removed_drives = g_list_prepend (*removed_drives, g_object_ref (drive));
          g_object_unref (drive);
        }
    }

  for (l = added; l != NULL; l = l->next)
    {
      UDisksDrive *udisks_drive = UDISKS_DRIVE (l->data);

      drive = find_drive_for_udisks_drive (monitor, udisks_drive);
      if (drive == NULL)
        {
          /*g_debug ("adding drive %s", gdu_presentable_get_id (p));*/
          drive = gvfs_udisks2_drive_new (monitor, udisks_drive);
          if (udisks_drive != NULL)
            {
              monitor->drives = g_list_prepend (monitor->drives, drive);
              *added_drives = g_list_prepend (*added_drives, g_object_ref (drive));
            }
        }
    }

  g_list_free (added);
  g_list_free (removed);

  g_list_free (cur_udisks_drives);
  g_list_free (new_udisks_drives);

  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_volumes (GVfsUDisks2VolumeMonitor  *monitor,
                GList                    **added_volumes,
                GList                    **removed_volumes)
{
  GList *cur_block_volumes;
  GList *new_block_volumes;
  GList *removed, *added;
  GList *l;
  GVfsUDisks2Volume *volume;
  GList *objects;

  objects = g_dbus_object_manager_get_objects (udisks_client_get_object_manager (monitor->client));

  cur_block_volumes = NULL;
  for (l = monitor->volumes; l != NULL; l = l->next)
    {
      cur_block_volumes = g_list_prepend (cur_block_volumes,
                                          gvfs_udisks2_volume_get_block (GVFS_UDISKS2_VOLUME (l->data)));
    }

  new_block_volumes = NULL;
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksBlock *block = udisks_object_peek_block (UDISKS_OBJECT (l->data));
      if (block == NULL)
        continue;
      if (should_include_volume (monitor, block))
        new_block_volumes = g_list_prepend (new_block_volumes, block);
    }

  cur_block_volumes = g_list_sort (cur_block_volumes, (GCompareFunc) block_compare);
  new_block_volumes = g_list_sort (new_block_volumes, (GCompareFunc) block_compare);
  diff_sorted_lists (cur_block_volumes,
                     new_block_volumes, (GCompareFunc) block_compare,
                     &added, &removed);

  for (l = removed; l != NULL; l = l->next)
    {
      UDisksBlock *block = UDISKS_BLOCK (l->data);
      volume = find_volume_for_block (monitor, block);
      if (volume != NULL)
        {
          gvfs_udisks2_volume_removed (volume);
          monitor->volumes = g_list_remove (monitor->volumes, volume);
          *removed_volumes = g_list_prepend (*removed_volumes, g_object_ref (volume));
          g_object_unref (volume);
        }
    }

  for (l = added; l != NULL; l = l->next)
    {
      UDisksBlock *block = UDISKS_BLOCK (l->data);
      volume = find_volume_for_block (monitor, block);
      if (volume == NULL)
        {
          GVfsUDisks2Drive *drive = NULL;
          UDisksDrive *udisks_drive;

          udisks_drive = udisks_client_get_drive_for_block (monitor->client, block);
          if (udisks_drive != NULL)
            {
              drive = find_drive_for_udisks_drive (monitor, udisks_drive);
              g_object_unref (udisks_drive);
            }
          volume = gvfs_udisks2_volume_new (monitor,
                                            block,
                                            drive,
                                            NULL); /* activation_root */
          if (volume != NULL)
            {
              monitor->volumes = g_list_prepend (monitor->volumes, volume);
              *added_volumes = g_list_prepend (*added_volumes, g_object_ref (volume));
            }
         }
    }

  g_list_free (added);
  g_list_free (removed);
  g_list_free (new_block_volumes);
  g_list_free (cur_block_volumes);

  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_mounts (GVfsUDisks2VolumeMonitor  *monitor,
               GList                    **added_mounts,
               GList                    **removed_mounts)
{
  GList *new_mounts;
  GList *removed, *added;
  GList *l, *ll;
  GVfsUDisks2Mount *mount;
  GVfsUDisks2Volume *volume;

  new_mounts = g_unix_mounts_get (NULL);

  /* remove mounts we want to ignore - we do it here so we get to reevaluate
   * on the next update whether they should still be ignored
   */
  for (l = new_mounts; l != NULL; l = ll)
    {
      GUnixMountEntry *mount_entry = l->data;
      ll = l->next;
      if (!should_include_mount (monitor, mount_entry))
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
      if (mount != NULL)
        {
          gvfs_udisks2_mount_unmounted (mount);
          monitor->mounts = g_list_remove (monitor->mounts, mount);
          *removed_mounts = g_list_prepend (*removed_mounts, g_object_ref (mount));
          g_debug ("removed mount at %s", gvfs_udisks2_mount_get_mount_path (mount));
          g_object_unref (mount);
        }
    }

  for (l = added; l != NULL; l = l->next)
    {
      GUnixMountEntry *mount_entry = l->data;
      const gchar *device_file;

      device_file = g_unix_mount_get_device_path (mount_entry);
      volume = find_volume_for_device (monitor, device_file);
      mount = gvfs_udisks2_mount_new (monitor, mount_entry, volume); /* adopts mount_entry */
      if (mount != NULL)
        {
          monitor->mounts = g_list_prepend (monitor->mounts, mount);
          *added_mounts = g_list_prepend (*added_mounts, g_object_ref (mount));
          g_debug ("added mount at %s for %p", gvfs_udisks2_mount_get_mount_path (mount), volume);
        }
    }

  g_list_free (added);
  g_list_free (removed);
  g_list_foreach (monitor->last_mounts, (GFunc) g_unix_mount_free, NULL);
  g_list_free (monitor->last_mounts);
  monitor->last_mounts = new_mounts;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_discs (GVfsUDisks2VolumeMonitor  *monitor,
              GList                    **added_volumes,
              GList                    **removed_volumes,
              GList                    **added_mounts,
              GList                    **removed_mounts)
{
}

/* ---------------------------------------------------------------------------------------------------- */
