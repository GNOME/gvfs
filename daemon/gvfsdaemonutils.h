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

#ifndef __G_VFS_DAEMON_UTILS_H__
#define __G_VFS_DAEMON_UTILS_H__

#include <glib-object.h>
#include <dbus/dbus.h>

G_BEGIN_DECLS

void         dbus_connection_add_fd_send_fd       (DBusConnection   *connection,
						   int               extra_fd);
gboolean     dbus_connection_send_fd              (DBusConnection   *connection,
						   int               fd,
						   int              *fd_id,
						   GError          **error);
char *       g_error_to_daemon_reply              (GError           *error,
						   guint32           seq_nr,
						   gsize            *len_out);

void	     gvfs_file_info_populate_default	    (GFileInfo        *info,
						     const char       *name_string,
						     GFileType	      type);
char *	     gvfs_file_info_populate_names_as_local (GFileInfo        *info,
						     const char       *name_string);
void	     gvfs_file_info_populate_content_types  (GFileInfo        *info,
						     const char       *basename,
						     GFileType         type);

G_END_DECLS

#endif /* __G_VFS_DAEMON_UTILS_H__ */
