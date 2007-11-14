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

#ifndef __G_DAEMON_VFS_H__
#define __G_DAEMON_VFS_H__

#include <gio/gvfs.h>
#include <dbus/dbus.h>
#include "gmountspec.h"
#include "gmounttracker.h"
#include "gvfsuriutils.h"

G_BEGIN_DECLS

typedef struct _GDaemonVfs       GDaemonVfs;
typedef struct _GDaemonVfsClass  GDaemonVfsClass;

typedef void (*GMountInfoLookupCallback) (GMountInfo *mount_info,
					  gpointer data,
					  GError *error);

GType   g_daemon_vfs_get_type  (void);

GDaemonVfs *g_daemon_vfs_new (void);

char *          _g_daemon_vfs_get_uri_for_mountspec    (GMountSpec               *spec,
							char                     *path,
							gboolean                  allow_utf8);
const char *    _g_daemon_vfs_mountspec_get_uri_scheme (GMountSpec               *spec);
void            _g_daemon_vfs_get_mount_info_async     (GMountSpec               *spec,
							const char               *path,
							GMountInfoLookupCallback  callback,
							gpointer                  user_data);
GMountInfo *    _g_daemon_vfs_get_mount_info_sync      (GMountSpec               *spec,
							const char               *path,
							GError                  **error);
void            _g_daemon_vfs_invalidate_dbus_id       (const char               *dbus_id);
DBusConnection *_g_daemon_vfs_get_async_bus            (void);



G_END_DECLS

#endif /* __G_DAEMON_VFS_H__ */
