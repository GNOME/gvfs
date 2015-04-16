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

#ifndef __G_DOCUMENT_FILE_H__
#define __G_DOCUMENT_FILE_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define GVFS_TYPE_DOCUMENT_FILE         (gvfs_document_file_get_type ())
#define GVFS_DOCUMENT_FILE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), GVFS_TYPE_DOCUMENT_FILE, GVfsDocumentFile))
#define GVFS_DOCUMENT_FILE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), GVFS_TYPE_DOCUMENT_FILE, GVfsDocumentFileClass))
#define GVFS_IS_DOCUMENT_FILE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), GVFS_TYPE_DOCUMENT_FILE))
#define GVFS_IS_DOCUMENT_FILE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), GVFS_TYPE_DOCUMENT_FILE))
#define GVFS_DOCUMENT_FILE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_DOCUMENT_FILE, GVfsDocumentFileClass))

typedef struct _GVfsDocumentFile        GVfsDocumentFile;
typedef struct _GVfsDocumentFileClass   GVfsDocumentFileClass;

struct _GVfsDocumentFileClass
{
  GObjectClass parent_class;
};

struct _GVfsDocumentFile
{
  GObject parent_instance;

  char *path;
};

GType gvfs_document_file_get_type (void) G_GNUC_CONST;
  
GFile * gvfs_document_file_new (const char *uri);

G_END_DECLS

#endif /* __G_DOCUMENT_FILE_H__ */
