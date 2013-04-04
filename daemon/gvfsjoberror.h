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

#ifndef __G_VFS_JOB_ERROR_H__
#define __G_VFS_JOB_ERROR_H__

#include <gvfsjob.h>
#include <gvfsbackend.h>
#include <gvfschannel.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_ERROR         (g_vfs_job_error_get_type ())
#define G_VFS_JOB_ERROR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_ERROR, GVfsJobError))
#define G_VFS_JOB_ERROR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_ERROR, GVfsJobErrorClass))
#define G_VFS_IS_JOB_ERROR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_ERROR))
#define G_VFS_IS_JOB_ERROR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_ERROR))
#define G_VFS_JOB_ERROR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_ERROR, GVfsJobErrorClass))

typedef struct _GVfsJobErrorClass   GVfsJobErrorClass;

struct _GVfsJobError
{
  GVfsJob parent_instance;

  GVfsChannel *channel;
  GError *error;
};

struct _GVfsJobErrorClass
{
  GVfsJobClass parent_class;
};

GType g_vfs_job_error_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_error_new (GVfsChannel   *channel,
			      GError *error);

G_END_DECLS

#endif /* __G_VFS_JOB_ERROR_H__ */
