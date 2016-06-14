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

#ifndef __G_VFS_DAEMON_DBUS_H__
#define __G_VFS_DAEMON_DBUS_H__

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* Used for internal errors */
GQuark  _g_vfs_error_quark (void);
#define G_VFS_ERROR _g_vfs_error_quark()

typedef enum
{
  G_VFS_ERROR_RETRY
} GVfsError;


typedef void (*GVfsAsyncDBusCallback) (GDBusConnection *connection,
				       GError *io_error,
				       gpointer callback_data);
typedef void (*GetFdAsyncCallback)    (int fd,
				       gpointer callback_data);
typedef GDBusInterfaceSkeleton *  (*GVfsRegisterVfsFilterCallback)  (GDBusConnection *connection,
                                                                     const char      *obj_path,
                                                                     gpointer         callback_data);


GDBusConnection *_g_dbus_connection_get_sync            (const char                     *dbus_id,
                                                         GCancellable                   *cancellable,
							 GError                        **error);
void            _g_dbus_connection_get_for_async        (const char                     *dbus_id,
                                                         GVfsAsyncDBusCallback           callback,
                                                         gpointer                        callback_data,
                                                         GCancellable                   *cancellable);

gulong          _g_dbus_async_subscribe_cancellable     (GDBusConnection                *connection,
                                                         GCancellable                   *cancellable);
void            _g_dbus_async_unsubscribe_cancellable   (GCancellable                   *cancellable,
                                                         gulong                          cancelled_tag);
void            _g_dbus_send_cancelled_sync             (GDBusConnection                *connection);
void            _g_dbus_send_cancelled_with_serial_sync (GDBusConnection                *connection,
                                                         guint32                         serial);
void            _g_propagate_error_stripped             (GError                        **dest,
                                                         GError                         *src);
G_END_DECLS

#endif /* __G_VFS_DAEMON_DBUS_H__ */
