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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Benjamin Otte <otte@gnome.org>
 */

#ifndef __G_VFS_FTP_FILE_H__
#define __G_VFS_FTP_FILE_H__

#include <gvfsbackendftp.h>

G_BEGIN_DECLS


GVfsFtpFile *     g_vfs_ftp_file_new_from_gvfs          (GVfsBackendFtp *       ftp,
                                                         const char *           gvfs_path);
GVfsFtpFile *     g_vfs_ftp_file_new_from_ftp           (GVfsBackendFtp *       ftp,
                                                         const char *           ftp_path);
GVfsFtpFile *     g_vfs_ftp_file_new_parent             (const GVfsFtpFile *    file);
GVfsFtpFile *     g_vfs_ftp_file_new_child              (const GVfsFtpFile *    parent,
                                                         const char *           display_name,
                                                         GError **              error);
GVfsFtpFile *     g_vfs_ftp_file_copy                   (const GVfsFtpFile *    file);
void              g_vfs_ftp_file_free                   (GVfsFtpFile *          file);

gboolean          g_vfs_ftp_file_is_root                (const GVfsFtpFile *    file);
const char *      g_vfs_ftp_file_get_ftp_path           (const GVfsFtpFile *    file);
const char *      g_vfs_ftp_file_get_gvfs_path          (const GVfsFtpFile *    file);

gboolean          g_vfs_ftp_file_equal                  (gconstpointer          a,
                                                         gconstpointer          b);
guint             g_vfs_ftp_file_hash                   (gconstpointer          a);


G_END_DECLS

#endif /* __G_VFS_FTP_FILE_H__ */
