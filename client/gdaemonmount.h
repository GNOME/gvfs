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

#ifndef __G_DAEMON_MOUNT_H__
#define __G_DAEMON_MOUNT_H__

#include <glib-object.h>
#include <gio/gio.h>
#include "gdaemonvfs.h"
#include "gdaemonvolumemonitor.h"
#include "gmounttracker.h"

G_BEGIN_DECLS

#define G_TYPE_DAEMON_MOUNT        (g_daemon_mount_get_type ())
#define G_DAEMON_MOUNT(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DAEMON_MOUNT, GDaemonMount))
#define G_DAEMON_MOUNT_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_DAEMON_MOUNT, GDaemonMountClass))
#define G_IS_DAEMON_MOUNT(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DAEMON_MOUNT))
#define G_IS_DAEMON_MOUNT_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DAEMON_MOUNT))

typedef struct _GDaemonMountClass GDaemonMountClass;

struct _GDaemonMountClass {
   GObjectClass parent_class;
};

GType g_daemon_mount_get_type (void) G_GNUC_CONST;

GDaemonMount *g_daemon_mount_new            (GMountInfo     *mount_info,
                                             GVolumeMonitor *volume_monitor);

GMountInfo   *g_daemon_mount_get_mount_info (GDaemonMount *mount);

void          g_daemon_mount_set_foreign_volume (GDaemonMount *mount, GVolume *foreign_volume);

G_END_DECLS

#endif /* __G_DAEMON_MOUNT_H__ */
