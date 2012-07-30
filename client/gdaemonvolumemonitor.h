/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
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
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#ifndef __G_DAEMON_VOLUME_MONITOR_H__
#define __G_DAEMON_VOLUME_MONITOR_H__

#include <glib-object.h>
#include <gio/gio.h>
#include "gmounttracker.h"

G_BEGIN_DECLS

#define G_TYPE_DAEMON_VOLUME_MONITOR        (g_daemon_volume_monitor_get_type ())
#define G_DAEMON_VOLUME_MONITOR(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DAEMON_VOLUME_MONITOR, GDaemonVolumeMonitor))
#define G_DAEMON_VOLUME_MONITOR_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_DAEMON_VOLUME_MONITOR, GDaemonVolumeMonitorClass))
#define G_IS_DAEMON_VOLUME_MONITOR(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DAEMON_VOLUME_MONITOR))
#define G_IS_DAEMON_VOLUME_MONITOR_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DAEMON_VOLUME_MONITOR))

typedef struct _GDaemonVolumeMonitor GDaemonVolumeMonitor;
typedef struct _GDaemonVolumeMonitorClass GDaemonVolumeMonitorClass;

/* Forward definitions */
typedef struct _GDaemonMount GDaemonMount;
typedef struct _GDaemonVolume GDaemonVolume;
typedef struct _GDaemonDrive GDaemonDrive;

struct _GDaemonVolumeMonitorClass {
  GVolumeMonitorClass parent_class;

};

GType g_daemon_volume_monitor_get_type (void) G_GNUC_CONST;
void g_daemon_volume_monitor_register_types (GTypeModule *type_module);

GVolumeMonitor *g_daemon_volume_monitor_new (void);

GDaemonMount *g_daemon_volume_monitor_find_mount_by_mount_info (GMountInfo *mount_info);

G_END_DECLS

#endif /* __G_DAEMON_VOLUME_MONITOR_H__ */
