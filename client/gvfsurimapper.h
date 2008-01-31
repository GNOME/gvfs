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

#ifndef __G_VFS_URI_MAPPER_H__
#define __G_VFS_URI_MAPPER_H__

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_URI_MAPPER         (g_vfs_uri_mapper_get_type ())
#define G_VFS_URI_MAPPER(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_URI_MAPPER, GVfsUriMapper))
#define G_VFS_URI_MAPPER_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_URI_MAPPER, GVfsUriMapperClass))
#define G_VFS_URI_MAPPER_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_URI_MAPPER, GVfsUriMapperClass))
#define G_IS_VFS_URI_MAPPER(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_URI_MAPPER))
#define G_IS_VFS_URI_MAPPER_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_URI_MAPPER))

typedef struct _GVfsUriMapper         GVfsUriMapper; /* Dummy typedef */
typedef struct _GVfsUriMapperClass    GVfsUriMapperClass;

struct _GVfsUriMapper {
  GObject parent;
};

/* Keep in sync with GMountSpecItem */
typedef struct {
  char *key;
  char *value;
} GVfsUriMountInfoKey;

typedef struct {
  GArray *keys;
  char *path;
} GVfsUriMountInfo;

struct _GVfsUriMapperClass
{
  GObjectClass parent_class;

  /* Virtual Table */

  const char * const * (*get_handled_schemes)     (GVfsUriMapper *mapper);
  GVfsUriMountInfo *   (*from_uri)                (GVfsUriMapper *mapper,
					           const char *uri);
  GVfsUriMountInfo *   (*get_mount_info_for_path) (GVfsUriMapper *mapper,
						   GVfsUriMountInfo *mount_info,
					           const char *new_path);

  const char * const * (*get_handled_mount_types) (GVfsUriMapper *mapper);
  char *               (*to_uri)                  (GVfsUriMapper *mapper,
					           GVfsUriMountInfo *mount_info,
					           gboolean allow_utf8);
  const char *         (*to_uri_scheme)           (GVfsUriMapper *mapper,
					           GVfsUriMountInfo *mount_info);
};

GType g_vfs_uri_mapper_get_type (void);
void g_vfs_uri_mapper_register (GIOModule *module);

GVfsUriMountInfo *g_vfs_uri_mount_info_new          (const char       *type);
void              g_vfs_uri_mount_info_free         (GVfsUriMountInfo *info);
const char *      g_vfs_uri_mount_info_get          (GVfsUriMountInfo *info,
						     const char       *key);
void              g_vfs_uri_mount_info_set          (GVfsUriMountInfo *info,
						     const char       *key,
						     const char       *value);
void              g_vfs_uri_mount_info_set_with_len (GVfsUriMountInfo *info,
						     const char       *key,
						     const char       *value,
						     int               value_len);

const char * const *g_vfs_uri_mapper_get_handled_schemes     (GVfsUriMapper    *mapper);
GVfsUriMountInfo *  g_vfs_uri_mapper_from_uri                (GVfsUriMapper    *mapper,
							      const char       *uri);
GVfsUriMountInfo *  g_vfs_uri_mapper_get_mount_info_for_path (GVfsUriMapper    *mapper,
							      GVfsUriMountInfo *mount_info,
							      const char       *new_path);

const char * const *g_vfs_uri_mapper_get_handled_mount_types (GVfsUriMapper    *mapper);
char *              g_vfs_uri_mapper_to_uri                  (GVfsUriMapper    *mapper,
							      GVfsUriMountInfo *mount_info,
							      gboolean          allow_utf8);
const char *        g_vfs_uri_mapper_to_uri_scheme           (GVfsUriMapper    *mapper,
							      GVfsUriMountInfo *mount_infoxo);


G_END_DECLS

#endif /* __G_VFS_URI_MAPPER_H__ */
