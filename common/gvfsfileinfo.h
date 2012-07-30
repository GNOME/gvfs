/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2009 Red Hat, Inc.
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

#ifndef __G_VFS_FILE_INFO_H__
#define __G_VFS_FILE_INFO_H__

#include <glib.h>

G_BEGIN_DECLS

char *     gvfs_file_info_marshal   (GFileInfo *info,
				     gsize     *size);
GFileInfo *gvfs_file_info_demarshal (char      *data,
				     gsize      size);

G_END_DECLS

#endif /* __G_VFS_FILE_INFO_H__ */
