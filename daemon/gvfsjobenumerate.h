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

#ifndef __G_VFS_JOB_ENUMERATE_H__
#define __G_VFS_JOB_ENUMERATE_H__

#include <gio/gio.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_ENUMERATE         (g_vfs_job_enumerate_get_type ())
#define G_VFS_JOB_ENUMERATE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_ENUMERATE, GVfsJobEnumerate))
#define G_VFS_JOB_ENUMERATE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_ENUMERATE, GVfsJobEnumerateClass))
#define G_VFS_IS_JOB_ENUMERATE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_ENUMERATE))
#define G_VFS_IS_JOB_ENUMERATE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_ENUMERATE))
#define G_VFS_JOB_ENUMERATE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_ENUMERATE, GVfsJobEnumerateClass))

typedef struct _GVfsJobEnumerateClass   GVfsJobEnumerateClass;

struct _GVfsJobEnumerate
{
  GVfsJobDBus parent_instance;

  GVfsBackend *backend;
  char *filename;
  char *object_path;
  char *attributes;
  GFileAttributeMatcher *attribute_matcher;
  GFileQueryInfoFlags flags;
  char *uri;

  GVariantBuilder *building_infos;
  int n_building_infos;
};

struct _GVfsJobEnumerateClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_enumerate_get_type (void) G_GNUC_CONST;

gboolean g_vfs_job_enumerate_new_handle (GVfsDBusMount         *object,
                                         GDBusMethodInvocation *invocation,
                                         const gchar           *arg_path_data,
                                         const gchar           *arg_obj_path,
                                         const gchar           *arg_attributes,
                                         guint                  arg_flags,
                                         const gchar           *arg_uri,
                                         GVfsBackend           *backend);

void     g_vfs_job_enumerate_add_info   (GVfsJobEnumerate      *job,
					 GFileInfo             *info);
void     g_vfs_job_enumerate_add_infos  (GVfsJobEnumerate      *job,
					 const GList           *info);
void     g_vfs_job_enumerate_done       (GVfsJobEnumerate      *job);

G_END_DECLS

#endif /* __G_VFS_JOB_ENUMERATE_H__ */
