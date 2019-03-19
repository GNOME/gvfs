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
#include "gvfswritechannel.h"
#include "gvfsjobwrite.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobWrite, g_vfs_job_write, G_VFS_TYPE_JOB)

static void     run        (GVfsJob *job);
static gboolean try        (GVfsJob *job);
static void     send_reply (GVfsJob *job);

static void
g_vfs_job_write_finalize (GObject *object)
{
  GVfsJobWrite *job;

  job = G_VFS_JOB_WRITE (object);

  g_object_unref (job->channel);
  g_free (job->data);
  
  if (G_OBJECT_CLASS (g_vfs_job_write_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_write_parent_class)->finalize) (object);
}

static void
g_vfs_job_write_class_init (GVfsJobWriteClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_write_finalize;

  job_class->run = run;
  job_class->try = try;
  job_class->send_reply = send_reply;
}

static void
g_vfs_job_write_init (GVfsJobWrite *job)
{
}

GVfsJob *
g_vfs_job_write_new (GVfsWriteChannel *channel,
		     GVfsBackendHandle handle,
		     char *data,
		     gsize data_size,
		     GVfsBackend *backend)
{
  GVfsJobWrite *job;
  
  job = g_object_new (G_VFS_TYPE_JOB_WRITE,
		      NULL);

  job->backend = backend;
  job->channel = g_object_ref (channel);
  job->handle = handle;
  /* Takes ownership */
  job->data = data;
  job->data_size = data_size;
  job->written_size = 0;
  
  return G_VFS_JOB (job);
}

/* Might be called on an i/o thwrite */
static void
send_reply (GVfsJob *job)
{
  GVfsJobWrite *op_job = G_VFS_JOB_WRITE (job);

  g_debug ("send_reply(%p), bytes=%"G_GSIZE_FORMAT", failed=%d (%s)\n",
           job, op_job->written_size,
           job->failed, job->failed ? job->error->message : "");

  if (job->failed)
    g_vfs_channel_send_error (G_VFS_CHANNEL (op_job->channel), job->error);
  else
    g_vfs_write_channel_send_written (op_job->channel,
				      op_job->written_size);
}

static void
run (GVfsJob *job)
{
  GVfsJobWrite *op_job = G_VFS_JOB_WRITE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->write == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported"));
      return;
    }
      
  class->write (op_job->backend,
		op_job,
		op_job->handle,
		op_job->data,
		op_job->data_size);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobWrite *op_job = G_VFS_JOB_WRITE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_write == NULL)
    return FALSE;

  return class->try_write (op_job->backend,
			   op_job,
			   op_job->handle,
			   op_job->data,
			   op_job->data_size);
}


void
g_vfs_job_write_set_written_size (GVfsJobWrite *job,
				  gsize written_size)
{
  job->written_size = written_size;
}
