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

#include <stdlib.h>
#include <config.h>
#include <string.h>

#include <gio/gio.h>
#include <gvfsurimapper.h>
#include <gvfsuriutils.h>

#define DEFAULT_SMB_PORT 445

typedef struct _GVfsUriMapperSmb GVfsUriMapperSmb;
typedef struct _GVfsUriMapperSmbClass GVfsUriMapperSmbClass;

struct _GVfsUriMapperSmb
{
  GVfsUriMapper parent;
};

struct _GVfsUriMapperSmbClass
{
  GVfsUriMapperClass parent_class;
};

GType g_vfs_uri_mapper_smb_get_type (void);
void g_vfs_uri_mapper_smb_register (GIOModule *module);

G_DEFINE_DYNAMIC_TYPE (GVfsUriMapperSmb, g_vfs_uri_mapper_smb, G_VFS_TYPE_URI_MAPPER)

static void
g_vfs_uri_mapper_smb_init (GVfsUriMapperSmb *vfs)
{
}

static char *
normalize_smb_name (const char *name, gssize len)
{
  gboolean valid_utf8;

  valid_utf8 = g_utf8_validate (name, len, NULL);
  
  if (valid_utf8)
    return g_utf8_casefold (name, len);
  else
    return g_ascii_strdown (name, len);
}

static const char * const *
smb_get_handled_schemes (GVfsUriMapper *mapper)
{
  static const char *schemes[] = {
    "smb",
    NULL
  };
  return schemes;
}

static GMountSpec *
smb_from_uri (GVfsUriMapper *mapper,
	      const char  *uri_str,
              char       **path)
{
  char *tmp;
  const char *p;
  const char *share, *share_end;
  GDecodedUri *uri;
  GMountSpec *spec;

  uri = g_vfs_decode_uri (uri_str);
  if (uri == NULL)
    return NULL;

  if (uri->host == NULL || strlen (uri->host) == 0)
    {
      /* uri form: smb:/// or smb:///$path */
      spec = g_mount_spec_new ("smb-network");
      if (uri->path == NULL || *uri->path == 0)
	*path = g_strdup ("/");
      else
	*path = g_strdup (uri->path);
    }
  else
    {
      /* host set */
      p = uri->path;
      while (p && *p == '/')
	p++;

      if (p == NULL || *p == 0)
	{
	  /* uri form: smb://$host/ */
	  spec = g_mount_spec_new ("smb-server");
          g_mount_spec_take (spec, "server", normalize_smb_name (uri->host, -1));

	  *path = g_strdup ("/");
	}
      else
	{
	  share = p;
	  share_end = strchr (share, '/');
	  if (share_end == NULL)
	    share_end = share + strlen (share);

	  p = share_end;

	  while (*p == '/')
	    p++;

	  if (*p == 0)
	    {
	      /* uri form: smb://$host/$share/
	       * Here we special case smb-server files by adding "._" to the names in the uri */
	      if (share[0] == '.' && share[1] == '_')
		{
		  spec = g_mount_spec_new ("smb-server");
                  g_mount_spec_take (spec, "server", normalize_smb_name (uri->host, -1));

		  tmp = normalize_smb_name (share + 2, share_end - (share + 2));
		  *path = g_strconcat ("/", tmp, NULL);
		  g_free (tmp);
		}
	      else
		{
		  spec = g_mount_spec_new ("smb-share");
		  g_mount_spec_take (spec, "server", normalize_smb_name (uri->host, -1));
                  g_mount_spec_take (spec, "share", normalize_smb_name (share, share_end - share));

		  *path = g_strdup ("/");
		}
	    }
	  else
	    {
	      spec = g_mount_spec_new ("smb-share");
              g_mount_spec_take (spec, "server", normalize_smb_name (uri->host, -1));
              g_mount_spec_take (spec, "share", normalize_smb_name (share, share_end - share));

	      *path = g_strconcat ("/", p, NULL);
	    }
	}

      /* only set the port if it isn't the default port */
      if (uri->port != -1 && uri->port != DEFAULT_SMB_PORT)
	{
	  gchar *port = g_strdup_printf ("%d", uri->port);
	  g_mount_spec_take (spec, "port", port);
	}
    }

  if (uri->userinfo)
    {
      const char *user = uri->userinfo;
      p = strchr (uri->userinfo, ';');
      if (p)
	{
	  if (p != user)
	    g_mount_spec_set_with_len (spec, "domain", user, p - user);
	  user = p + 1;
	}
      if (*user != 0)
	g_mount_spec_set (spec, "user", user);
    }

  g_vfs_decoded_uri_free (uri);

  return spec;
}

static const char * const *
smb_get_handled_mount_types (GVfsUriMapper *mapper)
{
  static const char *types[] = {
    "smb-network", 
    "smb-server", 
    "smb-share",
    NULL
  };
  return types;
}

static char *
smb_to_uri (GVfsUriMapper *mapper,
            GMountSpec *spec,
            const char *path,
	    gboolean allow_utf8)
{
  const char *type;
  const char *server;
  const char *share;
  const char *user;
  const char *domain;
  const char *port = NULL;
  char *s;
  int port_num;
  GDecodedUri *uri;

  uri = g_new0 (GDecodedUri, 1);
  
  type = g_mount_spec_get (spec, "type");

  uri->scheme = g_strdup ("smb");
  
  if (strcmp (type, "smb-network") == 0)
    {
      uri->path = g_strdup (path);
    }
  else if (strcmp (type, "smb-server") == 0)
    {
      server = g_mount_spec_get (spec, "server");
      uri->host = g_strdup (server);

      /* Map the mountables in server to ._share because the actual share mount maps to smb://server/share */
      if (path && path[0] == '/' && path[1] != 0)
	uri->path = g_strconcat ("/._", path + 1, NULL);
      else
	uri->path = g_strdup ("/");
      port = g_mount_spec_get (spec, "port");
    }
  else if (strcmp (type, "smb-share") == 0)
    {
      server = g_mount_spec_get (spec, "server");
      uri->host = g_strdup (server);
      share = g_mount_spec_get (spec, "share");
      if (path[0] == '/')
	uri->path = g_strconcat ("/", share, path, NULL);
      else
	uri->path = g_strconcat ("/", share, "/", path, NULL);

      user = g_mount_spec_get (spec, "user");
      domain = g_mount_spec_get (spec, "domain");
      if (user) {
        if (domain)
          uri->userinfo = g_strconcat (domain, ";", user, NULL);
        else
          uri->userinfo = g_strdup (user);
      }
      port = g_mount_spec_get (spec, "port");
    }

  if (port && (port_num = atoi (port)))
      uri->port = port_num;
  else
      uri->port = -1;

  s = g_vfs_encode_uri (uri, allow_utf8);
  g_vfs_decoded_uri_free (uri);
  return s;
}

static const char *
smb_to_uri_scheme (GVfsUriMapper *mapper,
                   GMountSpec    *spec)
{
  const gchar *type = g_mount_spec_get (spec, "type");

  if (strcmp ("smb-network", type) == 0 ||
      strcmp ("smb-server", type) == 0 ||
      strcmp ("smb-share", type) == 0)
    return "smb";
  else
    return NULL;
}

static void
g_vfs_uri_mapper_smb_class_finalize (GVfsUriMapperSmbClass *klass)
{
}

static void
g_vfs_uri_mapper_smb_class_init (GVfsUriMapperSmbClass *class)
{
  GVfsUriMapperClass *mapper_class;

  mapper_class = G_VFS_URI_MAPPER_CLASS (class);
  mapper_class->get_handled_schemes = smb_get_handled_schemes;
  mapper_class->from_uri = smb_from_uri;
  mapper_class->get_handled_mount_types = smb_get_handled_mount_types;
  mapper_class->to_uri = smb_to_uri;
  mapper_class->to_uri_scheme = smb_to_uri_scheme;
}

void
g_vfs_uri_mapper_smb_register (GIOModule *module)
{
  g_vfs_uri_mapper_smb_register_type (G_TYPE_MODULE (module));
}
