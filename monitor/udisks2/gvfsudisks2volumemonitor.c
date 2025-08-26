/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2011-2012 Red Hat, Inc.
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
#include "gvfsudisks2utils.h"

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
  GUnixMountMonitor *mount_monitor;

  GHashTable *drives_by_udisks_drive; /* UDisksDrive * ~> GVfsUDisks2Drive * */
  GHashTable *volumes; /* GVfsUDisks2Volume * */
  GHashTable *volumes_by_dev_id; /* guint64 *dev_id ~> GVfsUDisks2Volume * */
  GHashTable *fstab_volumes; /* GVfsUDisks2Volume * */
  GHashTable *mounts; /* gchar *path ~> GVfsUDisks2Mount * */
  /* we keep volumes/mounts for blank and audio discs separate to handle e.g. mixed discs properly */
  GHashTable *disc_volumes;
  GHashTable *disc_volumes_by_dev_id; /* guint64 *dev_id ~> GVfsUDisks2Volume * */
  GHashTable *disc_mounts; /* GVfsUDisks2Mount * */

  GSettings *lockdown_settings;
  gboolean readonly_lockdown;

  gint update_id;
};

#define UPDATE_TIMEOUT 100 /* ms */

static UDisksClient *get_udisks_client_sync (GError **error);

static void update_all               (GVfsUDisks2VolumeMonitor  *monitor,
                                      gboolean                   emit_changes,
                                      gboolean                   coldplug);
static void update_drives            (GVfsUDisks2VolumeMonitor  *monitor,
                                      GList                    **added_drives,
                                      GList                    **removed_drives,
                                      gboolean                   coldplug);
static void update_volumes           (GVfsUDisks2VolumeMonitor  *monitor,
                                      GList                    **added_volumes,
                                      GList                    **removed_volumes,
                                      gboolean                   coldplug);
static void update_fstab_volumes     (GVfsUDisks2VolumeMonitor  *monitor,
                                      GList                    **added_volumes,
                                      GList                    **removed_volumes,
                                      gboolean                   coldplug);
static void update_mounts            (GVfsUDisks2VolumeMonitor  *monitor,
                                      GList                    **added_mounts,
                                      GList                    **removed_mounts,
                                      gboolean                   coldplug);

#if defined(HAVE_BURN) || defined(HAVE_CDDA)
static void update_discs             (GVfsUDisks2VolumeMonitor  *monitor,
                                      GList                    **added_volumes,
                                      GList                    **removed_volumes,
                                      GList                    **added_mounts,
                                      GList                    **removed_mounts,
                                      gboolean                   coldplug);
#endif


static void on_client_changed (UDisksClient *client,
                               gpointer      user_data);

static void mountpoints_changed      (GUnixMountMonitor  *mount_monitor,
                                      gpointer            user_data);

static void mounts_changed           (GUnixMountMonitor  *mount_monitor,
                                      gpointer            user_data);

G_DEFINE_TYPE (GVfsUDisks2VolumeMonitor, gvfs_udisks2_volume_monitor, G_TYPE_NATIVE_VOLUME_MONITOR)

static guint64 *
gvfs_dev_id_new (dev_t dev_id)
{
  guint64 *id = g_new0 (guint64, 1);
  *id = (guint64) dev_id;
  return id;
}

/* this mimics g_str_hash(), except it ignores the trailing slash in the path */
static guint
gvfs_mount_path_str_hash (gconstpointer ptr)
{
  const signed char *path = ptr;
  guint value = 5381;

  if (path == NULL || *path == '\0')
    return value;

  while (*path != '\0')
    {
      /* ignore trailing slash */
      if (*path == '/' && path[1] == '\0')
        break;
      value = (value << 5) + value + (*path);
      path++;
    }

  return value;
}

static gboolean
gvfs_mount_path_str_equal (gconstpointer ptr1,
                           gconstpointer ptr2)
{
  const gchar *path1 = ptr1, *path2 = ptr2;

  if (path1 == NULL || path2 == NULL)
    return path1 == path2;

  while (*path1 != '\0' && *path2 != '\0' && *path1 == *path2)
    {
      path1++;
      path2++;
    }

  /* ignore trailing slash */
  return (*path1 == '\0' && (*path2 == '\0' || (*path2 == '/' && path2[1] == '\0'))) ||
         (*path2 == '\0' && (*path1 == '\0' || (*path1 == '/' && path1[1] == '\0')));
}

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

  g_signal_handlers_disconnect_by_func (monitor->mount_monitor, mountpoints_changed, monitor);
  g_signal_handlers_disconnect_by_func (monitor->mount_monitor, mounts_changed, monitor);
  g_clear_object (&monitor->mount_monitor);

  g_signal_handlers_disconnect_by_func (monitor->client,
                                        G_CALLBACK (on_client_changed),
                                        monitor);

  g_clear_object (&monitor->client);
  g_clear_object (&monitor->gudev_client);

  g_clear_pointer (&monitor->drives_by_udisks_drive, g_hash_table_unref);
  g_clear_pointer (&monitor->volumes, g_hash_table_unref);
  g_clear_pointer (&monitor->volumes_by_dev_id, g_hash_table_unref);
  g_clear_pointer (&monitor->fstab_volumes, g_hash_table_unref);
  g_clear_pointer (&monitor->mounts, g_hash_table_unref);

  g_clear_pointer (&monitor->disc_volumes, g_hash_table_unref);
  g_clear_pointer (&monitor->disc_volumes_by_dev_id, g_hash_table_unref);
  g_clear_pointer (&monitor->disc_mounts, g_hash_table_unref);

  g_clear_object (&monitor->lockdown_settings);

  g_clear_handle_id (&monitor->update_id, g_source_remove);

  G_OBJECT_CLASS (gvfs_udisks2_volume_monitor_parent_class)->finalize (object);
}

static GList *
get_mounts (GVolumeMonitor *_monitor)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (_monitor);
  GList *ret = NULL;
  GHashTableIter iter;
  gpointer key = NULL, value = NULL;

  g_hash_table_iter_init (&iter, monitor->mounts);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      GVfsUDisks2Mount *mount = value;
      ret = g_list_prepend (ret, g_object_ref (mount));
    }
  g_hash_table_iter_init (&iter, monitor->disc_mounts);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      GVfsUDisks2Mount *mount = key;
      ret = g_list_prepend (ret, g_object_ref (mount));
    }
  return ret;
}

static GList *
get_volumes (GVolumeMonitor *_monitor)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (_monitor);
  GHashTableIter iter;
  gpointer key = NULL;
  GList *ret = NULL;

  g_hash_table_iter_init (&iter, monitor->volumes);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      ret = g_list_prepend (ret, g_object_ref (key));
    }
  g_hash_table_iter_init (&iter, monitor->fstab_volumes);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      ret = g_list_prepend (ret, g_object_ref (key));
    }
  g_hash_table_iter_init (&iter, monitor->disc_volumes);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      ret = g_list_prepend (ret, g_object_ref (key));
    }
  return ret;
}

static GList *
get_connected_drives (GVolumeMonitor *_monitor)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (_monitor);
  GList *ret = NULL;
  GHashTableIter iter;
  gpointer value = NULL;

  g_hash_table_iter_init (&iter, monitor->drives_by_udisks_drive);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      GVfsUDisks2Drive *drive = value;
      ret = g_list_prepend (ret, g_object_ref (drive));
    }
  return ret;
}

static GVolume *
get_volume_for_uuid (GVolumeMonitor *_monitor,
                     const gchar    *uuid)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (_monitor);
  GVfsUDisks2Volume *volume;
  GHashTableIter iter;
  gpointer key = NULL;

  g_hash_table_iter_init (&iter, monitor->volumes);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      volume = key;
      if (gvfs_udisks2_volume_has_uuid (volume, uuid))
        goto found;
    }
  g_hash_table_iter_init (&iter, monitor->fstab_volumes);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      volume = key;
      if (gvfs_udisks2_volume_has_uuid (volume, uuid))
        goto found;
    }
  g_hash_table_iter_init (&iter, monitor->disc_volumes);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      volume = key;
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
  GHashTableIter iter;
  gpointer key = NULL, value = NULL;

  g_hash_table_iter_init (&iter, monitor->mounts);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      mount = value;
      if (gvfs_udisks2_mount_has_uuid (mount, uuid))
        goto found;
    }
  g_hash_table_iter_init (&iter, monitor->disc_mounts);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      mount = key;
      if (gvfs_udisks2_mount_has_uuid (mount, uuid))
        goto found;
    }

  return NULL;

 found:
  return G_MOUNT (g_object_ref (mount));
}

static GVfsUDisks2Mount *
find_mount_by_mount_path (GVfsUDisks2VolumeMonitor *monitor,
                          const gchar              *mount_path)
{
  return g_hash_table_lookup (monitor->mounts, mount_path);
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
      GVfsUDisks2Mount *mount = find_mount_by_mount_path (monitor, mount_path);

      if (mount != NULL)
        {
          ret = G_MOUNT (g_object_ref (mount));
        }
    }

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
      ret = G_OBJECT (g_object_ref (the_volume_monitor));
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
lockdown_settings_changed (GSettings *settings,
                           gchar     *key,
                           gpointer   user_data)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (user_data);

  monitor->readonly_lockdown = g_settings_get_boolean (settings,
                                                       "mount-removable-storage-devices-as-read-only");
}

static void
gvfs_udisks2_volume_monitor_init (GVfsUDisks2VolumeMonitor *monitor)
{
  monitor->drives_by_udisks_drive = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_object_unref);
  monitor->volumes = g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, NULL);
  monitor->volumes_by_dev_id = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, g_object_unref);
  monitor->fstab_volumes = g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, NULL);
  monitor->mounts = g_hash_table_new_full (gvfs_mount_path_str_hash, gvfs_mount_path_str_equal, g_free, g_object_unref);
  monitor->disc_volumes = g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, NULL);
  monitor->disc_volumes_by_dev_id = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, g_object_unref);
  monitor->disc_mounts = g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, NULL);

  monitor->gudev_client = g_udev_client_new (NULL); /* don't listen to any changes */

  monitor->client = get_udisks_client_sync (NULL);
  g_signal_connect (monitor->client,
                    "changed",
                    G_CALLBACK (on_client_changed),
                    monitor);

  monitor->mount_monitor = g_unix_mount_monitor_get ();
  g_signal_connect (monitor->mount_monitor,
                    "mounts-changed",
                    G_CALLBACK (mounts_changed),
                    monitor);
  g_signal_connect (monitor->mount_monitor,
                    "mountpoints-changed",
                    G_CALLBACK (mountpoints_changed),
                    monitor);

  monitor->lockdown_settings = g_settings_new ("org.gnome.desktop.lockdown");
  monitor->readonly_lockdown = g_settings_get_boolean (monitor->lockdown_settings,
                                                       "mount-removable-storage-devices-as-read-only");
  g_signal_connect_object (monitor->lockdown_settings,
                           "changed",
                           G_CALLBACK (lockdown_settings_changed),
                           monitor,
                           0);

  update_all (monitor, FALSE, TRUE);
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

gboolean
gvfs_udisks2_volume_monitor_get_readonly_lockdown (GVfsUDisks2VolumeMonitor *monitor)
{
  g_return_val_if_fail (GVFS_IS_UDISKS2_VOLUME_MONITOR (monitor), FALSE);
  return monitor->readonly_lockdown;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
update_func (gpointer user_data)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (user_data);

  monitor->update_id = 0;

  update_all (monitor, TRUE, FALSE);

  return G_SOURCE_REMOVE;
}

static void
schedule_update (GVfsUDisks2VolumeMonitor *monitor)
{
  if (monitor->update_id != 0)
    return;

  monitor->update_id = g_timeout_add (UPDATE_TIMEOUT, update_func, monitor);
}

void
gvfs_udisks2_volume_monitor_update (GVfsUDisks2VolumeMonitor *monitor)
{
  g_return_if_fail (GVFS_IS_UDISKS2_VOLUME_MONITOR (monitor));
  udisks_client_settle (monitor->client);

  if (monitor->update_id != 0)
    g_source_remove (monitor->update_id);

  update_func (monitor);
}

/* ---------------------------------------------------------------------------------------------------- */

static UDisksClient *
get_udisks_client_sync (GError **error)
{
  static UDisksClient *_client = NULL;
  static GError *_error = NULL;
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized))
    {
      _client = udisks_client_new_sync (NULL, &_error);
      if (_error != NULL)
        g_warning ("Failed to connect to UDisks: %s", _error->message);

      g_once_init_leave (&initialized, 1);
    }

  if (_error != NULL && error != NULL)
    *error = g_error_copy (_error);

  return _client;
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
on_client_changed (UDisksClient  *client,
                   gpointer       user_data)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (user_data);
  schedule_update (monitor);
}

static void
mountpoints_changed (GUnixMountMonitor *mount_monitor,
                     gpointer           user_data)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (user_data);
  schedule_update (monitor);
}

static void
mounts_changed (GUnixMountMonitor *mount_monitor,
                gpointer           user_data)
{
  GVfsUDisks2VolumeMonitor *monitor = GVFS_UDISKS2_VOLUME_MONITOR (user_data);
  schedule_update (monitor);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_all (GVfsUDisks2VolumeMonitor *monitor,
            gboolean                  emit_changes,
            gboolean                  coldplug)
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

  update_drives (monitor, &added_drives, &removed_drives, coldplug);
  update_volumes (monitor, &added_volumes, &removed_volumes, coldplug);
  update_fstab_volumes (monitor, &added_volumes, &removed_volumes, coldplug);
  update_mounts (monitor, &added_mounts, &removed_mounts, coldplug);

#if defined(HAVE_BURN) || defined(HAVE_CDDA)
  update_discs (monitor,
                &added_volumes, &removed_volumes,
                &added_mounts, &removed_mounts,
                coldplug);
#endif

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

  g_list_free_full (removed_drives, g_object_unref);
  g_list_free_full (added_drives, g_object_unref);
  g_list_free_full (removed_volumes, g_object_unref);
  g_list_free_full (added_volumes, g_object_unref);
  g_list_free_full (removed_mounts, g_object_unref);
  g_list_free_full (added_mounts, g_object_unref);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
should_include (const gchar *mount_path,
                const gchar *options)
{
  gboolean ret = FALSE;
  const gchar *home_dir = NULL;
  const gchar *user_name;
  const gchar *p;

  g_return_val_if_fail (mount_path != NULL, FALSE);

  /* The x-gvfs-show option trumps everything else */
  if (options != NULL)
    {
      gchar *value;
      value = gvfs_udisks2_utils_lookup_fstab_options_value (options, "x-gvfs-show");
      if (value != NULL)
        {
          ret = TRUE;
          g_free (value);
          goto out;
        }
      value = gvfs_udisks2_utils_lookup_fstab_options_value (options, "x-gvfs-hide");
      if (value != NULL)
        {
          ret = FALSE;
          g_free (value);
          goto out;
        }
    }

  /* Never display internal mountpoints */
  if (g_unix_is_mount_path_system_internal (mount_path))
    goto out;

  /* Hide mounts within a subdirectory starting with a "." - suppose it was a purpose to hide this mount */
  if (g_strstr_len (mount_path, -1, "/.") != NULL)
    goto out;

  /* Check home dir */
  home_dir = g_get_home_dir ();
  if (home_dir != NULL)
    {
      if (g_str_has_prefix (mount_path, home_dir) && mount_path[strlen (home_dir)] == G_DIR_SEPARATOR)
        {
          ret = TRUE;
          goto out;
        }
    }

  /* Display mounts that are direct descendants of /media/ resp. /run/media/,
   * or mounts with /media/$USER/ resp. /run/media/$USER/ prefix.
   */
  p = mount_path;
  if (g_str_has_prefix (p, "/run/"))
    p += sizeof ("/run") - 1;
  if (g_str_has_prefix (p, "/media/"))
    {
      p += sizeof ("/media/") - 1;

      user_name = g_get_user_name ();
      if ((g_str_has_prefix (p, user_name) && p[strlen (user_name)] == '/') ||
          g_strrstr (p, "/") == NULL)
        {
          ret = TRUE;
          goto out;
        }
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
should_include_mount_point (GVfsUDisks2VolumeMonitor  *monitor,
                            GUnixMountPoint           *mount_point)
{
  return should_include (g_unix_mount_point_get_mount_path (mount_point),
                         g_unix_mount_point_get_options (mount_point));
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
should_include_mount (GVfsUDisks2VolumeMonitor  *monitor,
                      GUnixMountEntry           *mount_entry)
{
  GUnixMountPoint *mount_point;
  const gchar *options;
  gboolean ret = FALSE;
  const gchar *mount_path;

  if (g_strcmp0 (g_unix_mount_entry_get_fs_type (mount_entry), "autofs") == 0)
    {
      goto out;
    }

  /* If mounted at the designated mount point, use g_unix_mount_point_get_options
   * in prior to g_unix_mount_entry_get_options to keep support of "comment="
   * options, see https://gitlab.gnome.org/GNOME/gvfs/issues/348.
   */
  mount_path = g_unix_mount_entry_get_mount_path (mount_entry);
  mount_point = g_unix_mount_point_at (mount_path, NULL);
  if (mount_point != NULL)
    {
      ret = should_include_mount_point (monitor, mount_point);
      g_unix_mount_point_free (mount_point);
      goto out;
    }

  /* g_unix_mount_entry_get_options works only with libmount,
   * see https://bugzilla.gnome.org/show_bug.cgi?id=668132
   */
  options = g_unix_mount_entry_get_options (mount_entry);
  if (options != NULL)
    {
      ret = should_include (mount_path, options);
      goto out;
    }

  ret = should_include (mount_path, NULL);

 out:
  return ret;
}


/* ---------------------------------------------------------------------------------------------------- */

static gboolean
should_include_volume_check_mount_points (GVfsUDisks2VolumeMonitor *monitor,
                                          UDisksBlock              *block,
                                          GHashTable               *mount_entries) /* gchar *path ~> GUnixMountEntry * */
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

      mount_entry = g_hash_table_lookup (mount_entries, mount_point);
      if (mount_entry != NULL)
        {
          const gchar *root = g_unix_mount_entry_get_root_path (mount_entry);

          if ((root == NULL || g_strcmp0 (root, "/") == 0) &&
              should_include_mount (monitor, mount_entry))
            {
              ret = TRUE;
              goto out;
            }

          ret = FALSE;
        }
    }

 out:
  return ret;
}

static gboolean
should_include_volume_check_configuration (GVfsUDisks2VolumeMonitor *monitor,
                                           UDisksBlock              *block)
{
  gboolean ret = TRUE;
  GVariantIter iter;
  const gchar *configuration_type;
  GVariant *configuration_value;

  g_variant_iter_init (&iter, udisks_block_get_configuration (block));
  while (g_variant_iter_next (&iter, "(&s@a{sv})", &configuration_type, &configuration_value))
    {
      if (g_strcmp0 (configuration_type, "fstab") == 0)
        {
          const gchar *fstab_dir;
          const gchar *fstab_options;
          if (g_variant_lookup (configuration_value, "dir", "^&ay", &fstab_dir) &&
              g_variant_lookup (configuration_value, "opts", "^&ay", &fstab_options))
            {
              if (!should_include (fstab_dir, fstab_options))
                {
                  ret = FALSE;
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

static gboolean should_include_drive (GVfsUDisks2VolumeMonitor *monitor,
                                      UDisksDrive              *drive);

static gboolean
should_include_volume (GVfsUDisks2VolumeMonitor *monitor,
                       UDisksBlock              *block,
                       GHashTable               *mount_entries, /* gchar *path ~> GUnixMountEntry * */
                       gboolean                  allow_encrypted_cleartext)
{
  gboolean ret = FALSE;
  GDBusObject *object;
  UDisksFilesystem *filesystem;
  UDisksDrive *udisks_drive = NULL;
  const gchar* const *mount_points;
  UDisksLoop *loop = NULL;

  /* Block:Ignore trumps everything */
  if (udisks_block_get_hint_ignore (block))
    goto out;

  /* If the device (or if a partition, its containing device) is a
   * loop device, check the SetupByUid property - we don't want to
   * show loop devices set up by other users
   */
  loop = udisks_client_get_loop_for_block (monitor->client, block);
  if (loop != NULL)
    {
      GDBusObject *loop_object;
      UDisksBlock *block_for_loop;
      guint setup_by_uid;

      setup_by_uid = udisks_loop_get_setup_by_uid (loop);
      if (setup_by_uid != 0 && setup_by_uid != getuid ())
        goto out;

      /* Work-around bug in Linux where partitions of a loop
       * device (e.g. /dev/loop0p1) are lingering even when the
       * parent loop device (e.g. /dev/loop0) has been cleared
       */
      loop_object = g_dbus_interface_get_object (G_DBUS_INTERFACE (loop));
      if (loop_object == NULL)
        goto out;
      block_for_loop = udisks_object_peek_block (UDISKS_OBJECT (loop_object));
      if (block_for_loop == NULL)
        goto out;
      if (udisks_block_get_size (block_for_loop) == 0)
        goto out;
    }

  /* ignore the volume if the drive is ignored */
  udisks_drive = udisks_client_get_drive_for_block (monitor->client, block);
  if (udisks_drive != NULL)
    {
      if (!should_include_drive (monitor, udisks_drive))
        {
          goto out;
        }
    }

  /* show encrypted volumes... */
  if (g_strcmp0 (udisks_block_get_id_usage (block), "crypto") == 0)
    {
      UDisksBlock *cleartext_block;
      /* ... unless the volume is unlocked and we don't want to show the cleartext volume */
      cleartext_block = udisks_client_get_cleartext_block (monitor->client, block);
      if (cleartext_block != NULL)
        {
          ret = should_include_volume (monitor, cleartext_block, mount_entries, TRUE);
          g_object_unref (cleartext_block);
        }
      else
        {
          ret = TRUE;
        }
      goto out;
    }

  if (!allow_encrypted_cleartext)
    {
      /* ... but not unlocked volumes (because the volume for the encrypted part morphs
       * into the cleartext part when unlocked)
       */
      if (g_strcmp0 (udisks_block_get_crypto_backing_device (block), "/") != 0)
        {
          goto out;
        }
    }

  /* Check should_include_mount() for all mount points, if any - e.g. if a volume
   * is mounted in a place where the mount is to be ignored, we ignore the volume
   * as well
   */
  if (!should_include_volume_check_mount_points (monitor, block, mount_entries))
    goto out;

  object = g_dbus_interface_get_object (G_DBUS_INTERFACE (block));
  if (object == NULL)
    goto out;

  filesystem = udisks_object_peek_filesystem (UDISKS_OBJECT (object));
  if (filesystem == NULL)
    goto out;

  /* If not mounted but the volume is referenced in /etc/fstab and
   * that configuration indicates the volume should be ignored, then
   * do so
   */
  mount_points = udisks_filesystem_get_mount_points (filesystem);
  if (mount_points == NULL || g_strv_length ((gchar **) mount_points) == 0)
    {
      if (!should_include_volume_check_configuration (monitor, block))
        goto out;
    }

  /* otherwise, we're good to go */
  ret = TRUE;

 out:
  g_clear_object (&udisks_drive);
  g_clear_object (&loop);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
should_include_drive (GVfsUDisks2VolumeMonitor *monitor,
                      UDisksDrive              *drive)
{
  gboolean ret = TRUE;

  /* Don't include drives on other seats */
  if (!gvfs_udisks2_utils_is_drive_on_our_seat (drive))
    {
      ret = FALSE;
      goto out;
    }

  /* NOTE: For now, we just include a drive no matter its
   * content. This may be wrong ... for example non-removable drives
   * without anything visible (such RAID components) should probably
   * not be shown. Then again, the GNOME 3 user interface doesn't
   * really show GDrive instances except for in the computer:///
   * location in Nautilus....
   */

 out:

  return ret;
}

#if defined(HAVE_BURN) || defined(HAVE_CDDA)
static gboolean
should_include_disc (GVfsUDisks2VolumeMonitor *monitor,
                     UDisksDrive              *drive)
{
  gboolean ret = FALSE;

  /* only consider blank and audio discs */

#ifdef HAVE_BURN
  if (udisks_drive_get_optical_blank (drive))
    ret = TRUE;
#endif

#ifdef HAVE_CDDA
  if (udisks_drive_get_optical_num_audio_tracks (drive) > 0)
    ret = TRUE;
#endif

  return ret;
}
#endif

/* ---------------------------------------------------------------------------------------------------- */

#if defined(HAVE_BURN) || defined(HAVE_CDDA)
static void
add_disc_volume (GVfsUDisks2VolumeMonitor *monitor,
                 GVfsUDisks2Volume        *volume)
{
  g_hash_table_add (monitor->disc_volumes, g_object_ref (volume));
  g_hash_table_insert (monitor->disc_volumes_by_dev_id, gvfs_dev_id_new (gvfs_udisks2_volume_get_dev (volume)), g_object_ref (volume));
}

static void
remove_disc_volume (GVfsUDisks2VolumeMonitor *monitor,
                    GVfsUDisks2Volume        *volume)
{
  guint64 dev_id = (guint64) gvfs_udisks2_volume_get_dev (volume);
  g_hash_table_remove (monitor->disc_volumes_by_dev_id, &dev_id);
  g_hash_table_remove (monitor->disc_volumes, volume);
}
#endif

/* ---------------------------------------------------------------------------------------------------- */

static GVfsUDisks2Drive *
find_drive_for_udisks_drive (GVfsUDisks2VolumeMonitor *monitor,
                             UDisksDrive              *udisks_drive)
{
  return g_hash_table_lookup (monitor->drives_by_udisks_drive, udisks_drive);
}

static void
add_drive (GVfsUDisks2VolumeMonitor *monitor,
           GVfsUDisks2Drive         *drive)
{
  g_hash_table_insert (monitor->drives_by_udisks_drive, gvfs_udisks2_drive_get_udisks_drive (drive), g_object_ref (drive));
}

static void
remove_drive (GVfsUDisks2VolumeMonitor *monitor,
              GVfsUDisks2Drive         *drive)
{
  g_hash_table_remove (monitor->drives_by_udisks_drive, gvfs_udisks2_drive_get_udisks_drive (drive));
}

/* ---------------------------------------------------------------------------------------------------- */

static void
add_volume (GVfsUDisks2VolumeMonitor *monitor,
            GVfsUDisks2Volume        *volume)
{
  g_hash_table_add (monitor->volumes, g_object_ref (volume));
  g_hash_table_insert (monitor->volumes_by_dev_id, gvfs_dev_id_new (gvfs_udisks2_volume_get_dev (volume)), g_object_ref (volume));
}

/* ---------------------------------------------------------------------------------------------------- */

static void
remove_volume (GVfsUDisks2VolumeMonitor *monitor,
               GVfsUDisks2Volume        *volume)
{
  guint64 dev_id = (guint64) gvfs_udisks2_volume_get_dev (volume);
  g_hash_table_remove (monitor->volumes_by_dev_id, &dev_id);
  g_hash_table_remove (monitor->volumes, volume);
}

/* ---------------------------------------------------------------------------------------------------- */

static guint
gvfs_mount_point_hash (gconstpointer ptr)
{
  GUnixMountPoint *mount_point = (GUnixMountPoint *) ptr;
  return g_str_hash (g_unix_mount_point_get_mount_path (mount_point)) ^
         g_str_hash (g_unix_mount_point_get_device_path (mount_point));
}

static gboolean
gvfs_mount_point_equal (gconstpointer ptr1,
                        gconstpointer ptr2)
{
  GUnixMountPoint *mount_point1 = (GUnixMountPoint *) ptr1;
  GUnixMountPoint *mount_point2 = (GUnixMountPoint *) ptr2;
  return g_unix_mount_point_compare (mount_point1, mount_point2) == 0;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
mount_point_matches_mount_entry (GUnixMountPoint *mount_point,
                                 GUnixMountEntry *mount_entry)
{
  const gchar *mp_path;
  const gchar *mp_entry;

  mp_path = g_unix_mount_point_get_mount_path (mount_point);
  mp_entry = g_unix_mount_entry_get_mount_path (mount_entry);

  return gvfs_mount_path_str_equal (mp_path, mp_entry);
}

static GVfsUDisks2Volume *
find_fstab_volume_for_mount_entry (GVfsUDisks2VolumeMonitor *monitor,
                                   GUnixMountEntry          *mount_entry)
{
  GVfsUDisks2Volume *ret = NULL;
  GHashTableIter iter;
  gpointer key = NULL;

  g_hash_table_iter_init (&iter, monitor->fstab_volumes);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      GVfsUDisks2Volume *volume = GVFS_UDISKS2_VOLUME (key);
      if (mount_point_matches_mount_entry (gvfs_udisks2_volume_get_mount_point (volume), mount_entry))
        {
          ret = volume;
          goto out;
        }
    }

 out:
  return ret;
}

static GVfsUDisks2Mount *
find_lonely_mount_for_mount_point (GVfsUDisks2VolumeMonitor *monitor,
                                   GUnixMountPoint          *mount_point)
{
  GVfsUDisks2Mount *ret = NULL;
  GVfsUDisks2Mount *mount;

  mount = g_hash_table_lookup (monitor->mounts, g_unix_mount_point_get_mount_path (mount_point));
  if (mount != NULL)
    {
      GVolume *volume = g_mount_get_volume (G_MOUNT (mount));
      if (volume != NULL)
        {
          g_object_unref (volume);
        }
      else
        {
          ret = mount;
          goto out;
        }
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static const char *
_udisks_client_get_device_for_part (UDisksClient *client, const char *label, const char *uuid)
{
  GList *objects;
  const char *device = NULL;
  GList *l;

  objects = g_dbus_object_manager_get_objects (udisks_client_get_object_manager (client));
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksPartition *partition;
      UDisksBlock *block;

      partition = udisks_object_peek_partition (UDISKS_OBJECT (l->data));
      block = udisks_object_peek_block (UDISKS_OBJECT (l->data));
      if (partition == NULL || block == NULL)
        continue;

      if ((label != NULL && g_strcmp0 (udisks_partition_get_name (partition), label) == 0) ||
          (uuid != NULL && g_strcmp0 (udisks_partition_get_uuid (partition), uuid) == 0))
        {
          device = udisks_block_get_device (block);
          break;
        }
    }

  g_list_free_full (objects, g_object_unref);

  return device;
}

static GVfsUDisks2Volume *
find_volume_for_device (GVfsUDisks2VolumeMonitor *monitor,
                        const gchar              *device)
{
  GVfsUDisks2Volume *ret = NULL;
  GList *blocks = NULL;
  struct stat statbuf;
  guint64 dev_id;

  /* don't consider e.g. network mounts */
  if (g_str_has_prefix (device, "LABEL="))
    {
      blocks = udisks_client_get_block_for_label (monitor->client, device + 6);
      if (blocks != NULL)
        device = udisks_block_get_device (UDISKS_BLOCK (blocks->data));
      else
        goto out;
    }
  else if (g_str_has_prefix (device, "UUID="))
    {
      blocks = udisks_client_get_block_for_uuid (monitor->client, device + 5);
      if (blocks != NULL)
        device = udisks_block_get_device (UDISKS_BLOCK (blocks->data));
      else
        goto out;
    }
  else if (g_str_has_prefix (device, "PARTLABEL="))
    {
      device = _udisks_client_get_device_for_part (monitor->client, device + 10, NULL);
    }
  else if (g_str_has_prefix (device, "PARTUUID="))
    {
      device = _udisks_client_get_device_for_part (monitor->client, NULL, device + 9);
    }
  else if (!g_str_has_prefix (device, "/dev/"))
    {
      goto out;
    }

  if (device == NULL)
    goto out;

  if (stat (device, &statbuf) != 0)
    goto out;

  dev_id = (guint64) statbuf.st_rdev;

  ret = g_hash_table_lookup (monitor->volumes_by_dev_id, &dev_id);
  if (ret != NULL)
    goto out;

  ret = g_hash_table_lookup (monitor->disc_volumes_by_dev_id, &dev_id);
  if (ret != NULL)
    goto out;

 out:
  g_list_free_full (blocks, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_drives (GVfsUDisks2VolumeMonitor  *monitor,
               GList                    **added_drives,
               GList                    **removed_drives,
               gboolean                   coldplug)
{
  GHashTable *cur_udisks_drives; /* UDisksDrive * ~> GVfsUDisks2Drive * */
  GHashTableIter iter;
  gpointer key = NULL, value = NULL;
  GVfsUDisks2Drive *drive;
  GList *objects, *l;

  objects = g_dbus_object_manager_get_objects (udisks_client_get_object_manager (monitor->client));

  cur_udisks_drives = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_object_unref);

  g_hash_table_iter_init (&iter, monitor->drives_by_udisks_drive);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      g_hash_table_insert (cur_udisks_drives, key, g_object_ref (GVFS_UDISKS2_DRIVE (value)));
    }

  /* remove devices we want to ignore - we do it here so we get to reevaluate
   * on the next update whether they should still be ignored
   */
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksDrive *udisks_drive = udisks_object_peek_drive (UDISKS_OBJECT (l->data));
      if (udisks_drive == NULL)
        continue;
      if (should_include_drive (monitor, udisks_drive))
        {
          /* not in currently known drives => add it */
          if (!g_hash_table_remove (cur_udisks_drives, udisks_drive))
            {
              drive = gvfs_udisks2_drive_new (monitor, udisks_drive, coldplug);
              if (drive != NULL)
                {
                  add_drive (monitor, drive);
                  *added_drives = g_list_prepend (*added_drives, g_steal_pointer (&drive));
                }
            }
        }
    }

  /* which left are removed */
  g_hash_table_iter_init (&iter, cur_udisks_drives);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      drive = value;
      gvfs_udisks2_drive_disconnected (drive);
      *removed_drives = g_list_prepend (*removed_drives, g_object_ref (drive));
      remove_drive (monitor, drive);
    }

  g_hash_table_unref (cur_udisks_drives);

  g_list_free_full (objects, g_object_unref);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_volumes (GVfsUDisks2VolumeMonitor  *monitor,
                GList                    **added_volumes,
                GList                    **removed_volumes,
                gboolean                   coldplug)
{
  GHashTable *cur_block_volumes; /* UDisksBlock * ~> GVfsUDisks2Volume * */
  GHashTable *mount_entries; /* gchar *path ~> GUnixMountEntry * */
  GHashTableIter iter;
  gpointer key = NULL, value = NULL;
  GVfsUDisks2Volume *volume;
  GList *objects, *l;

  mount_entries = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) g_unix_mount_entry_free);

  /* move mount entries into a hash table */
  objects = g_unix_mount_entries_get (NULL);
  for (l = objects; l != NULL; l = g_list_next (l))
    {
      GUnixMountEntry *mount_entry = l->data;
      g_hash_table_insert (mount_entries, (gpointer) g_unix_mount_entry_get_mount_path (mount_entry), mount_entry);
    }
  /* the mount_entries took ownership of the mount entry objects */
  g_list_free (objects);

  objects = g_dbus_object_manager_get_objects (udisks_client_get_object_manager (monitor->client));

  cur_block_volumes = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_object_unref);

  g_hash_table_iter_init (&iter, monitor->volumes);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      volume = key;
      g_hash_table_insert (cur_block_volumes, gvfs_udisks2_volume_get_block (volume), g_object_ref (volume));
    }

  for (l = objects; l != NULL; l = l->next)
    {
      UDisksBlock *block = udisks_object_peek_block (UDISKS_OBJECT (l->data));
      if (block == NULL)
        continue;
      if (should_include_volume (monitor, block, mount_entries, FALSE))
        {
          /* not in currently known volumes => add it */
          if (!g_hash_table_remove (cur_block_volumes, block))
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
                                                NULL, /* mount_point */
                                                drive,
                                                NULL, /* activation_root */
                                                coldplug);
              if (volume != NULL)
                {
                  add_volume (monitor, volume);
                  *added_volumes = g_list_prepend (*added_volumes, g_steal_pointer (&volume));
                }
            }
        }
    }

  /* which left are removed */
  g_hash_table_iter_init (&iter, cur_block_volumes);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      volume = value;
      gvfs_udisks2_volume_removed (volume);
      *removed_volumes = g_list_prepend (*removed_volumes, g_object_ref (volume));
      remove_volume (monitor, volume);
    }

  g_hash_table_unref (cur_block_volumes);
  g_hash_table_unref (mount_entries);

  g_list_free_full (objects, g_object_unref);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
have_udisks_volume_for_mount_point (GVfsUDisks2VolumeMonitor *monitor,
                                    GUnixMountPoint          *mount_point)
{
  gboolean ret = FALSE;

  if (find_volume_for_device (monitor, g_unix_mount_point_get_device_path (mount_point)) == NULL)
    goto out;

  ret = TRUE;

 out:
  return ret;
}

static gboolean
mount_point_has_device (GVfsUDisks2VolumeMonitor  *monitor,
                        GUnixMountPoint           *mount_point)
{
  gboolean ret = FALSE;
  const gchar *device;
  struct stat statbuf;
  UDisksBlock *block;
  GList *blocks = NULL;

  device = g_unix_mount_point_get_device_path (mount_point);
  if (g_str_has_prefix (device, "LABEL="))
    {
      blocks = udisks_client_get_block_for_label (monitor->client, device + 6);
      if (blocks != NULL)
        device = udisks_block_get_device (UDISKS_BLOCK (blocks->data));
      else
        goto out;
    }
  else if (g_str_has_prefix (device, "UUID="))
    {
      blocks = udisks_client_get_block_for_uuid (monitor->client, device + 5);
      if (blocks != NULL)
        device = udisks_block_get_device (UDISKS_BLOCK (blocks->data));
      else
        goto out;
    }
  else if (g_str_has_prefix (device, "PARTLABEL="))
    {
      device = _udisks_client_get_device_for_part (monitor->client, device + 10, NULL);
    }
  else if (g_str_has_prefix (device, "PARTUUID="))
    {
      device = _udisks_client_get_device_for_part (monitor->client, NULL, device + 9);
    }
  else if (!g_str_has_prefix (device, "/dev/"))
    {
      /* NFS, CIFS and other non-device mounts always have a device */
      ret = TRUE;
      goto out;
    }

  if (device == NULL)
    goto out;

  if (stat (device, &statbuf) != 0)
    goto out;

  if (statbuf.st_rdev == 0)
    goto out;

  /* assume non-existant if media is not available */
  block = udisks_client_get_block_for_dev (monitor->client, statbuf.st_rdev);
  if (block != NULL)
    {
      UDisksDrive *drive;
      drive = udisks_client_get_drive_for_block (monitor->client, block);
      if (drive != NULL)
        {
          if (!udisks_drive_get_media_available (drive))
            {
              g_object_unref (drive);
              g_object_unref (block);
              goto out;
            }
          g_object_unref (drive);
        }
      g_object_unref (block);
    }
  else
    {
      /* not known by udisks, assume media is available */
    }

  ret = TRUE;

 out:
  g_list_free_full (blocks, g_object_unref);
  return ret;
}

static void
free_nonnull_unix_mount_point (gpointer ptr)
{
  GUnixMountPoint *mount_point = ptr;
  if (mount_point != NULL)
    g_unix_mount_point_free (mount_point);
}

static void
update_fstab_volumes (GVfsUDisks2VolumeMonitor  *monitor,
                      GList                    **added_volumes,
                      GList                    **removed_volumes,
                      gboolean                   coldplug)
{
  GHashTable *cur_mount_points; /* GUnixMountPoint * ~> GVfsUDisks2Volume * */
  GHashTableIter iter;
  gpointer key = NULL, value = NULL;
  GList *new_mount_points, *l;
  GVfsUDisks2Volume *volume;

  cur_mount_points = g_hash_table_new_full (gvfs_mount_point_hash, gvfs_mount_point_equal, NULL, g_object_unref);

  g_hash_table_iter_init (&iter, monitor->fstab_volumes);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      GUnixMountPoint *mount_point;
      volume = GVFS_UDISKS2_VOLUME (key);
      mount_point = gvfs_udisks2_volume_get_mount_point (volume);
      if (mount_point != NULL)
        g_hash_table_insert (cur_mount_points, mount_point, g_object_ref (volume));
    }

  new_mount_points = g_unix_mount_points_get (NULL);
  for (l = new_mount_points; l != NULL; l = g_list_next (l))
    {
      GUnixMountPoint *mount_point = l->data;

      /* use the mount points that we want to include */
      if (should_include_mount_point (monitor, mount_point) &&
          !have_udisks_volume_for_mount_point (monitor, mount_point) &&
          mount_point_has_device (monitor, mount_point))
        {
          /* not in currently known volumes => add it */
          if (!g_hash_table_remove (cur_mount_points, mount_point))
            {
              volume = gvfs_udisks2_volume_new (monitor,
                                                NULL,        /* block */
                                                mount_point,
                                                NULL,        /* drive */
                                                NULL,        /* activation_root */
                                                coldplug);
              if (volume != NULL)
                {
                  GVfsUDisks2Mount *mount;

                  /* Could be there's already a mount for this volume - for example, the
                   * user could just have added it to the /etc/fstab file
                   */
                  mount = find_lonely_mount_for_mount_point (monitor, mount_point);
                  if (mount != NULL)
                    gvfs_udisks2_mount_set_volume (mount, volume);

                  g_hash_table_add (monitor->fstab_volumes, g_object_ref (volume));
                  *added_volumes = g_list_prepend (*added_volumes, g_steal_pointer (&volume));
                  l->data = NULL;
                }
            }
        }
    }

  /* which left are removed */
  g_hash_table_iter_init (&iter, cur_mount_points);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      volume = value;
      gvfs_udisks2_volume_removed (volume);
      *removed_volumes = g_list_prepend (*removed_volumes, g_object_ref (volume));
      g_hash_table_remove (monitor->fstab_volumes, volume);
    }

  g_list_free_full (new_mount_points, free_nonnull_unix_mount_point);

  g_hash_table_unref (cur_mount_points);
}

/* ---------------------------------------------------------------------------------------------------- */

static guint
gvfs_mount_entry_hash (gconstpointer ptr)
{
  GUnixMountEntry *mount_entry = (GUnixMountEntry *) ptr;
  return g_str_hash (g_unix_mount_entry_get_mount_path (mount_entry)) ^
         g_str_hash (g_unix_mount_entry_get_device_path (mount_entry));
}

static gboolean
gvfs_mount_entry_equal (gconstpointer ptr1,
                        gconstpointer ptr2)
{
  GUnixMountEntry *mount_entry1 = (GUnixMountEntry *) ptr1;
  GUnixMountEntry *mount_entry2 = (GUnixMountEntry *) ptr2;
  return g_unix_mount_entry_compare (mount_entry1, mount_entry2) == 0;
}

static void
update_mounts (GVfsUDisks2VolumeMonitor  *monitor,
               GList                    **added_mounts,
               GList                    **removed_mounts,
               gboolean                   coldplug)
{
  GHashTable *cur_mounts; /* GUnixMountEntry * ~> GVfsUDisks2Mount * */
  GHashTableIter iter;
  gpointer value = NULL;
  GList *new_mounts, *l;
  GList *unchanged = NULL; /* GVfsUDisks2Mount * */
  GVfsUDisks2Mount *mount;
  GVfsUDisks2Volume *volume;

  cur_mounts = g_hash_table_new_full (gvfs_mount_entry_hash, gvfs_mount_entry_equal, NULL, g_object_unref);

  g_hash_table_iter_init (&iter, monitor->mounts);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      GUnixMountEntry *mount_entry;

      mount = GVFS_UDISKS2_MOUNT (value);
      mount_entry = gvfs_udisks2_mount_get_mount_entry (mount);
      if (mount_entry != NULL)
        g_hash_table_insert (cur_mounts, mount_entry, g_object_ref (mount));
    }

  new_mounts = g_unix_mount_entries_get (NULL);
  /* skip mounts we want to ignore - we do it here so we get to reevaluate
   * on the next update whether they should still be ignored
   */
  for (l = new_mounts; l != NULL; l = g_list_next (l))
    {
      GUnixMountEntry *mount_entry = l->data;
      if (should_include_mount (monitor, mount_entry))
        {
          mount = g_hash_table_lookup (cur_mounts, mount_entry);
          if (mount != NULL)
            {
              unchanged = g_list_prepend (unchanged, g_object_ref (mount));
              g_hash_table_remove (cur_mounts, mount_entry);
            }
          else
            {
              const gchar *root = g_unix_mount_entry_get_root_path (mount_entry);
              const gchar *device_path;

              /* since @mount takes ownership of @mount_entry, create a copy to not free it below */
              mount_entry = g_unix_mount_entry_copy (mount_entry);
              device_path = g_unix_mount_entry_get_device_path (mount_entry);
              volume = NULL;
              if (root == NULL || g_strcmp0 (root, "/") == 0)
                volume = find_volume_for_device (monitor, device_path);
              if (volume == NULL)
                volume = find_fstab_volume_for_mount_entry (monitor, mount_entry);
              mount = gvfs_udisks2_mount_new (monitor, mount_entry, volume); /* adopts mount_entry */
              if (mount != NULL)
                {
                  g_hash_table_insert (monitor->mounts, g_strdup (gvfs_udisks2_mount_get_mount_path (mount)), g_object_ref (mount));
                  *added_mounts = g_list_prepend (*added_mounts, g_steal_pointer (&mount));
                }
            }
        }
    }

  g_hash_table_iter_init (&iter, cur_mounts);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      mount = value;

      gvfs_udisks2_mount_unmounted (mount);
      *removed_mounts = g_list_prepend (*removed_mounts, g_object_ref (mount));

      if (g_hash_table_lookup (monitor->mounts, gvfs_udisks2_mount_get_mount_path (mount)) == mount)
        g_hash_table_remove (monitor->mounts, gvfs_udisks2_mount_get_mount_path (mount));
    }

  /* Handle the case where the volume containing the mount appears *after*
   * the mount.
   *
   * This can happen when unlocking+mounting a LUKS device and the two
   * operations are *right* after each other. In that case we get the
   * event from GUnixMountMonitor (which monitors /proc/mounts) before
   * the event from udisks.
   */
  for (l = unchanged; l != NULL; l = l->next)
    {
      mount = l->data;

      if (gvfs_udisks2_mount_get_volume (mount) == NULL)
        {
          GUnixMountEntry *mount_entry = gvfs_udisks2_mount_get_mount_entry (mount);
          const gchar *root = g_unix_mount_entry_get_root_path (mount_entry);
          const gchar *device_path;

          device_path = g_unix_mount_entry_get_device_path (mount_entry);
          volume = NULL;
          if (root == NULL || g_strcmp0 (root, "/") == 0)
            volume = find_volume_for_device (monitor, device_path);
          if (volume == NULL)
            volume = find_fstab_volume_for_mount_entry (monitor, mount_entry);
          if (volume != NULL)
            gvfs_udisks2_mount_set_volume (mount, volume);
        }
    }

  g_list_free_full (unchanged, g_object_unref);
  g_list_free_full (new_mounts, (GDestroyNotify) g_unix_mount_entry_free);

  g_hash_table_unref (cur_mounts);
}

/* ---------------------------------------------------------------------------------------------------- */

#if defined(HAVE_BURN) || defined(HAVE_CDDA)
static void
update_discs (GVfsUDisks2VolumeMonitor  *monitor,
              GList                    **added_volumes,
              GList                    **removed_volumes,
              GList                    **added_mounts,
              GList                    **removed_mounts,
              gboolean                   coldplug)
{
  GList *objects, *l;
  GHashTable *cur_disc_block_volumes; /* UDisksBlock * ~> GVfsUDisks2Volume * */
  GHashTableIter iter;
  gpointer key = NULL, value = NULL;
  GVfsUDisks2Volume *volume;
  GVfsUDisks2Mount *mount;

  /* we also need to generate GVolume + GMount objects for
   *
   * - optical discs with audio
   * - optical discs that are blank
   *
   */

  objects = g_dbus_object_manager_get_objects (udisks_client_get_object_manager (monitor->client));

  cur_disc_block_volumes = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_object_unref);

  g_hash_table_iter_init (&iter, monitor->disc_volumes);
  while (g_hash_table_iter_next (&iter, &key, NULL))
    {
      volume = key;
      g_hash_table_insert (cur_disc_block_volumes, gvfs_udisks2_volume_get_block (volume), g_object_ref (volume));
    }

  for (l = objects; l != NULL; l = l->next)
    {
      UDisksDrive *udisks_drive = udisks_object_peek_drive (UDISKS_OBJECT (l->data));
      UDisksBlock *block;

      if (udisks_drive == NULL)
        continue;

      if (!should_include_drive (monitor, udisks_drive))
        continue;

      if (!should_include_disc (monitor, udisks_drive))
        continue;

      block = udisks_client_get_block_for_drive (monitor->client, udisks_drive, FALSE);
      if (block != NULL)
        {
          /* not in currently known volumes => add it */
          if (!g_hash_table_remove (cur_disc_block_volumes, block))
            {
              UDisksDrive *udisks_drive;

              udisks_drive = udisks_client_get_drive_for_block (monitor->client, block);
              if (udisks_drive != NULL)
                {
                  gchar *uri = NULL;
                  GFile *activation_root;

#ifdef HAVE_BURN
                  if (udisks_drive_get_optical_blank (udisks_drive))
                    {
                      uri = g_strdup ("burn://");
                    }
#endif

#ifdef HAVE_CDDA
                  if (udisks_drive_get_optical_num_audio_tracks (udisks_drive) > 0)
                    {
                      gchar *basename = g_path_get_basename (udisks_block_get_device (block));
                      uri = g_strdup_printf ("cdda://%s", basename);
                      g_free (basename);
                    }
#endif

                  activation_root = g_file_new_for_uri (uri);
                  volume = gvfs_udisks2_volume_new (monitor,
                                                    block,
                                                    NULL, /* mount_point */
                                                    find_drive_for_udisks_drive (monitor, udisks_drive),
                                                    activation_root,
                                                    coldplug);
                  if (volume != NULL)
                    {
#ifdef HAVE_BURN
                      if (udisks_drive_get_optical_blank (udisks_drive))
                        {
                          mount = gvfs_udisks2_mount_new (monitor,
                                                          NULL, /* GUnixMountEntry */
                                                          volume);
                          if (mount != NULL)
                            {
                              g_hash_table_add (monitor->disc_mounts, g_object_ref (mount));
                              *added_mounts = g_list_prepend (*added_mounts, g_steal_pointer (&mount));
                            }
                        }
#endif
                      add_disc_volume (monitor, volume);
                      *added_volumes = g_list_prepend (*added_volumes, g_steal_pointer (&volume));
                    }

                  g_object_unref (activation_root);
                  g_free (uri);
                  g_object_unref (udisks_drive);
                }
            }
          g_object_unref (block);
        }
    }

  /* which left are removed */
  g_hash_table_iter_init (&iter, cur_disc_block_volumes);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      volume = value;

      mount = NULL;
      if (volume != NULL)
        mount = GVFS_UDISKS2_MOUNT (g_volume_get_mount (G_VOLUME (volume)));

      if (mount != NULL)
        {
          gvfs_udisks2_mount_unmounted (mount);
          *removed_mounts = g_list_prepend (*removed_mounts, g_object_ref (mount));
          g_hash_table_remove (monitor->disc_mounts, mount);
        }
      if (volume != NULL)
        {
          gvfs_udisks2_volume_removed (volume);
          *removed_volumes = g_list_prepend (*removed_volumes, g_object_ref (volume));
          remove_disc_volume (monitor, volume);
        }
    }

  g_hash_table_unref (cur_disc_block_volumes);

  g_list_free_full (objects, g_object_unref);
}
#endif

/* ---------------------------------------------------------------------------------------------------- */
