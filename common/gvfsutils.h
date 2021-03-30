/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2014 Ross Lagerwall
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
 * Public License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef __G_VFS_UTILS_H__
#define __G_VFS_UTILS_H__

G_BEGIN_DECLS

void         gvfs_randomize_string                  (char             *str,
                                                     int               len);
gboolean     gvfs_have_session_bus                  (void);

gboolean     gvfs_get_debug                         (void);
void         gvfs_set_debug                         (gboolean          debugging);
void         gvfs_setup_debug_handler               (void);

gboolean     gvfs_is_ipv6                           (const char       *host);
gchar *      gvfs_get_socket_dir                    (void);

G_END_DECLS

#endif /* __G_VFS_UTILS_H__ */
