/*
 * gvfs/monitor/afc/afc-volume.h
 *
 * Copyright (c) 2008 Patrick Walton <pcwalton@cs.ucla.edu>
 */

#ifndef GVFS_MONITOR_AFC_AFC_VOLUME_H
#define GVFS_MONITOR_AFC_AFC_VOLUME_H

#include <glib-object.h>
#include <gio/gio.h>

#include "afcvolumemonitor.h"

G_BEGIN_DECLS

#define G_VFS_TYPE_AFC_VOLUME   (g_vfs_afc_volume_get_type())
#define G_VFS_AFC_VOLUME(o) (G_TYPE_CHECK_INSTANCE_CAST((o), G_VFS_TYPE_AFC_VOLUME, GVfsAfcVolume))
#define G_VFS_AFC_VOLUME_CLASS(k) (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_AFC_VOLUME, GVfsAfcVolumeClass))
#define G_VFS_IS_AFC_VOLUME(o) (G_TYPE_CHECK_INSTANCE_TYPE((o), G_VFS_TYPE_AFC_VOLUME))
#define G_VFS_IS_AFC_VOLUME_CLASS(k) ((G_TYPE_CHECK_CLASS_TYPE((k), G_VFS_TYPE_AFC_VOLUME))
#define G_VFS_AFC_VOLUME_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS((o), G_VFS_TYPE_AFC_VOLUME, GVfsAfcVolumeClass))

typedef struct _GVfsAfcVolume GVfsAfcVolume;
typedef struct _GVfsAfcVolumeClass GVfsAfcVolumeClass;

struct _GVfsAfcVolumeClass {
  GObjectClass parent_class;
};

GType g_vfs_afc_volume_get_type (void) G_GNUC_CONST;

GVfsAfcVolume *g_vfs_afc_volume_new (GVolumeMonitor *monitor,
                                     const char     *uuid);

gboolean g_vfs_afc_volume_has_uuid (GVfsAfcVolume *volume, const char *uuid);

G_END_DECLS

#endif /* GVFS_MONITOR_AFC_AFC_VOLUME_H */

/*
 * vim: sw=2 ts=8 cindent expandtab cinoptions=f0,>4,n2,{2,(0,^-2,t0 ai
 */
