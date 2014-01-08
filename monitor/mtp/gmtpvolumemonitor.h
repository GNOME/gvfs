/* GIO - GLib Input, Output and Streaming Library
 *   Volume Monitor for MTP Backend
 *
 * Copyright (C) 2012 Philip Langdale <philipl@overt.org>
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
 * Public License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef __G_MTP_VOLUME_MONITOR_H__
#define __G_MTP_VOLUME_MONITOR_H__

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define G_TYPE_MTP_VOLUME_MONITOR        (g_mtp_volume_monitor_get_type ())
#define G_MTP_VOLUME_MONITOR(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_MTP_VOLUME_MONITOR, GMtpVolumeMonitor))
#define G_MTP_VOLUME_MONITOR_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_MTP_VOLUME_MONITOR, GMtpVolumeMonitorClass))
#define G_IS_MTP_VOLUME_MONITOR(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_MTP_VOLUME_MONITOR))
#define G_IS_MTP_VOLUME_MONITOR_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_MTP_VOLUME_MONITOR))

typedef struct _GMtpVolumeMonitor GMtpVolumeMonitor;
typedef struct _GMtpVolumeMonitorClass GMtpVolumeMonitorClass;

/* Forward definitions */
typedef struct _GMtpVolume GMtpVolume;

struct _GMtpVolumeMonitorClass {
  GVolumeMonitorClass parent_class;
};

GType g_mtp_volume_monitor_get_type (void) G_GNUC_CONST;

GVolumeMonitor *g_mtp_volume_monitor_new          (void);
void            g_mtp_volume_monitor_force_update (GMtpVolumeMonitor *monitor);

G_END_DECLS

#endif /* __G_MTP_VOLUME_MONITOR_H__ */
