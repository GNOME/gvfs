/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2012 Red Hat, Inc.
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
 * Author: Debarshi Ray <debarshir@gnome.org>
 */

#ifndef __GOA_VOLUME_MONITOR_H__
#define __GOA_VOLUME_MONITOR_H__

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_GOA_VOLUME_MONITOR (g_vfs_goa_volume_monitor_get_type())

#define G_VFS_GOA_VOLUME_MONITOR(o) \
  (G_TYPE_CHECK_INSTANCE_CAST((o), \
   G_VFS_TYPE_GOA_VOLUME_MONITOR, GVfsGoaVolumeMonitor))

#define G_VFS_GOA_VOLUME_MONITOR_CLASS(k) \
  (G_TYPE_CHECK_CLASS_CAST((k), \
   G_VFS_TYPE_GOA_VOLUME_MONITOR, GVfsGoaVolumeMonitorClass))

#define G_VFS_IS_GOA_VOLUME_MONITOR(o) \
  (G_TYPE_CHECK_INSTANCE_TYPE((o), \
   G_VFS_TYPE_GOA_VOLUME_MONITOR))

#define G_VFS_IS_GOA_VOLUME_MONITOR_CLASS(k) \
  (G_TYPE_CHECK_CLASS_TYPE((k), \
   G_VFS_TYPE_GOA_VOLUME_MONITOR))

#define G_VFS_GOA_VOLUME_MONITOR_GET_CLASS(o) \
  (G_TYPE_INSTANCE_GET_CLASS((o), \
   G_VFS_TYPE_GOA_VOLUME_MONITOR, GVfsGoaVolumeMonitorClass))

typedef struct _GVfsGoaVolumeMonitor GVfsGoaVolumeMonitor;
typedef struct _GVfsGoaVolumeMonitorClass GVfsGoaVolumeMonitorClass;

GType g_vfs_goa_volume_monitor_get_type (void) G_GNUC_CONST;

GVolumeMonitor *g_vfs_goa_volume_monitor_new (void);

G_END_DECLS

#endif /* __GOA_VOLUME_MONITOR_H__ */
