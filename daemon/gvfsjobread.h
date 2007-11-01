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

#ifndef __G_VFS_JOB_READ_H__
#define __G_VFS_JOB_READ_H__

#include <gvfsjob.h>
#include <gvfsbackend.h>
#include <gvfsreadchannel.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_READ         (g_vfs_job_read_get_type ())
#define G_VFS_JOB_READ(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_READ, GVfsJobRead))
#define G_VFS_JOB_READ_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_READ, GVfsJobReadClass))
#define G_VFS_IS_JOB_READ(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_READ))
#define G_VFS_IS_JOB_READ_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_READ))
#define G_VFS_JOB_READ_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_READ, GVfsJobReadClass))

typedef struct _GVfsJobReadClass   GVfsJobReadClass;

struct _GVfsJobRead
{
  GVfsJob parent_instance;

  GVfsReadChannel *channel;
  GVfsBackend *backend;
  GVfsBackendHandle handle;
  gsize bytes_requested;
  char *buffer;
  gsize data_count;
};

struct _GVfsJobReadClass
{
  GVfsJobClass parent_class;
};

GType g_vfs_job_read_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_read_new        (GVfsReadChannel   *channel,
				    GVfsBackendHandle  handle,
				    gsize              bytes_requested,
				    GVfsBackend       *backend);
void     g_vfs_job_read_set_size   (GVfsJobRead       *job,
				    gsize              data_size);

G_END_DECLS

#endif /* __G_VFS_JOB_READ_H__ */
