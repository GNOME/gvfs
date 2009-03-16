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

static inline gboolean
port_is_defaul_port (int port, gboolean ssl)
{
  if (ssl)
    return port == 443;
  else
    return port == 80;
}

static GVfsUriMountInfo *
http_from_uri (GVfsUriMapper *mapper,
               const char     *uri_str)
{
  GVfsUriMountInfo *info;
  gboolean ssl;
  GDecodedUri *uri;

  uri = g_vfs_decode_uri (uri_str);

  if (uri == NULL)
    return NULL;

  if (!g_ascii_strncasecmp (uri->scheme, "http", 4))
    {
      info = g_vfs_uri_mount_info_new ("http");
      g_vfs_uri_mount_info_set (info, "uri", uri_str);
    }
  else
    {
      info = g_vfs_uri_mount_info_new ("dav");
      ssl = !g_ascii_strcasecmp (uri->scheme, "davs");
      g_vfs_uri_mount_info_set (info, "ssl", ssl ? "true" : "false");

      if (uri->host && *uri->host)
        g_vfs_uri_mount_info_set (info, "host", uri->host);

      if (uri->userinfo && *uri->userinfo)
        g_vfs_uri_mount_info_set (info, "user", uri->userinfo);

      /* only set the port if it isn't the default port */
      if (uri->port != -1 && ! port_is_defaul_port (uri->port, ssl))
        {
          char *port = g_strdup_printf ("%d", uri->port);
          g_vfs_uri_mount_info_set (info, "port", port);
          g_free (port);
        }
    }

  info->path = uri->path;
  uri->path = NULL;
  g_vfs_decoded_uri_free (uri);

  return info;
}

static GVfsUriMountInfo *
http_get_mount_info_for_path (GVfsUriMapper *mapper,
			      GVfsUriMountInfo *info,
			      const char *new_path)
{
  const char *type;

  type = g_vfs_uri_mount_info_get (info, "type");

  if (strcmp (type, "http") == 0)
    {
      const char *uri_str;
      char *new_uri;
      GDecodedUri *uri;
      GVfsUriMountInfo *new_info;

      uri_str = g_vfs_uri_mount_info_get (info, "uri");

      uri = g_vfs_decode_uri (uri_str);

      if (uri == NULL)
        return NULL;

      if (strcmp (uri->path, new_path) == 0)
        {
          g_vfs_decoded_uri_free (uri);
          return NULL;
        }

      g_free (uri->path);
      uri->path = g_strdup (new_path);

      g_free (uri->query);
      uri->query = NULL;

      g_free (uri->fragment);
      uri->fragment = NULL;

      new_info = g_vfs_uri_mount_info_new ("http");

      new_uri = g_vfs_encode_uri (uri, TRUE);
      g_vfs_uri_mount_info_set (new_info, "uri", new_uri);
      g_free (new_uri);

      g_vfs_decoded_uri_free (uri);

      return new_info;
    }
  else
    return NULL;
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
  const char *type;
  const char *host;
  const char *user;
  const char *port;
  const char *ssl;

  type = g_vfs_uri_mount_info_get (info, "type");

  if (strcmp (type, "http") == 0)
    {
      res = g_strdup (g_vfs_uri_mount_info_get (info, "uri"));
    }
  else
    {
      GDecodedUri *decoded_uri;
      int          port_num;

      decoded_uri = g_new0 (GDecodedUri, 1);

      ssl  = g_vfs_uri_mount_info_get (info, "ssl");
      host = g_vfs_uri_mount_info_get (info, "host");
      user = g_vfs_uri_mount_info_get (info, "user");
      port = g_vfs_uri_mount_info_get (info, "port");

      if (ssl && strcmp (ssl, "true") == 0)
          decoded_uri->scheme = g_strdup ("davs");
      else
          decoded_uri->scheme = g_strdup ("dav");

      decoded_uri->host = g_strdup (host);
      decoded_uri->userinfo = g_strdup (user);
      
      if (port && (port_num = atoi (port)))
          decoded_uri->port = port_num;
      else
          decoded_uri->port = -1;

      decoded_uri->path = g_strdup (info->path);

      res = g_vfs_encode_uri (decoded_uri, allow_utf8);
      g_vfs_decoded_uri_free (decoded_uri);
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
  GVfsUriMapperClass *mapper_class;
  
  mapper_class = G_VFS_URI_MAPPER_CLASS (class);
  mapper_class->get_handled_schemes     = http_get_handled_schemes;
  mapper_class->from_uri                = http_from_uri;
  mapper_class->get_mount_info_for_path = http_get_mount_info_for_path;
  mapper_class->get_handled_mount_types = http_get_handled_mount_types;
  mapper_class->to_uri                  = http_to_uri;
  mapper_class->to_uri_scheme           = http_to_uri_scheme;
}

void
g_vfs_uri_mapper_http_register (GIOModule *module)
{
  g_vfs_uri_mapper_http_register_type (G_TYPE_MODULE (module));
}
