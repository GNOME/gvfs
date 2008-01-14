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
    "dav",
    "davs",
    NULL
  };
  return schemes;
}

static GVfsUriMountInfo *
http_from_uri (GVfsUriMapper *mapper,
	      const char     *uri_str)
{
  GVfsUriMountInfo *info;
  gboolean          ssl;
  char             *path;

  if (!g_ascii_strncasecmp (uri_str, "http", 4))
    {
      ssl = !g_ascii_strncasecmp (uri_str, "https", 5);
      info = g_vfs_uri_mount_info_new ("http");
      path = g_strdup ("/");

      g_vfs_uri_mount_info_set (info, "uri", uri_str);

    }
  else
    {
      GDecodedUri *uri;

      uri = g_vfs_decode_uri (uri_str);

      if (uri == NULL)
          return NULL;

      info = g_vfs_uri_mount_info_new ("dav");
      ssl = !g_ascii_strcasecmp (uri->scheme, "davs");

      if (uri->host && *uri->host)
          g_vfs_uri_mount_info_set (info, "host", uri->host);

      if (uri->userinfo && *uri->userinfo)
          g_vfs_uri_mount_info_set (info, "user", uri->userinfo);

      if (uri->port != -1)
        {
          char *port = g_strdup_printf ("%d", uri->port);
          g_vfs_uri_mount_info_set (info, "port", port);
          g_free (port);
        }

      path = uri->path;
      uri->path = NULL;
      g_vfs_decoded_uri_free (uri);
    }


  info->path = path;
  g_vfs_uri_mount_info_set (info, "ssl", ssl ? "true" : "false");

  return info;
}

static const char * const *
http_get_handled_mount_types (GVfsUriMapper *mapper)
{
  static const char *types[] = {
    "http",
    "dav", 
    NULL
  };
  return types;
}

static char *
http_to_uri (GVfsUriMapper    *mapper,
             GVfsUriMountInfo *info,
             gboolean          allow_utf8)
{
  char       *res;
  const char *uri;
  const char *type;
  const char *host;
  const char *user;
  const char *port;
  const char *ssl;

  type = g_vfs_uri_mount_info_get (info, "type");

  if (strcmp (type, "http") == 0)
    {
      uri = g_vfs_uri_mount_info_get (info, "uri");
      res = g_strdup (g_vfs_uri_mount_info_get (info, "uri"));
    }
  else
    {
      GDecodedUri *uri;
      int          port_num;

      uri = g_new0 (GDecodedUri, 1);

      ssl  = g_vfs_uri_mount_info_get (info, "ssl");
      host = g_vfs_uri_mount_info_get (info, "host");
      user = g_vfs_uri_mount_info_get (info, "user");
      port = g_vfs_uri_mount_info_get (info, "port");

      if (ssl && strcmp (ssl, "true") == 0)
          uri->scheme = g_strdup ("davs");
      else
          uri->scheme = g_strdup ("dav");

      uri->host = g_strdup (host);
      uri->userinfo = g_strdup (user);
      
      if (port && (port_num = atoi (port)))
          uri->port = port_num;

      uri->path = g_strdup (info->path);

      res = g_vfs_encode_uri (uri, allow_utf8);
      g_vfs_decoded_uri_free (uri);
    }

  return res;
}

static const char *
http_to_uri_scheme (GVfsUriMapper    *mapper,
                    GVfsUriMountInfo *info)
{
  const gchar *ssl;
  const gchar *type;
  gboolean     is_dav;
  gboolean     is_ssl;

  ssl = g_vfs_uri_mount_info_get (info, "ssl");
  type = g_vfs_uri_mount_info_get (info, "type");
  
  if (strcmp (type, "dav") == 0)
     is_dav = TRUE;
  else if (strcmp (type, "http") == 0)
     is_dav = FALSE;
  else
     return NULL; 

  is_ssl =
    ssl != NULL &&
    strcmp (ssl, "true") == 0;
  
  if (is_dav && is_ssl)
    return "davs";
  else if (is_dav && !is_ssl)
    return "dav";
  else if (!is_dav && is_ssl)
    return "https";
  else
    return "http";
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
