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

#ifndef __G_DAEMON_DIRECTORY_MONITOR_H__
#define __G_DAEMON_DIRECTORY_MONITOR_H__

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define G_TYPE_DAEMON_DIRECTORY_MONITOR		(g_daemon_directory_monitor_get_type ())
#define G_DAEMON_DIRECTORY_MONITOR(o)		(G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DAEMON_DIRECTORY_MONITOR, GDaemonDirectoryMonitor))
#define G_DAEMON_DIRECTORY_MONITOR_CLASS(k)	(G_TYPE_CHECK_CLASS_CAST ((k), G_TYPE_DAEMON_DIRECTORY_MONITOR, GDaemonDirectoryMonitorClass))
#define G_IS_DAEMON_DIRECTORY_MONITOR(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DAEMON_DIRECTORY_MONITOR))
#define G_IS_DAEMON_DIRECTORY_MONITOR_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DAEMON_DIRECTORY_MONITOR))

typedef struct _GDaemonDirectoryMonitor      GDaemonDirectoryMonitor;
typedef struct _GDaemonDirectoryMonitorClass GDaemonDirectoryMonitorClass;

struct _GDaemonDirectoryMonitorClass {
  GDirectoryMonitorClass parent_class;
};

GType g_daemon_directory_monitor_get_type (void) G_GNUC_CONST;

GDirectoryMonitor* g_daemon_directory_monitor_new (const char *remote_id,
						   const char *remote_obj_path);


G_END_DECLS

#endif /* __G_DAEMON_DIRECTORY_MONITOR_H__ */
