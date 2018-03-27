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

#include <config.h>
#include <string.h>
#include "gvfsurimapper.h"

G_DEFINE_DYNAMIC_TYPE (GVfsUriMapper, g_vfs_uri_mapper, G_TYPE_OBJECT)

void
g_vfs_uri_mapper_register (GIOModule *module)
{
  g_vfs_uri_mapper_register_type (G_TYPE_MODULE (module));
}

static void
g_vfs_uri_mapper_class_finalize (GVfsUriMapperClass *klass)
{
}

static void
g_vfs_uri_mapper_class_init (GVfsUriMapperClass *klass)
{
}

static void
g_vfs_uri_mapper_init (GVfsUriMapper *vfs)
{
}

const char * const *
g_vfs_uri_mapper_get_handled_schemes (GVfsUriMapper  *mapper)
{
  GVfsUriMapperClass *class;

  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);

  return (* class->get_handled_schemes) (mapper);
}

GMountSpec *
g_vfs_uri_mapper_from_uri (GVfsUriMapper  *mapper,
			   const char     *uri,
                           char          **path)
{
  GVfsUriMapperClass *class;

  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);

  return (* class->from_uri) (mapper, uri, path);
}

GMountSpec *
g_vfs_uri_mapper_get_mount_spec_for_path (GVfsUriMapper    *mapper,
					  GMountSpec       *spec,
                                          const char       *old_path,
					  const char       *new_path)
{
  GVfsUriMapperClass *class;

  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);

  if (class->get_mount_spec_for_path != NULL)
    return (* class->get_mount_spec_for_path) (mapper, spec, old_path, new_path);
  else
    return NULL;
}

const char * const *
g_vfs_uri_mapper_get_handled_mount_types (GVfsUriMapper  *mapper)
{
  GVfsUriMapperClass *class;
  
  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);

  return (* class->get_handled_mount_types) (mapper);
}

char *
g_vfs_uri_mapper_to_uri (GVfsUriMapper *mapper,
			 GMountSpec *mount_spec,
                         const char *path,
			 gboolean allow_utf8)
{
  GVfsUriMapperClass *class;
  
  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);
  
  return (* class->to_uri) (mapper, mount_spec, path, allow_utf8);
}

const char *
g_vfs_uri_mapper_to_uri_scheme (GVfsUriMapper *mapper,
                                GMountSpec    *mount_spec)
{
  GVfsUriMapperClass *class;

  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);

  return (* class->to_uri_scheme) (mapper, mount_spec);
}
