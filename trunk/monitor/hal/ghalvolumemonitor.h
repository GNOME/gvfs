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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#ifndef __G_HAL_VOLUME_MONITOR_H__
#define __G_HAL_VOLUME_MONITOR_H__

#include <glib-object.h>
#include <gio/gio.h>
#include <gio/gunixmounts.h>

G_BEGIN_DECLS

#define G_TYPE_HAL_VOLUME_MONITOR        (g_hal_volume_monitor_get_type ())
#define G_HAL_VOLUME_MONITOR(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_HAL_VOLUME_MONITOR, GHalVolumeMonitor))
#define G_HAL_VOLUME_MONITOR_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_HAL_VOLUME_MONITOR, GHalVolumeMonitorClass))
#define G_IS_HAL_VOLUME_MONITOR(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_HAL_VOLUME_MONITOR))
#define G_IS_HAL_VOLUME_MONITOR_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_HAL_VOLUME_MONITOR))

typedef struct _GHalVolumeMonitor GHalVolumeMonitor;
typedef struct _GHalVolumeMonitorClass GHalVolumeMonitorClass;

/* Forward definitions */
typedef struct _GHalDrive GHalDrive;
typedef struct _GHalVolume GHalVolume;
typedef struct _GHalMount GHalMount;

struct _GHalVolumeMonitorClass {
  GNativeVolumeMonitorClass parent_class;

};

GType g_hal_volume_monitor_get_type (void) G_GNUC_CONST;

GVolumeMonitor *g_hal_volume_monitor_new                          (void);
void            g_hal_volume_monitor_force_update                 (GHalVolumeMonitor *monitor,
                                                                   gboolean emit_in_idle);

G_END_DECLS

#endif /* __G_HAL_VOLUME_MONITOR_H__ */
