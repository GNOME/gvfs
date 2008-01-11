/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2008 Red Hat, Inc.
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
 * Author: Christian Kellner <gicmo@gnome.org>
 */

#include <config.h>
#include <string.h>

#include <stdlib.h> /* atoi */

#include <gio/gio.h>
#include <gvfsurimapper.h>
#include <gvfsuriutils.h>

typedef struct _GVfsUriMapperHttp GVfsUriMapperHttp;
typedef struct _GVfsUriMapperHttpClass GVfsUriMapperHttpClass;

struct _GVfsUriMapperHttp
{
  GVfsUriMapper parent;
};

struct _GVfsUriMapperHttpClass
{
  GVfsUriMapperClass parent_class;
};

GType g_vfs_uri_mapper_http_get_type (void);
void  g_vfs_uri_mapper_http_register (GIOModule *module);

G_DEFINE_DYNAMIC_TYPE (GVfsUriMapperHttp, g_vfs_uri_mapper_http, G_VFS_TYPE_URI_MAPPER)

static void
g_vfs_uri_mapper_http_init (GVfsUriMapperHttp *vfs)
{
}

static const char * const *
http_get_handled_schemes (GVfsUriMapper *mapper)
{
  static const char *schemes[] = {
    "http",
    "https",
    NULL
  };
  return schemes;
}

static GVfsUriMountInfo *
http_from_uri (GVfsUriMapper *mapper,
	      const char           *uri_str)
{
  GVfsUriMountInfo *info;

  info = g_vfs_uri_mount_info_new ("http");
  info->path = g_strdup ("/");
  
  g_vfs_uri_mount_info_set (info, "uri", uri_str);

  return info;
}

static const char * const *
http_get_handled_mount_types (GVfsUriMapper *mapper)
{
  static const char *types[] = {
    "http", 
    NULL
  };
  return types;
}

static char *
http_to_uri (GVfsUriMapper    *mapper,
	           GVfsUriMountInfo *info,
	           gboolean          allow_utf8)
{
  return g_strdup (g_vfs_uri_mount_info_get (info, "uri"));
}

static const char *
http_to_uri_scheme (GVfsUriMapper    *mapper,
                    GVfsUriMountInfo *info)
{
  const gchar *uri = g_vfs_uri_mount_info_get (info, "uri");
  
  if (g_ascii_strncasecmp (uri, "https", 5) == 0)
    return "https";
  else if (g_ascii_strncasecmp (uri, "http", 4) == 0)
    return "http";
  else
    return NULL;
}

static void
g_vfs_uri_mapper_http_class_finalize (GVfsUriMapperHttpClass *klass)
{
}

static void
g_vfs_uri_mapper_http_class_init (GVfsUriMapperHttpClass *class)
{
  GObjectClass       *object_class;
  GVfsUriMapperClass *mapper_class;
  
  object_class = (GObjectClass *) class;

  mapper_class = G_VFS_URI_MAPPER_CLASS (class);
  mapper_class->get_handled_schemes     = http_get_handled_schemes;
  mapper_class->from_uri                = http_from_uri;
  mapper_class->get_handled_mount_types = http_get_handled_mount_types;
  mapper_class->to_uri                  = http_to_uri;
  mapper_class->to_uri_scheme           = http_to_uri_scheme;
}

void
g_vfs_uri_mapper_http_register (GIOModule *module)
{
  g_vfs_uri_mapper_http_register_type (G_TYPE_MODULE (module));
}
