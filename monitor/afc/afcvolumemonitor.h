/*
 * gvfs/monitor/afc/afc-volume-monitor.h
 *
 * Copyright (c) 2008 Patrick Walton <pcwalton@ucla.edu>
 */

#ifndef AFC_VOLUME_MONITOR_H
#define AFC_VOLUME_MONITOR_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_AFC_VOLUME_MONITOR   (g_vfs_afc_volume_monitor_get_type())
#define G_VFS_AFC_VOLUME_MONITOR(o) (G_TYPE_CHECK_INSTANCE_CAST((o), G_VFS_TYPE_AFC_VOLUME_MONITOR, GVfsAfcVolumeMonitor))
#define G_VFS_AFC_VOLUME_MONITOR_CLASS(k) (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_AFC_VOLUME_MONITOR, GVfsAfcVolumeMonitorClass))
#define G_VFS_IS_AFC_VOLUME_MONITOR(o) (G_TYPE_CHECK_INSTANCE_TYPE((o), G_VFS_TYPE_AFC_VOLUME_MONITOR))
#define G_VFS_IS_AFC_VOLUME_MONITOR_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE((k), G_VFS_TYPE_AFC_VOLUME_MONITOR))
#define G_VFS_AFC_VOLUME_MONITOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS((o), G_VFS_TYPE_AFC_VOLUME_MONITOR, GVfsAfcVolumeMonitorClass))

typedef struct _GVfsAfcVolumeMonitor GVfsAfcVolumeMonitor;
typedef struct _GVfsAfcVolumeMonitorClass GVfsAfcVolumeMonitorClass;

struct _GVfsAfcVolumeMonitorClass {
  GVolumeMonitorClass parent_class;
};

GType g_vfs_afc_volume_monitor_get_type (void) G_GNUC_CONST;

GVolumeMonitor *g_vfs_afc_volume_monitor_new (void);

G_END_DECLS

#endif /* AFC_VOLUME_MONITOR_H */

/*
 * vim: sw=2 ts=8 cindent expandtab cinoptions=f0,>4,n2,{2,(0,^-2,t0 ai
 */
