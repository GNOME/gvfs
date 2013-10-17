/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2013 Ross Lagerwall
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
 */

#ifndef __G_VFS_JOB_TRUNCATE_H__
#define __G_VFS_JOB_TRUNCATE_H__

#include <gvfsjob.h>
#include <gvfsbackend.h>
#include <gvfswritechannel.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_TRUNCATE         (g_vfs_job_truncate_get_type ())
#define G_VFS_JOB_TRUNCATE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_TRUNCATE, GVfsJobTruncate))
#define G_VFS_JOB_TRUNCATE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_TRUNCATE, GVfsJobTruncateClass))
#define G_VFS_IS_JOB_TRUNCATE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_TRUNCATE))
#define G_VFS_IS_JOB_TRUNCATE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_TRUNCATE))
#define G_VFS_JOB_TRUNCATE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_TRUNCATE, GVfsJobTruncateClass))

typedef struct _GVfsJobTruncateClass   GVfsJobTruncateClass;

struct _GVfsJobTruncate
{
  GVfsJob parent_instance;

  GVfsWriteChannel *channel;
  GVfsBackend *backend;
  GVfsBackendHandle handle;
  goffset size;
};

struct _GVfsJobTruncateClass
{
  GVfsJobClass parent_class;
};

GType g_vfs_job_truncate_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_truncate_new (GVfsWriteChannel  *channel,
                                 GVfsBackendHandle  handle,
                                 goffset            size,
                                 GVfsBackend       *backend);

G_END_DECLS

#endif /* __G_VFS_JOB_TRUNCATE_H__ */
