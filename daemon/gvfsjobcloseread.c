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
#include "gvfsreadchannel.h"
#include "gvfsjobcloseread.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobCloseRead, g_vfs_job_close_read, G_VFS_TYPE_JOB)

static void run (GVfsJob *job);
static gboolean try (GVfsJob *job);
static void send_reply (GVfsJob *job);

static void
g_vfs_job_close_read_finalize (GObject *object)
{
  GVfsJobCloseRead *job;

  job = G_VFS_JOB_CLOSE_READ (object);
  g_object_unref (job->channel);

  if (G_OBJECT_CLASS (g_vfs_job_close_read_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_close_read_parent_class)->finalize) (object);
}

static void
g_vfs_job_close_read_class_init (GVfsJobCloseReadClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_close_read_finalize;

  job_class->run = run;
  job_class->try = try;
  job_class->send_reply = send_reply;
}

static void
g_vfs_job_close_read_init (GVfsJobCloseRead *job)
{
}

GVfsJob *
g_vfs_job_close_read_new (GVfsReadChannel *channel,
			  GVfsBackendHandle handle,
			  GVfsBackend *backend)
{
  GVfsJobCloseRead *job;
  
  job = g_object_new (G_VFS_TYPE_JOB_CLOSE_READ,
		      NULL);

  job->channel = g_object_ref (channel);
  job->backend = backend;
  job->handle = handle;
  
  return G_VFS_JOB (job);
}

/* Might be called on an i/o thread */
static void
send_reply (GVfsJob *job)
{
  GVfsJobCloseRead *op_job = G_VFS_JOB_CLOSE_READ (job);
  
  g_debug ("send_reply(%p), failed=%d (%s)\n", job, job->failed,
           job->failed ? job->error->message : "");

  if (job->failed)
    g_vfs_channel_send_error (G_VFS_CHANNEL (op_job->channel), job->error);
  else
    g_vfs_read_channel_send_closed (op_job->channel);
}

static void
run (GVfsJob *job)
{
  GVfsJobCloseRead *op_job = G_VFS_JOB_CLOSE_READ (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->close_read == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported"));
      return;
    }

  class->close_read (op_job->backend,
		     op_job,
		     op_job->handle);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobCloseRead *op_job = G_VFS_JOB_CLOSE_READ (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  
  if (class->try_close_read == NULL)
    return FALSE;
  
  return class->try_close_read (op_job->backend,
				op_job,
				op_job->handle);
}
