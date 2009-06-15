/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2009 Benjamin Otte <otte@gnome.org>
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
 * Author: Benjamin Otte <otte@gnome.org>
 */

#ifndef __G_VFS_FTP_CONNECTION_H__
#define __G_VFS_FTP_CONNECTION_H__

#include <gio/gio.h>

G_BEGIN_DECLS


typedef struct _GVfsFtpConnection GVfsFtpConnection;

GVfsFtpConnection *     g_vfs_ftp_connection_new              (GSocketConnectable *     addr,
                                                               GCancellable *           cancellable,
                                                               GError **                error);
void                    g_vfs_ftp_connection_free             (GVfsFtpConnection *      conn);

gboolean                g_vfs_ftp_connection_send             (GVfsFtpConnection *      conn,
                                                               const char *             command,
                                                               int                      len,
                                                               GCancellable *           cancellable,
                                                               GError **                error);
guint                   g_vfs_ftp_connection_receive          (GVfsFtpConnection *      conn,
                                                               char ***                 reply,
                                                               GCancellable *           cancellable,
                                                               GError **                error);

gboolean                g_vfs_ftp_connection_is_usable        (GVfsFtpConnection *      conn);
GSocketAddress *        g_vfs_ftp_connection_get_address      (GVfsFtpConnection *      conn,
                                                               GError **                error);
guint                   g_vfs_ftp_connection_get_debug_id     (GVfsFtpConnection *      conn);

gboolean                g_vfs_ftp_connection_open_data_connection
                                                              (GVfsFtpConnection *      conn,
                                                               GSocketAddress *         addr,
                                                               GCancellable *           cancellable,
                                                               GError **                error);
GSocketAddress *        g_vfs_ftp_connection_listen_data_connection
                                                              (GVfsFtpConnection *      conn,
                                                               GError **                error);
gboolean                g_vfs_ftp_connection_accept_data_connection
                                                              (GVfsFtpConnection *      conn,
                                                               GCancellable *           cancellable,
                                                               GError **                error);
void                    g_vfs_ftp_connection_close_data_connection
                                                              (GVfsFtpConnection *      conn);
GIOStream *             g_vfs_ftp_connection_get_data_stream  (GVfsFtpConnection *      conn);



G_END_DECLS

#endif /* __G_VFS_FTP_CONNECTION_H__ */
