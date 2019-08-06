/*
 * gvfs/monitor/afc/afc-volume-monitor.c
 *
 * Copyright (c) 2008 Patrick Walton <pcwalton@ucla.edu>
 */

#include <config.h>
#include <glib.h>
#include <gvfsproxyvolumemonitordaemon.h>
#include <stdio.h>
#include <gio/gio.h>
#include <libimobiledevice/libimobiledevice.h>
#include "afcvolume.h"
#include "afcvolumemonitor.h"

struct _GVfsAfcVolumeMonitor {
  GNativeVolumeMonitor parent;
  GList *volumes;
};

G_DEFINE_TYPE(GVfsAfcVolumeMonitor, g_vfs_afc_volume_monitor, G_TYPE_VOLUME_MONITOR)

static void
g_vfs_afc_monitor_create_volume (GVfsAfcVolumeMonitor *self,
                                 const char *uuid)
{
  GVfsAfcVolume *volume = NULL;

  g_debug ("creating volume for device uuid '%s'\n", uuid);

  /* Only add the house arrest volume, the default AFC service
   * doesn't contain anything that users should modify */
  volume = g_vfs_afc_volume_new (G_VOLUME_MONITOR (self), uuid, HOUSE_ARREST_SERVICE_PORT);
  if (volume != NULL)
    {
      self->volumes = g_list_prepend (self->volumes, volume);
      g_signal_emit_by_name (self, "volume-added", volume);
    }
}

static GVfsAfcVolume *
find_volume_by_uuid (GVfsAfcVolumeMonitor *self,
                     const char * uuid)
{
  GList *l;

  for (l = self->volumes; l != NULL; l = l->next)
    {
      GVfsAfcVolume *volume = l->data;
      if (volume && g_vfs_afc_volume_has_uuid (volume, uuid))
        return volume;
    }

  return NULL;
}

static void
g_vfs_afc_monitor_remove_volume (GVfsAfcVolumeMonitor *self,
                                 const char *uuid)
{
  GVfsAfcVolume *volume = NULL;

  volume = find_volume_by_uuid (self, uuid);
  while (volume != NULL)
    {
      g_debug ("removing volume for device uuid '%s'\n", uuid);
      self->volumes = g_list_remove (self->volumes, volume);
      g_signal_emit_by_name (self, "volume-removed", volume);
      g_object_unref (volume);

      volume = find_volume_by_uuid (self, uuid);
    }
}

static void
g_vfs_afc_monitor_idevice_event (const idevice_event_t *event, void *user_data)
{
  GVfsAfcVolumeMonitor *self;
  const gchar *event_udid;

  g_return_if_fail (event != NULL);

  self = G_VFS_AFC_VOLUME_MONITOR(user_data);

  event_udid = event->udid;
  if (event->event == IDEVICE_DEVICE_ADD)
    g_vfs_afc_monitor_create_volume (self, event_udid);
  else
    g_vfs_afc_monitor_remove_volume (self, event_udid);
}

static GObject *
g_vfs_afc_volume_monitor_constructor (GType type, guint ncps,
                                      GObjectConstructParam *cps)
{
  GVfsAfcVolumeMonitor *self;

  /* Boilerplate code to chain from parent. */
  self = G_VFS_AFC_VOLUME_MONITOR((*G_OBJECT_CLASS(g_vfs_afc_volume_monitor_parent_class)->constructor)(type, ncps, cps));

  self->volumes = NULL;

  idevice_event_subscribe(g_vfs_afc_monitor_idevice_event, self);

  g_debug ("Volume monitor alive\n");

  return G_OBJECT(self);
}

static void
g_vfs_afc_volume_monitor_finalize (GObject *_self)
{
  GVfsAfcVolumeMonitor *self;

  self = G_VFS_AFC_VOLUME_MONITOR(_self);

  if (self->volumes)
    g_list_free_full (self->volumes, g_object_unref);

  idevice_event_unsubscribe();

  if (G_OBJECT_CLASS(g_vfs_afc_volume_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS(g_vfs_afc_volume_monitor_parent_class)->finalize)( G_OBJECT(self));
}

static GList *
g_vfs_afc_volume_monitor_get_mounts (GVolumeMonitor *_self)
{
  return NULL;
}

static GList *
g_vfs_afc_volume_monitor_get_volumes (GVolumeMonitor *_self)
{
  GVfsAfcVolumeMonitor *self;
  GList *l;

  self = G_VFS_AFC_VOLUME_MONITOR (_self);

  l = g_list_copy (self->volumes);
  g_list_foreach (l, (GFunc)g_object_ref, NULL);

  return l;
}

static GList *
g_vfs_afc_volume_monitor_get_connected_drives (GVolumeMonitor *_self)
{
  return NULL;
}

static gboolean
g_vfs_afc_volume_monitor_is_supported (void)
{
  return TRUE;
}

static void
g_vfs_afc_volume_monitor_class_init (GVfsAfcVolumeMonitorClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GVolumeMonitorClass *monitor_class = G_VOLUME_MONITOR_CLASS(klass);

  gobject_class->constructor = g_vfs_afc_volume_monitor_constructor;
  gobject_class->finalize = g_vfs_afc_volume_monitor_finalize;

  monitor_class->get_mounts = g_vfs_afc_volume_monitor_get_mounts;
  monitor_class->get_volumes = g_vfs_afc_volume_monitor_get_volumes;
  monitor_class->get_connected_drives = g_vfs_afc_volume_monitor_get_connected_drives;
  monitor_class->is_supported = g_vfs_afc_volume_monitor_is_supported;
}

static void
g_vfs_afc_volume_monitor_init(GVfsAfcVolumeMonitor *self)
{
}

GVolumeMonitor *
g_vfs_afc_volume_monitor_new (void)
{
  return G_VOLUME_MONITOR(g_object_new (G_VFS_TYPE_AFC_VOLUME_MONITOR,
                                        NULL));
}

/*
 * vim: sw=2 ts=8 cindent expandtab cinoptions=f0,>4,n2,{2,(0,^-2,t0 ai
 */
