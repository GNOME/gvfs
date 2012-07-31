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

#ifndef __G_VFS_JOB_QUERY_ATTRIBUTES_H__
#define __G_VFS_JOB_QUERY_ATTRIBUTES_H__

#include <gio/gio.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_QUERY_ATTRIBUTES         (g_vfs_job_query_attributes_get_type ())
#define G_VFS_JOB_QUERY_ATTRIBUTES(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_QUERY_ATTRIBUTES, GVfsJobQueryAttributes))
#define G_VFS_JOB_QUERY_ATTRIBUTES_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_QUERY_ATTRIBUTES, GVfsJobQueryAttributesClass))
#define G_VFS_IS_JOB_QUERY_ATTRIBUTES(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_QUERY_ATTRIBUTES))
#define G_VFS_IS_JOB_QUERY_ATTRIBUTES_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_QUERY_ATTRIBUTES))
#define G_VFS_JOB_QUERY_ATTRIBUTES_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_QUERY_ATTRIBUTES, GVfsJobQueryAttributesClass))

typedef struct _GVfsJobQueryAttributesClass   GVfsJobQueryAttributesClass;

struct _GVfsJobQueryAttributes
{
  GVfsJobDBus parent_instance;

  GVfsBackend *backend;

  char *filename;
  gboolean namespaces;
  
  GFileAttributeInfoList *list;
};

struct _GVfsJobQueryAttributesClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_query_attributes_get_type (void) G_GNUC_CONST;

gboolean g_vfs_job_query_settable_attributes_new_handle (GVfsDBusMount         *object,
                                                         GDBusMethodInvocation *invocation,
                                                         const gchar           *arg_path_data,
                                                         GVfsBackend           *backend);
gboolean g_vfs_job_query_writable_namespaces_new_handle (GVfsDBusMount         *object,
                                                         GDBusMethodInvocation *invocation,
                                                         const gchar           *arg_path_data,
                                                         GVfsBackend           *backend);
void     g_vfs_job_query_attributes_set_list (GVfsJobQueryAttributes *job,
					      GFileAttributeInfoList *list);

G_END_DECLS

#endif /* __G_VFS_JOB_QUERY_ATTRIBUTES_H__ */
