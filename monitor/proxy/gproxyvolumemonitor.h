/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2008 Red Hat, Inc.
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

#ifndef __G_PROXY_VOLUME_MONITOR_H__
#define __G_PROXY_VOLUME_MONITOR_H__

#include <glib-object.h>
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <dbus/dbus.h>

G_BEGIN_DECLS

#define G_TYPE_PROXY_VOLUME_MONITOR         (g_proxy_volume_monitor_get_type ())
#define G_PROXY_VOLUME_MONITOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_PROXY_VOLUME_MONITOR, GProxyVolumeMonitor))
#define G_PROXY_VOLUME_MONITOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_PROXY_VOLUME_MONITOR, GProxyVolumeMonitorClass))
#define G_PROXY_VOLUME_MONITOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_PROXY_VOLUME_MONITOR, GProxyVolumeMonitorClass))
#define G_IS_PROXY_VOLUME_MONITOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_PROXY_VOLUME_MONITOR))
#define G_IS_PROXY_VOLUME_MONITOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_PROXY_VOLUME_MONITOR))

typedef struct _GProxyVolumeMonitor GProxyVolumeMonitor;
typedef struct _GProxyVolumeMonitorClass GProxyVolumeMonitorClass;

/* Timeout used for D-Bus messages in msec - this needs to be high enough
 * to ensure that the user has time to interact with e.g. mount operation
 * dialogs.
 *
 * We use 30 minutes.
 */
#define G_PROXY_VOLUME_MONITOR_DBUS_TIMEOUT 30*60*1000

/* Forward definitions */
typedef struct _GProxyDrive GProxyDrive;
typedef struct _GProxyVolume GProxyVolume;
typedef struct _GProxyMount GProxyMount;
typedef struct _GProxyShadowMount GProxyShadowMount;

struct _GProxyVolumeMonitorClass {
  GNativeVolumeMonitorClass parent_class;
  char *dbus_name;
  gboolean is_native;
  int is_supported_nr;
};

GType g_proxy_volume_monitor_get_type (void) G_GNUC_CONST;

void            g_proxy_volume_monitor_register          (GIOModule           *module);
GProxyDrive    *g_proxy_volume_monitor_get_drive_for_id  (GProxyVolumeMonitor *volume_monitor,
                                                          const char          *id);
GProxyVolume   *g_proxy_volume_monitor_get_volume_for_id (GProxyVolumeMonitor *volume_monitor,
                                                          const char          *id);
GProxyMount    *g_proxy_volume_monitor_get_mount_for_id  (GProxyVolumeMonitor *volume_monitor,
                                                          const char          *id);
DBusConnection *g_proxy_volume_monitor_get_dbus_connection (GProxyVolumeMonitor *volume_monitor);
const char     *g_proxy_volume_monitor_get_dbus_name       (GProxyVolumeMonitor *volume_monitor);

gboolean g_proxy_volume_monitor_setup_session_bus_connection (gboolean need_integration);
void g_proxy_volume_monitor_teardown_session_bus_connection (void);


GHashTable *_get_identifiers (DBusMessageIter *iter);

G_END_DECLS

#endif /* __G_PROXY_VOLUME_MONITOR_H__ */
