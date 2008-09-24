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
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#ifndef __G_VFS_MONITOR_H__
#define __G_VFS_MONITOR_H__

#include <glib-object.h>
#include <gio/gio.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_MONITOR		(g_vfs_monitor_get_type ())
#define G_VFS_MONITOR(o)		(G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_MONITOR, GVfsMonitor))
#define G_VFS_MONITOR_CLASS(k)		(G_TYPE_CHECK_CLASS_CAST ((k), G_TYPE_VFS_MONITOR, GVfsMonitorClass))
#define G_IS_VFS_MONITOR(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_MONITOR))
#define G_IS_VFS_MONITOR_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_MONITOR))

typedef struct _GVfsMonitor        GVfsMonitor;
typedef struct _GVfsMonitorClass   GVfsMonitorClass;
typedef struct _GVfsMonitorPrivate GVfsMonitorPrivate;

struct _GVfsMonitor 
{
  GObject parent_instance;

  GVfsMonitorPrivate *priv;
};
  

struct _GVfsMonitorClass
{
  GObjectClass parent_class;
  
};

GType g_vfs_monitor_get_type (void) G_GNUC_CONST;

GVfsMonitor* g_vfs_monitor_new             (GVfsBackend       *backend);
const char * g_vfs_monitor_get_object_path (GVfsMonitor       *monitor);
void         g_vfs_monitor_emit_event      (GVfsMonitor       *monitor,
					    GFileMonitorEvent  event_type,
					    const char        *file_path,
					    const char        *other_file_path);

G_END_DECLS

#endif /* __G_VFS_MONITOR_H__ */
