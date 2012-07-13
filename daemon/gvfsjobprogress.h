/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2012 Red Hat, Inc.
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
 * Author: Tomas Bzatek <tbzatek@redhat.com>
 */

#ifndef __G_VFS_JOB_PROGRESS_H__
#define __G_VFS_JOB_PROGRESS_H__

#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_PROGRESS         (g_vfs_job_progress_get_type ())
#define G_VFS_JOB_PROGRESS(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_PROGRESS, GVfsJobProgress))
#define G_VFS_JOB_PROGRESS_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_PROGRESS, GVfsJobProgressClass))
#define G_VFS_IS_JOB_PROGRESS(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_PROGRESS))
#define G_VFS_IS_JOB_PROGRESS_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_PROGRESS))
#define G_VFS_JOB_PROGRESS_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_PROGRESS, GVfsJobProgressClass))

typedef struct _GVfsJobProgress        GVfsJobProgress;
typedef struct _GVfsJobProgressClass   GVfsJobProgressClass;

struct _GVfsJobProgress
{
  GVfsJobDBus parent_instance;

  gboolean send_progress;
  char *callback_obj_path;
  GVfsDBusProgress *progress_proxy;
};

struct _GVfsJobProgressClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_progress_get_type (void) G_GNUC_CONST;

void g_vfs_job_progress_callback (goffset current_num_bytes,
                                  goffset total_num_bytes,
                                  gpointer user_data);

void g_vfs_job_progress_construct_proxy (GVfsJob *job);


G_END_DECLS

#endif /* __G_VFS_JOB_PROGRESS_H__ */
