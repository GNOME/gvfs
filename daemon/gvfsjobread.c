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

#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gi18n.h>
#include "gvfsreadchannel.h"
#include "gvfsjobread.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobRead, g_vfs_job_read, G_VFS_TYPE_JOB)

static void     run        (GVfsJob *job);
static gboolean try        (GVfsJob *job);
static void     send_reply (GVfsJob *job);

static void
g_vfs_job_read_finalize (GObject *object)
{
  GVfsJobRead *job;

  job = G_VFS_JOB_READ (object);

  g_object_unref (job->channel);
  g_free (job->buffer);
  
  if (G_OBJECT_CLASS (g_vfs_job_read_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_read_parent_class)->finalize) (object);
}

static void
g_vfs_job_read_class_init (GVfsJobReadClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_read_finalize;

  job_class->run = run;
  job_class->try = try;
  job_class->send_reply = send_reply;
}

static void
g_vfs_job_read_init (GVfsJobRead *job)
{
}

GVfsJob *
g_vfs_job_read_new (GVfsReadChannel *channel,
		    GVfsBackendHandle handle,
		    gsize bytes_requested,
		    GVfsBackend *backend)
{
  GVfsJobRead *job;
  
  job = g_object_new (G_VFS_TYPE_JOB_READ,
		      NULL);

  job->backend = backend;
  job->channel = g_object_ref (channel);
  job->handle = handle;
  job->buffer = g_malloc (bytes_requested);
  job->bytes_requested = bytes_requested;
  
  return G_VFS_JOB (job);
}

/* Might be called on an i/o thread */
static void
send_reply (GVfsJob *job)
{
  GVfsJobRead *op_job = G_VFS_JOB_READ (job);
  g_debug ("job_read send reply, %"G_GSIZE_FORMAT" bytes\n", op_job->data_count);

  if (job->failed)
    g_vfs_channel_send_error (G_VFS_CHANNEL (op_job->channel), job->error);
  else
    {
      g_vfs_read_channel_send_data (op_job->channel,
				    op_job->buffer,
				    op_job->data_count);
    }
}

static void
run (GVfsJob *job)
{
  GVfsJobRead *op_job = G_VFS_JOB_READ (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->read == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported by backend"));
      return;
    }
      
  class->read (op_job->backend,
	       op_job,
	       op_job->handle,
	       op_job->buffer,
	       op_job->bytes_requested);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobRead *op_job = G_VFS_JOB_READ (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_read == NULL)
    return FALSE;

  return class->try_read (op_job->backend,
			  op_job,
			  op_job->handle,
			  op_job->buffer,
			  op_job->bytes_requested);
}


/* Takes ownership */
void
g_vfs_job_read_set_size (GVfsJobRead *job,
			 gsize data_size)
{
  job->data_count = data_size;
}
