/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 * Copyright (C) Carl-Anton Ingmarsson 2011 <ca.ingmarsson@gmail.com>.
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
 *         Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
 */

#include <config.h>
#include <string.h>

#include <stdlib.h>
#include <gio/gio.h>
#include <gvfsurimapper.h>
#include <gvfsuriutils.h>

typedef struct _GVfsUriMapperAfp GVfsUriMapperAfp;
typedef struct _GVfsUriMapperAfpClass GVfsUriMapperAfpClass;

struct _GVfsUriMapperAfp
{
  GVfsUriMapper parent;
};

struct _GVfsUriMapperAfpClass
{
  GVfsUriMapperClass parent_class;
};

GType g_vfs_uri_mapper_afp_get_type (void);
void g_vfs_uri_mapper_afp_register (GIOModule *module);

G_DEFINE_DYNAMIC_TYPE (GVfsUriMapperAfp, g_vfs_uri_mapper_afp, G_VFS_TYPE_URI_MAPPER)

static void
g_vfs_uri_mapper_afp_init (GVfsUriMapperAfp *vfs)
{
}

static const char * const *
afp_get_handled_schemes (GVfsUriMapper *mapper)
{
  static const char *schemes[] = {
    "afp",
    NULL
  };
  return schemes;
}

static GMountSpec *
afp_from_uri (GVfsUriMapper  *mapper,
              const char     *uri_str,
              char          **path)
{
  const char *p;
  GDecodedUri *uri;
  GMountSpec *spec;

  uri = g_vfs_decode_uri (uri_str);
  if (uri == NULL)
    return NULL;

  if (uri->host == NULL || strlen (uri->host) == 0)
  {
    g_vfs_decoded_uri_free (uri);
    return NULL;
  }
  else
  {
    /* host set */
    p = uri->path;
    while (p && *p == '/')
      p++;

    if (p == NULL || *p == 0)
    {
      /* uri form: afp://$host/ */
       spec = g_mount_spec_new ("afp-server");

       g_mount_spec_set (spec, "host", uri->host);
       *path = g_strdup ("/");
    }
    else
    {
      const char *volume, *volume_end;

      volume = p;
      volume_end = strchr (volume, '/');
      if (volume_end == NULL)
        volume_end = volume + strlen (volume);

      p = volume_end;

      while (*p == '/')
        p++;

      if (*p == 0)
      {
        /* uri form: afp://$host/$volume/
         * Here we special case afp-server files by adding "._" to the names in the uri */
        if (volume[0] == '.' && volume[1] == '_')
        {
          char *tmp;

          spec = g_mount_spec_new ("afp-server");
          g_mount_spec_set (spec, "host", uri->host);

          tmp = g_strndup (volume + 2, volume_end - (volume + 2));
          *path = g_strconcat ("/", tmp, NULL);
          g_free (tmp);
        }
        else
        {
          char *tmp;

          spec = g_mount_spec_new ("afp-volume");
          g_mount_spec_set (spec, "host", uri->host);

          tmp = g_strndup (volume, volume_end - volume);
          g_mount_spec_take (spec, "volume", tmp);

          *path = g_strdup ("/");
        }
      }
      else
      {
        char *tmp;

        spec = g_mount_spec_new ("afp-volume");
        g_mount_spec_set (spec, "host", uri->host);

        tmp = g_strndup (volume, volume_end - volume);
        g_mount_spec_take (spec, "volume", tmp);

        *path = g_strconcat ("/", p, NULL);
      }
    }
  }

  if (uri->userinfo)
    g_mount_spec_set (spec, "user", uri->userinfo);

  g_vfs_decoded_uri_free (uri);

  return spec;
}

static const char * const *
afp_get_handled_mount_types (GVfsUriMapper *mapper)
{
  static const char *types[] = {
    "afp-server",
    "afp-volume",
    NULL
  };
  return types;
}

static char *
afp_to_uri (GVfsUriMapper *mapper,
            GMountSpec *spec,
            const char *path,
            gboolean allow_utf8)
{
  const char *type;
  const char *host;
  const char *port;
  const char *user;
  char *s;
  GDecodedUri *uri;

  uri = g_new0 (GDecodedUri, 1);

  type = g_mount_spec_get (spec, "type");

  uri->scheme = g_strdup ("afp");

  host = g_mount_spec_get (spec, "host");
  uri->host = g_strdup (host);

  port = g_mount_spec_get (spec, "port");
  if (port)
    uri->port = atoi (port);
  else
    uri->port = -1;

  user = g_mount_spec_get (spec, "user");
  uri->userinfo = g_strdup (user);

  if (strcmp (type, "afp-server") == 0)
  {
    /* Map the mountables in server to ._share because the actual share mount maps to afp://host/share */
     if (path && path[0] == '/' && path[1] != 0)
     uri->path = g_strconcat ("/._", path + 1, NULL);
     else
     uri->path = g_strdup ("/");
  }
  else if (strcmp (type, "afp-volume") == 0)
  {
    const char *volume;

    volume = g_mount_spec_get (spec, "volume");
    if (path[0] == '/')
      uri->path = g_strconcat ("/", volume, path, NULL);
    else
      uri->path = g_strconcat ("/", volume, "/", path, NULL);
  }

  s = g_vfs_encode_uri (uri, allow_utf8);
  g_vfs_decoded_uri_free (uri);
  return s;
}

static const char *
afp_to_uri_scheme (GVfsUriMapper *mapper,
                   GMountSpec *spec)
{
  const gchar *type = g_mount_spec_get (spec, "type");

  if (strcmp ("afp-server", type) == 0 ||
      strcmp ("afp-volume", type) == 0)
    return "afp";
  else
    return NULL;
}

static void
g_vfs_uri_mapper_afp_class_finalize (GVfsUriMapperAfpClass *klass)
{
}

static void
g_vfs_uri_mapper_afp_class_init (GVfsUriMapperAfpClass *class)
{
  GVfsUriMapperClass *mapper_class;

  mapper_class = G_VFS_URI_MAPPER_CLASS (class);
  mapper_class->get_handled_schemes = afp_get_handled_schemes;
  mapper_class->from_uri = afp_from_uri;
  mapper_class->get_handled_mount_types = afp_get_handled_mount_types;
  mapper_class->to_uri = afp_to_uri;
  mapper_class->to_uri_scheme = afp_to_uri_scheme;
}

void
g_vfs_uri_mapper_afp_register (GIOModule *module)
{
  g_vfs_uri_mapper_afp_register_type (G_TYPE_MODULE (module));
}
