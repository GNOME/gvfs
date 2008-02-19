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
#include <stdlib.h>

#include <gio/gio.h>
#include <gvfsurimapper.h>
#include <gvfsuriutils.h>

typedef struct _GVfsUriMapperSftp GVfsUriMapperSftp;
typedef struct _GVfsUriMapperSftpClass GVfsUriMapperSftpClass;

struct _GVfsUriMapperSftp
{
  GVfsUriMapper parent;
};

struct _GVfsUriMapperSftpClass
{
  GVfsUriMapperClass parent_class;
};

GType g_vfs_uri_mapper_sftp_get_type (void);
void g_vfs_uri_mapper_sftp_register (GIOModule *module);

G_DEFINE_DYNAMIC_TYPE (GVfsUriMapperSftp, g_vfs_uri_mapper_sftp, G_VFS_TYPE_URI_MAPPER)

static void
g_vfs_uri_mapper_sftp_init (GVfsUriMapperSftp *vfs)
{
}

static const char * const *
sftp_get_handled_schemes (GVfsUriMapper *mapper)
{
  static const char *schemes[] = {
    "sftp",
    "ssh",
    NULL
  };
  return schemes;
}

static GVfsUriMountInfo *
sftp_from_uri (GVfsUriMapper *mapper,
	       const char *uri_str)
{
  GDecodedUri *uri;
  GVfsUriMountInfo *info;

  uri = g_vfs_decode_uri (uri_str);
  if (uri == NULL)
    return NULL;

  info = g_vfs_uri_mount_info_new ("sftp");
  
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
  
  info->path = g_strdup (uri->path);
  
  g_vfs_decoded_uri_free (uri);

  return info;
}

static const char * const *
sftp_get_handled_mount_types (GVfsUriMapper *mapper)
{
  static const char *types[] = {
    "sftp", 
    NULL
  };
  return types;
}

static char *
sftp_to_uri (GVfsUriMapper *mapper,
	     GVfsUriMountInfo *info,
	     gboolean allow_utf8)
{
  GDecodedUri uri;
  const char *port;

  memset (&uri, 0, sizeof (uri));
  uri.port = -1;

  uri.scheme = "sftp";
  uri.host = (char *)g_vfs_uri_mount_info_get (info, "host");
  uri.userinfo = (char *)g_vfs_uri_mount_info_get (info, "user");
  
  port = g_vfs_uri_mount_info_get (info, "port");
  if (port != NULL)
    uri.port = atoi (port);

  if (info->path == NULL)
    uri.path = "/";
  else
    uri.path = info->path;
  
  return g_vfs_encode_uri (&uri, allow_utf8);
}

static const char *
sftp_to_uri_scheme (GVfsUriMapper *mapper,
		    GVfsUriMountInfo *info)
{
  return "sftp";
}

static void
g_vfs_uri_mapper_sftp_class_finalize (GVfsUriMapperSftpClass *klass)
{
}

static void
g_vfs_uri_mapper_sftp_class_init (GVfsUriMapperSftpClass *class)
{
  GVfsUriMapperClass *mapper_class;
  
  mapper_class = G_VFS_URI_MAPPER_CLASS (class);
  mapper_class->get_handled_schemes = sftp_get_handled_schemes;
  mapper_class->from_uri = sftp_from_uri;
  mapper_class->get_handled_mount_types = sftp_get_handled_mount_types;
  mapper_class->to_uri = sftp_to_uri;
  mapper_class->to_uri_scheme = sftp_to_uri_scheme;
}

void
g_vfs_uri_mapper_sftp_register (GIOModule *module)
{
  g_vfs_uri_mapper_sftp_register_type (G_TYPE_MODULE (module));
}
