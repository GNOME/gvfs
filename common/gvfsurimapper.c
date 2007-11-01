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

#include <config.h>
#include <string.h>
#include <gmodule.h>
#include "gvfsurimapper.h"

G_DEFINE_TYPE (GVfsUriMapper, g_vfs_uri_mapper, G_TYPE_OBJECT);

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

  
gboolean
g_vfs_uri_mapper_from_uri (GVfsUriMapper  *mapper,
			   const char     *uri,
			   GMountSpec    **spec_out,
			   char          **path_out)
{
  GVfsUriMapperClass *class;

  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);

  return (* class->from_uri) (mapper, uri, spec_out, path_out);
}

const char * const *
g_vfs_uri_mapper_get_handled_mount_types (GVfsUriMapper  *mapper)
{
  GVfsUriMapperClass *class;
  
  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);

  return (* class->get_handled_mount_types) (mapper);
}

char *
g_vfs_uri_mapper_to_uri (GVfsUriMapper  *mapper,
			 GMountSpec     *spec,
			 char           *path,
			 gboolean        allow_utf8)
{
  GVfsUriMapperClass *class;
  
  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);
  
  return (* class->to_uri) (mapper, spec, path, allow_utf8);
}

const char *
g_vfs_uri_mapper_to_uri_scheme (GVfsUriMapper  *mapper,
                                GMountSpec     *spec)
{
  GVfsUriMapperClass *class;
  
  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);
  
  return (* class->to_uri_scheme) (mapper, spec);
}
						       
