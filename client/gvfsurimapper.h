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

#ifndef __G_VFS_URI_MAPPER_H__
#define __G_VFS_URI_MAPPER_H__

#include <glib-object.h>
#include <gio/gio.h>

#include <common/gmountspec.h>

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

struct _GVfsUriMapperClass
{
  GObjectClass parent_class;

  /* Virtual Table */

  const char * const * (*get_handled_schemes)     (GVfsUriMapper *mapper);
  GMountSpec *         (*from_uri)                (GVfsUriMapper *mapper,
					           const char *uri,
                                                   char **path);
  GMountSpec *         (*get_mount_spec_for_path) (GVfsUriMapper *mapper,
						   GMountSpec *mount_spec,
                                                   const char *old_path,
					           const char *new_path);

  const char * const * (*get_handled_mount_types) (GVfsUriMapper *mapper);
  char *               (*to_uri)                  (GVfsUriMapper *mapper,
					           GMountSpec *mount_spec,
                                                   const char *path,
					           gboolean allow_utf8);
  const char *         (*to_uri_scheme)           (GVfsUriMapper *mapper,
					           GMountSpec *mount_spec);
};

GType g_vfs_uri_mapper_get_type (void);
void g_vfs_uri_mapper_register (GIOModule *module);

const char * const *g_vfs_uri_mapper_get_handled_schemes     (GVfsUriMapper    *mapper);
GMountSpec *        g_vfs_uri_mapper_from_uri                (GVfsUriMapper    *mapper,
							      const char       *uri,
                                                              char            **path);
GMountSpec *        g_vfs_uri_mapper_get_mount_spec_for_path (GVfsUriMapper    *mapper,
							      GMountSpec       *mount_spec,
                                                              const char       *old_path,
							      const char       *new_path);

const char * const *g_vfs_uri_mapper_get_handled_mount_types (GVfsUriMapper    *mapper);
char *              g_vfs_uri_mapper_to_uri                  (GVfsUriMapper    *mapper,
							      GMountSpec       *mount_spec,
                                                              const char       *path,
							      gboolean          allow_utf8);
const char *        g_vfs_uri_mapper_to_uri_scheme           (GVfsUriMapper    *mapper,
							      GMountSpec       *mount_spec);


G_END_DECLS

#endif /* __G_VFS_URI_MAPPER_H__ */
