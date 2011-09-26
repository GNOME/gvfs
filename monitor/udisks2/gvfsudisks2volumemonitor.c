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

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "gvfsudisks2volumemonitor.h"

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
};

static UDisksClient *get_udisks_client_sync (GError **error);

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

  g_clear_object (&monitor->client);

  G_OBJECT_CLASS (gvfs_udisks2_volume_monitor_parent_class)->finalize (object);
}

static GList *
get_mounts (GVolumeMonitor *monitor)
{
  /* TODO */
  return NULL;
}

static GList *
get_volumes (GVolumeMonitor *monitor)
{
  /* TODO */
  return NULL;
}

static GList *
get_connected_drives (GVolumeMonitor *monitor)
{
  /* TODO */
  return NULL;
}

static GVolume *
get_volume_for_uuid (GVolumeMonitor *monitor,
                     const gchar    *uuid)
{
  /* TODO */
  return NULL;
}

static GMount *
get_mount_for_uuid (GVolumeMonitor *monitor,
                    const gchar    *uuid)
{
  /* TODO */
  return NULL;
}

static GMount *
get_mount_for_mount_path (const gchar  *mount_path,
                          GCancellable *cancellable)
{
  /* TODO */
  return NULL;
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

  /*g_warning ("creating gdu vm");*/

  /* Invoke parent constructor. */
  parent_class = G_OBJECT_CLASS (g_type_class_peek_parent (g_type_class_peek (GVFS_TYPE_UDISKS2_VOLUME_MONITOR)));
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
  monitor->client = get_udisks_client_sync (NULL);
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
