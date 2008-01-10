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

#ifndef __G_VFS_JOB_CREATE_MONITOR_H__
#define __G_VFS_JOB_CREATE_MONITOR_H__

#include <gio/gio.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>
#include <gvfsmonitor.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_CREATE_MONITOR         (g_vfs_job_create_monitor_get_type ())
#define G_VFS_JOB_CREATE_MONITOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_CREATE_MONITOR, GVfsJobCreateMonitor))
#define G_VFS_JOB_CREATE_MONITOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_CREATE_MONITOR, GVfsJobCreateMonitorClass))
#define G_VFS_IS_JOB_CREATE_MONITOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_CREATE_MONITOR))
#define G_VFS_IS_JOB_CREATE_MONITOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_CREATE_MONITOR))
#define G_VFS_JOB_CREATE_MONITOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_CREATE_MONITOR, GVfsJobCreateMonitorClass))

typedef struct _GVfsJobCreateMonitorClass   GVfsJobCreateMonitorClass;

struct _GVfsJobCreateMonitor
{
  GVfsJobDBus parent_instance;

  gboolean is_directory;
  GVfsBackend *backend;
  char *filename;
  GFileMonitorFlags flags;

  GVfsMonitor *monitor;
};

struct _GVfsJobCreateMonitorClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_create_monitor_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_create_monitor_new         (DBusConnection       *connection,
					       DBusMessage          *message,
					       GVfsBackend          *backend,
					       gboolean              is_directory);
void     g_vfs_job_create_monitor_set_monitor (GVfsJobCreateMonitor *job,
					       GVfsMonitor          *monitor);

G_END_DECLS

#endif /* __G_VFS_JOB_CREATE_MONITOR_H__ */
