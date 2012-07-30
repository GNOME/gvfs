/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2008 Benjamin Otte <otte@gnome.org>
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

#ifndef __G_VFS_BACKEND_ARCHIVE_H__
#define __G_VFS_BACKEND_ARCHIVE_H__

#include <gvfsbackend.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_ARCHIVE         (g_vfs_backend_archive_get_type ())
#define G_VFS_BACKEND_ARCHIVE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND_ARCHIVE, GVfsBackendArchive))
#define G_VFS_BACKEND_ARCHIVE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_ARCHIVE, GVfsBackendArchiveClass))
#define G_VFS_IS_BACKEND_ARCHIVE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND_ARCHIVE))
#define G_VFS_IS_BACKEND_ARCHIVE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND_ARCHIVE))
#define G_VFS_BACKEND_ARCHIVE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND_ARCHIVE, GVfsBackendArchiveClass))

typedef struct _GVfsBackendArchive        GVfsBackendArchive;
typedef struct _GVfsBackendArchiveClass   GVfsBackendArchiveClass;

struct _GVfsBackendArchiveClass
{
  GVfsBackendClass parent_class;
};

GType g_vfs_backend_archive_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif /* __G_VFS_BACKEND_ARCHIVE_H__ */
