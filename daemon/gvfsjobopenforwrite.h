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

#ifndef __G_VFS_JOB_OPEN_FOR_WRITE_H__
#define __G_VFS_JOB_OPEN_FOR_WRITE_H__

#include <gvfsjobdbus.h>
#include <gvfsbackend.h>
#include <gvfswritechannel.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_OPEN_FOR_WRITE         (g_vfs_job_open_for_write_get_type ())
#define G_VFS_JOB_OPEN_FOR_WRITE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_OPEN_FOR_WRITE, GVfsJobOpenForWrite))
#define G_VFS_JOB_OPEN_FOR_WRITE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_OPEN_FOR_WRITE, GVfsJobOpenForWriteClass))
#define G_VFS_IS_JOB_OPEN_FOR_WRITE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_OPEN_FOR_WRITE))
#define G_VFS_IS_JOB_OPEN_FOR_WRITE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_OPEN_FOR_WRITE))
#define G_VFS_JOB_OPEN_FOR_WRITE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_OPEN_FOR_WRITE, GVfsJobOpenForWriteClass))

typedef struct _GVfsJobOpenForWriteClass   GVfsJobOpenForWriteClass;

typedef enum {
  OPEN_FOR_WRITE_CREATE = 0,
  OPEN_FOR_WRITE_APPEND = 1,
  OPEN_FOR_WRITE_REPLACE = 2,
  OPEN_FOR_WRITE_EDIT = 3
} GVfsJobOpenForWriteMode;

typedef enum {
  OPEN_FOR_WRITE_VERSION_ORIGINAL,
  OPEN_FOR_WRITE_VERSION_WITH_FLAGS,
} GVfsJobOpenForWriteVersion;

struct _GVfsJobOpenForWrite
{
  GVfsJobDBus parent_instance;

  GVfsJobOpenForWriteMode mode;
  char *filename;
  char *etag;
  gboolean make_backup;
  GFileCreateFlags flags;
  
  GVfsBackend *backend;
  GVfsBackendHandle backend_handle;

  guint can_seek : 1;
  guint can_truncate : 1;
  goffset initial_offset;
  GVfsWriteChannel *write_channel;

  GPid pid;

  GVfsJobOpenForWriteVersion version;
};

struct _GVfsJobOpenForWriteClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_open_for_write_get_type (void) G_GNUC_CONST;

gboolean g_vfs_job_open_for_write_new_handle         (GVfsDBusMount         *object,
                                                      GDBusMethodInvocation *invocation,
                                                      GUnixFDList           *fd_list,
                                                      const gchar           *arg_path_data,
                                                      guint16                arg_mode,
                                                      const gchar           *arg_etag,
                                                      gboolean               arg_make_backup,
                                                      guint                  arg_flags,
                                                      guint                  arg_pid,
                                                      GVfsBackend           *backend);
gboolean g_vfs_job_open_for_write_new_handle_with_flags (GVfsDBusMount         *object,
                                                         GDBusMethodInvocation *invocation,
                                                         GUnixFDList           *fd_list,
                                                         const gchar           *arg_path_data,
                                                         guint16                arg_mode,
                                                         const gchar           *arg_etag,
                                                         gboolean               arg_make_backup,
                                                         guint                  arg_flags,
                                                         guint                  arg_pid,
                                                         GVfsBackend           *backend);
void     g_vfs_job_open_for_write_set_handle         (GVfsJobOpenForWrite *job,
						      GVfsBackendHandle    handle);
void     g_vfs_job_open_for_write_set_can_seek       (GVfsJobOpenForWrite *job,
						      gboolean             can_seek);
void     g_vfs_job_open_for_write_set_can_truncate   (GVfsJobOpenForWrite *job,
                                                      gboolean             can_truncate);
void     g_vfs_job_open_for_write_set_initial_offset (GVfsJobOpenForWrite *job,
						      goffset              initial_offset);
GPid     g_vfs_job_open_for_write_get_pid            (GVfsJobOpenForWrite *job);

G_END_DECLS

#endif /* __G_VFS_JOB_OPEN_FOR_WRITE_H__ */
