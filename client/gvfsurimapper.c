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

G_DEFINE_DYNAMIC_TYPE (GVfsUriMapper, g_vfs_uri_mapper, G_TYPE_OBJECT)

void
g_vfs_uri_mapper_register (GIOModule *module)
{
  g_vfs_uri_mapper_register_type (G_TYPE_MODULE (module));
}

GVfsUriMountInfo *
g_vfs_uri_mount_info_new (const char *type)
{
  GVfsUriMountInfo *info;

  info = g_new0 (GVfsUriMountInfo, 1);
  info->keys = g_array_new (TRUE, TRUE, sizeof (GVfsUriMountInfoKey));

  if (type != NULL)
    g_vfs_uri_mount_info_set (info, "type", type);
  
  return info;
}

void
g_vfs_uri_mount_info_free (GVfsUriMountInfo *info)
{
  int i;
  GVfsUriMountInfoKey *key;
    
  for (i = 0; i < info->keys->len; i++) {
    key = &g_array_index (info->keys, GVfsUriMountInfoKey, i);

    g_free (key->key);
    g_free (key->value);
  }
  g_array_free (info->keys, TRUE);  
  g_free (info->path);
  g_free (info);
}

static GVfsUriMountInfoKey *
lookup_key (GVfsUriMountInfo *info,
	    const char *key)
{
  int i;
  GVfsUriMountInfoKey *keyp;
    
  for (i = 0; i < info->keys->len; i++) {
    keyp = &g_array_index (info->keys, GVfsUriMountInfoKey, i);
    
    if (strcmp (keyp->key, key) == 0)
      return keyp;
  }

  return NULL;
}
  
const char *
g_vfs_uri_mount_info_get (GVfsUriMountInfo *info,
			  const char       *key)
{
  GVfsUriMountInfoKey *keyp;

  keyp = lookup_key (info, key);

  if (keyp)
    return keyp->value;
  
  return NULL;
}

void 
g_vfs_uri_mount_info_set_with_len (GVfsUriMountInfo *info,
				   const char *key,
				   const char *value,
				   int value_len)
{
  GVfsUriMountInfoKey *keyp;
  GVfsUriMountInfoKey keyv;
  char *value_copy;

  if (value_len == -1)
    value_copy = g_strdup (value);
  else
    value_copy = g_strndup (value, value_len);
  
  keyp = lookup_key (info, key);
  if (keyp)
    {
      g_free (keyp->value);
      keyp->value = value_copy;
    }
  else
    {
      keyv.key = g_strdup (key);
      keyv.value = value_copy;
      g_array_append_val (info->keys, keyv);
    }
}

void
g_vfs_uri_mount_info_set (GVfsUriMountInfo *info,
			  const char *key,
			  const char *value)
{
  g_vfs_uri_mount_info_set_with_len (info, key, value, -1);
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

  
GVfsUriMountInfo *
g_vfs_uri_mapper_from_uri (GVfsUriMapper  *mapper,
			   const char     *uri)
{
  GVfsUriMapperClass *class;

  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);

  return (* class->from_uri) (mapper, uri);
}

GVfsUriMountInfo *
g_vfs_uri_mapper_get_mount_info_for_path (GVfsUriMapper    *mapper,
					  GVfsUriMountInfo *info,
					  const char       *new_path)
{
  GVfsUriMapperClass *class;

  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);

  if (class->get_mount_info_for_path != NULL)
    return (* class->get_mount_info_for_path) (mapper, info, new_path);
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
			 GVfsUriMountInfo *mount_info,
			 gboolean allow_utf8)
{
  GVfsUriMapperClass *class;
  
  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);
  
  return (* class->to_uri) (mapper, mount_info, allow_utf8);
}

const char *
g_vfs_uri_mapper_to_uri_scheme (GVfsUriMapper  *mapper,
                                GVfsUriMountInfo *mount_info)
{
  GVfsUriMapperClass *class;
  
  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);
  
  return (* class->to_uri_scheme) (mapper, mount_info);
}
						       
