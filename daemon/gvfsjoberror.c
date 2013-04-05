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
#include "gvfsjoberror.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobError, g_vfs_job_error, G_VFS_TYPE_JOB)

static void     run        (GVfsJob *job);
static gboolean try        (GVfsJob *job);
static void     send_reply (GVfsJob *job);

static void
g_vfs_job_error_finalize (GObject *object)
{
  GVfsJobError *job;

  job = G_VFS_JOB_ERROR (object);
  g_object_unref (job->channel);
  g_error_free (job->error);

  if (G_OBJECT_CLASS (g_vfs_job_error_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_error_parent_class)->finalize) (object);
}

static void
g_vfs_job_error_class_init (GVfsJobErrorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_error_finalize;

  job_class->run = run;
  job_class->try = try;
  job_class->send_reply = send_reply;
}

static void
g_vfs_job_error_init (GVfsJobError *job)
{
}

GVfsJob *
g_vfs_job_error_new (GVfsChannel *channel,
		     GError *error)
{
  GVfsJobError *job;

  job = g_object_new (G_VFS_TYPE_JOB_ERROR,
		      NULL);
  job->channel = g_object_ref (channel);
  job->error = g_error_copy (error);

  return G_VFS_JOB (job);
}

/* Might be called on an i/o thread */
static void
send_reply (GVfsJob *job)
{
  GVfsJobError *op_job = G_VFS_JOB_ERROR (job);

  g_assert (job->failed);

  g_vfs_channel_send_error (G_VFS_CHANNEL (op_job->channel), job->error);
}

static void
run (GVfsJob *job)
{
}

static gboolean
try (GVfsJob *job)
{
  g_vfs_job_failed_from_error (job, G_VFS_JOB_ERROR (job)->error);
  return TRUE;
}
