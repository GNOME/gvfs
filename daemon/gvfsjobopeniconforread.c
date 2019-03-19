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

#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gunixfdlist.h>
#include "gvfsreadchannel.h"
#include "gvfsjobopeniconforread.h"

G_DEFINE_TYPE (GVfsJobOpenIconForRead, g_vfs_job_open_icon_for_read, G_VFS_TYPE_JOB_OPEN_FOR_READ)

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);

static void
g_vfs_job_open_icon_for_read_finalize (GObject *object)
{
  if (G_OBJECT_CLASS (g_vfs_job_open_icon_for_read_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_open_icon_for_read_parent_class)->finalize) (object);
}

static void
g_vfs_job_open_icon_for_read_class_init (GVfsJobOpenIconForReadClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);

  gobject_class->finalize = g_vfs_job_open_icon_for_read_finalize;
  job_class->run = run;
  job_class->try = try;
}

static void
g_vfs_job_open_icon_for_read_init (GVfsJobOpenIconForRead *job)
{
}

gboolean
g_vfs_job_open_icon_for_read_new_handle (GVfsDBusMount *object,
                                         GDBusMethodInvocation *invocation,
                                         GUnixFDList *fd_list,
                                         const gchar *arg_path_data,
                                         GVfsBackend *backend)
{
  GVfsJobOpenIconForRead *job;
  GVfsJobOpenForRead *job_open_for_read;

  if (g_vfs_backend_invocation_first_handler (object, invocation, backend))
    return TRUE;
  
  job = g_object_new (G_VFS_TYPE_JOB_OPEN_ICON_FOR_READ,
                      "object", object,
                      "invocation", invocation,
                      NULL);
  
  job_open_for_read = G_VFS_JOB_OPEN_FOR_READ (job);

  job->icon_id = g_strdup (arg_path_data);
  job_open_for_read->backend = backend;
  job_open_for_read->read_icon = TRUE;

  g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), G_VFS_JOB (job));
  g_object_unref (job);

  return TRUE;
}

static void
run (GVfsJob *job)
{
  GVfsJobOpenIconForRead *op_job = G_VFS_JOB_OPEN_ICON_FOR_READ (job);
  GVfsJobOpenForRead *op_job_open_for_read = G_VFS_JOB_OPEN_FOR_READ (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job_open_for_read->backend);

  if (class->open_icon_for_read == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported"));
      return;
    }

  class->open_icon_for_read (op_job_open_for_read->backend,
                             op_job,
                             op_job->icon_id);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobOpenIconForRead *op_job = G_VFS_JOB_OPEN_ICON_FOR_READ (job);
  GVfsJobOpenForRead *op_job_open_for_read = G_VFS_JOB_OPEN_FOR_READ (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job_open_for_read->backend);

  if (class->try_open_icon_for_read == NULL)
    return FALSE;

  return class->try_open_icon_for_read (op_job_open_for_read->backend,
                                        op_job,
                                        op_job->icon_id);
}
