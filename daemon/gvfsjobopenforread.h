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

#ifndef __G_VFS_JOB_OPEN_FOR_READ_H__
#define __G_VFS_JOB_OPEN_FOR_READ_H__

#include <gvfsjobdbus.h>
#include <gvfsbackend.h>
#include <gvfsreadchannel.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_OPEN_FOR_READ         (g_vfs_job_open_for_read_get_type ())
#define G_VFS_JOB_OPEN_FOR_READ(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_OPEN_FOR_READ, GVfsJobOpenForRead))
#define G_VFS_JOB_OPEN_FOR_READ_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_OPEN_FOR_READ, GVfsJobOpenForReadClass))
#define G_VFS_IS_JOB_OPEN_FOR_READ(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_OPEN_FOR_READ))
#define G_VFS_IS_JOB_OPEN_FOR_READ_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_OPEN_FOR_READ))
#define G_VFS_JOB_OPEN_FOR_READ_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_OPEN_FOR_READ, GVfsJobOpenForReadClass))

typedef struct _GVfsJobOpenForReadClass   GVfsJobOpenForReadClass;

struct _GVfsJobOpenForRead
{
  GVfsJobDBus parent_instance;

  char *filename;
  GVfsBackend *backend;
  GVfsBackendHandle backend_handle;
  gboolean can_seek;
  GVfsReadChannel *read_channel;
  gboolean read_icon;

  GPid pid;
};

struct _GVfsJobOpenForReadClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_open_for_read_get_type (void) G_GNUC_CONST;

gboolean         g_vfs_job_open_for_read_new_handle    (GVfsDBusMount         *object,
                                                        GDBusMethodInvocation *invocation,
                                                        GUnixFDList           *fd_list,
                                                        const gchar           *arg_path_data,
                                                        guint                  arg_pid,
                                                        GVfsBackend           *backend);
void             g_vfs_job_open_for_read_set_handle    (GVfsJobOpenForRead *job,
							GVfsBackendHandle   handle);
void             g_vfs_job_open_for_read_set_can_seek  (GVfsJobOpenForRead *job,
							gboolean            can_seek);
GPid             g_vfs_job_open_for_read_get_pid       (GVfsJobOpenForRead *job);

G_END_DECLS

#endif /* __G_VFS_JOB_OPEN_FOR_READ_H__ */
