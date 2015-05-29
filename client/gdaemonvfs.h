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

#ifndef __G_DAEMON_VFS_H__
#define __G_DAEMON_VFS_H__

#include <gio/gio.h>
#include "gmountspec.h"
#include "gmounttracker.h"
#include "gvfsuriutils.h"
#include <metatree.h>

G_BEGIN_DECLS

#define G_TYPE_DAEMON_VFS		(g_daemon_vfs_get_type ())
#define G_DAEMON_VFS(obj)		(G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_DAEMON_VFS, GDaemonVfs))
#define G_DAEMON_VFS_CLASS(klass)	(G_TYPE_CHECK_CLASS_CAST ((klass), G_TYPE_DAEMON_VFS, GDaemonVfsClass))
#define G_IS_DAEMON_VFS(obj)		(G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_DAEMON_VFS))
#define G_IS_DAEMON_VFS_CLASS(klass)	(G_TYPE_CHECK_CLASS_TYPE ((klass), G_TYPE_DAEMON_VFS))
#define G_DAEMON_VFS_GET_CLASS(obj)	(G_TYPE_INSTANCE_GET_CLASS ((obj), G_TYPE_DAEMON_VFS, GDaemonVfsClass))

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
							GCancellable             *cancellable,
							GError                  **error);
GMountInfo *    _g_daemon_vfs_get_mount_info_by_fuse_sync (const char *fuse_path,
							   char **mount_path);
GMountSpec *    _g_daemon_vfs_get_mount_spec_for_path  (GMountSpec               *spec,
						        const char               *path,
						        const char               *new_path);
void            _g_daemon_vfs_invalidate               (const char               *dbus_id,
                                                        const char               *object_path);
GDBusConnection *_g_daemon_vfs_get_async_bus           (void);
int             _g_daemon_vfs_append_metadata_for_set  (GVariantBuilder *builder,
							MetaTree *tree,
							const char *path,
							const char *attribute,
							GFileAttributeType type,
							gpointer   value);

G_END_DECLS

#endif /* __G_DAEMON_VFS_H__ */
