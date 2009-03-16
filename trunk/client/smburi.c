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

#include <gio/gio.h>
#include <gvfsurimapper.h>
#include <gvfsuriutils.h>

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

static GVfsUriMountInfo *
smb_from_uri (GVfsUriMapper *mapper,
	      const char *uri_str)
{
  char *tmp;
  const char *p;
  const char *share, *share_end;
  GDecodedUri *uri;
  GVfsUriMountInfo *info;

  uri = g_vfs_decode_uri (uri_str);
  if (uri == NULL)
    return NULL;
  
  if (uri->host == NULL || strlen (uri->host) == 0)
    {
      /* uri form: smb:/// or smb:///$path */
      info = g_vfs_uri_mount_info_new ("smb-network");
      if (uri->path == NULL || *uri->path == 0)
	info->path = g_strdup ("/");
      else
	info->path = g_strdup (uri->path);
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
	  info = g_vfs_uri_mount_info_new ("smb-server");
	  tmp = normalize_smb_name (uri->host, -1);
	  g_vfs_uri_mount_info_set (info, "server", tmp);
	  g_free (tmp);
	  info->path = g_strdup ("/");
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
		  info = g_vfs_uri_mount_info_new ("smb-server");
		  tmp = normalize_smb_name (uri->host, -1);
		  g_vfs_uri_mount_info_set  (info, "server", tmp);
		  g_free (tmp);
		  tmp = normalize_smb_name (share + 2, share_end - (share + 2));
		  info->path = g_strconcat ("/", tmp, NULL);
		  g_free (tmp);
		}
	      else
		{
		  info = g_vfs_uri_mount_info_new ("smb-share");
		  tmp = normalize_smb_name (uri->host, -1);
		  g_vfs_uri_mount_info_set  (info, "server", tmp);
		  g_free (tmp);
		  tmp = normalize_smb_name (share, share_end - share);
		  g_vfs_uri_mount_info_set  (info, "share", tmp);
		  g_free (tmp);
		  info->path = g_strdup ("/");
		}
	    }
	  else
	    {
	      info = g_vfs_uri_mount_info_new ("smb-share");
	      
	      tmp = normalize_smb_name (uri->host, -1);
	      g_vfs_uri_mount_info_set  (info, "server", tmp);
	      g_free (tmp);
	      
	      tmp = normalize_smb_name (share, share_end - share);
	      g_vfs_uri_mount_info_set  (info, "share", tmp);
	      g_free (tmp);
	      
	      info->path = g_strconcat ("/", p, NULL);
	    }
	}
    }
  
  if (uri->userinfo)
    {
      const char *user = uri->userinfo;
      p = strchr (uri->userinfo, ';');
      if (p)
	{
	  if (p != user)
	    g_vfs_uri_mount_info_set_with_len  (info, "domain", user, p - user);
	  user = p + 1;
	}
      if (*user != 0)
	g_vfs_uri_mount_info_set  (info, "user", user);
    }

  g_vfs_decoded_uri_free (uri);
  
  return info;
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
	    GVfsUriMountInfo *info,
	    gboolean allow_utf8)
{
  const char *type;
  const char *server;
  const char *share;
  const char *user;
  const char *domain;
  char *s;
  GDecodedUri *uri;

  uri = g_new0 (GDecodedUri, 1);
  
  type = g_vfs_uri_mount_info_get (info, "type");

  uri->scheme = g_strdup ("smb");
  uri->port = -1;
  
  if (strcmp (type, "smb-network") == 0)
    {
      uri->path = g_strdup (info->path);
    }
  else if (strcmp (type, "smb-server") == 0)
    {
      server = g_vfs_uri_mount_info_get (info, "server");
      uri->host = g_strdup (server);

      /* Map the mountables in server to ._share because the actual share mount maps to smb://server/share */
      if (info->path && info->path[0] == '/' && info->path[1] != 0)
	uri->path = g_strconcat ("/._", info->path + 1, NULL);
      else
	uri->path = g_strdup ("/");
    }
  else if (strcmp (type, "smb-share") == 0)
    {
      server = g_vfs_uri_mount_info_get (info, "server");
      uri->host = g_strdup (server);
      share = g_vfs_uri_mount_info_get (info, "share");
      if (info->path[0] == '/')
	uri->path = g_strconcat ("/", share, info->path, NULL);
      else
	uri->path = g_strconcat ("/", share, "/", info->path, NULL);
	
      user = g_vfs_uri_mount_info_get (info, "user");
      domain = g_vfs_uri_mount_info_get (info, "domain");
      if (user) {
        if (domain)
          uri->userinfo = g_strconcat (domain, ";", user, NULL);
        else
          uri->userinfo = g_strdup (user);
      }
    }

  s = g_vfs_encode_uri (uri, allow_utf8);
  g_vfs_decoded_uri_free (uri);
  return s;
}

static const char *
smb_to_uri_scheme (GVfsUriMapper *mapper,
                   GVfsUriMountInfo *info)
{
  const gchar *type = g_vfs_uri_mount_info_get (info, "type");
  
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
