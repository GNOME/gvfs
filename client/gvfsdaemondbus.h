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

#ifndef __G_VFS_DAEMON_DBUS_H__
#define __G_VFS_DAEMON_DBUS_H__

#include <glib.h>
#include <dbus/dbus.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* Used for internal errors */
GQuark  _g_vfs_error_quark (void);
#define G_VFS_ERROR _g_vfs_error_quark()

typedef enum
{
  G_VFS_ERROR_RETRY
} GVfsError;


typedef void (*GVfsAsyncDBusCallback) (DBusMessage *reply,
				       DBusConnection *connection,
				       GError *io_error,
				       gpointer callback_data);
typedef void (*GetFdAsyncCallback)    (int fd,
				       gpointer callback_data);

void            _g_dbus_register_vfs_filter             (const char                     *obj_path,
							 DBusHandleMessageFunction       callback,
							 GObject                        *data);
void            _g_dbus_unregister_vfs_filter           (const char                     *obj_path);
GList *         _g_dbus_bus_list_names_with_prefix_sync (DBusConnection                 *connection,
							 const char                     *prefix,
							 DBusError                      *error);
DBusConnection *_g_dbus_connection_get_sync             (const char                     *dbus_id,
							 GError                        **error);
int             _g_dbus_connection_get_fd_sync          (DBusConnection                 *conn,
							 int                             fd_id);
void            _g_dbus_connection_get_fd_async         (DBusConnection                 *connection,
							 int                             fd_id,
							 GetFdAsyncCallback              callback,
							 gpointer                        callback_data);
void            _g_vfs_daemon_call_async                (DBusMessage                    *message,
							 GVfsAsyncDBusCallback           callback,
							 gpointer                        callback_data,
							 GCancellable                   *cancellable);
DBusMessage *   _g_vfs_daemon_call_sync                 (DBusMessage                    *message,
							 DBusConnection                **connection_out,
							 const char                     *callback_obj_path,
							 DBusObjectPathMessageFunction   callback,
							 gpointer                        callback_user_data,
							 GCancellable                   *cancellable,
							 GError                        **error);
GFileInfo *     _g_dbus_get_file_info                   (DBusMessageIter                *iter,
							 GError                        **error);

void        _g_simple_async_result_complete_with_cancellable
                                                        (GSimpleAsyncResult             *result,
                                                         GCancellable                   *cancellable);

G_END_DECLS

#endif /* __G_VFS_DAEMON_DBUS_H__ */
