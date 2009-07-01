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

#ifndef __G_VFS_DAEMON_H__
#define __G_VFS_DAEMON_H__

#include <glib-object.h>
#include <gvfsjobsource.h>
#include <gmountsource.h>
#include <dbus/dbus.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_DAEMON         (g_vfs_daemon_get_type ())
#define G_VFS_DAEMON(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_DAEMON, GVfsDaemon))
#define G_VFS_DAEMON_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_DAEMON, GVfsDaemonClass))
#define G_VFS_IS_DAEMON(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_DAEMON))
#define G_VFS_IS_DAEMON_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_DAEMON))
#define G_VFS_DAEMON_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_DAEMON, GVfsDaemonClass))

typedef struct _GVfsDaemon        GVfsDaemon;
typedef struct _GVfsDaemonClass   GVfsDaemonClass;
typedef struct _GVfsDaemonPrivate GVfsDaemonPrivate;

struct _GVfsDaemonClass
{
  GObjectClass parent_class;

  /* vtable */
  
};

GType g_vfs_daemon_get_type (void) G_GNUC_CONST;

GVfsDaemon *g_vfs_daemon_new             (gboolean                       main_daemon,
					  gboolean                       replace);
void        g_vfs_daemon_set_max_threads (GVfsDaemon                    *daemon,
					  gint                           max_threads);
void        g_vfs_daemon_add_job_source  (GVfsDaemon                    *daemon,
					  GVfsJobSource                 *job_source);
void        g_vfs_daemon_queue_job       (GVfsDaemon                    *daemon,
					  GVfsJob                       *job);
void        g_vfs_daemon_register_path   (GVfsDaemon                    *daemon,
					  const char                    *obj_path,
					  DBusObjectPathMessageFunction  callback,
					  gpointer                       user_data);
void        g_vfs_daemon_unregister_path (GVfsDaemon                    *daemon,
					  const char                    *obj_path);
void        g_vfs_daemon_initiate_mount  (GVfsDaemon                    *daemon,
					  GMountSpec                    *mount_spec,
					  GMountSource                  *mount_source,
					  gboolean                       is_automount,
					  DBusMessage                   *request);
GArray     *g_vfs_daemon_get_blocking_processes (GVfsDaemon             *daemon);

G_END_DECLS

#endif /* __G_VFS_DAEMON_H__ */
