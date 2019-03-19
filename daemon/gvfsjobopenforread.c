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
#include "gvfsjobopenforread.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobOpenForRead, g_vfs_job_open_for_read, G_VFS_TYPE_JOB_DBUS)

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static void         finished     (GVfsJob        *job);
static void         create_reply (GVfsJob               *job,
                                  GVfsDBusMount         *object,
                                  GDBusMethodInvocation *invocation);

static void
g_vfs_job_open_for_read_finalize (GObject *object)
{
  GVfsJobOpenForRead *job;

  job = G_VFS_JOB_OPEN_FOR_READ (object);

  /* TODO: manage backend_handle if not put in read channel */

  if (job->read_channel)
    g_object_unref (job->read_channel);
  
  g_free (job->filename);
  
  if (G_OBJECT_CLASS (g_vfs_job_open_for_read_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_open_for_read_parent_class)->finalize) (object);
}

static void
g_vfs_job_open_for_read_class_init (GVfsJobOpenForReadClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_open_for_read_finalize;
  job_class->run = run;
  job_class->try = try;
  job_class->finished = finished;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_open_for_read_init (GVfsJobOpenForRead *job)
{
}

gboolean
g_vfs_job_open_for_read_new_handle (GVfsDBusMount *object,
                                    GDBusMethodInvocation *invocation,
                                    GUnixFDList *fd_list,
                                    const gchar *arg_path_data,
                                    guint arg_pid,
                                    GVfsBackend *backend)
{
  GVfsJobOpenForRead *job;

  if (g_vfs_backend_invocation_first_handler (object, invocation, backend))
    return TRUE;
  
  job = g_object_new (G_VFS_TYPE_JOB_OPEN_FOR_READ,
                      "object", object,
                      "invocation", invocation,
                      NULL);
  
  job->filename = g_strdup (arg_path_data);
  job->backend = backend;
  job->pid = arg_pid;

  g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), G_VFS_JOB (job));
  g_object_unref (job);

  return TRUE;
}

static void
run (GVfsJob *job)
{
  GVfsJobOpenForRead *op_job = G_VFS_JOB_OPEN_FOR_READ (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->open_for_read == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported"));
      return;
    }
  
  class->open_for_read (op_job->backend,
			op_job,
			op_job->filename);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobOpenForRead *op_job = G_VFS_JOB_OPEN_FOR_READ (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_open_for_read == NULL)
    return FALSE;
  
  return class->try_open_for_read (op_job->backend,
				   op_job,
				   op_job->filename);
}


void
g_vfs_job_open_for_read_set_handle (GVfsJobOpenForRead *job,
				    GVfsBackendHandle handle)
{
  job->backend_handle = handle;
}

void
g_vfs_job_open_for_read_set_can_seek (GVfsJobOpenForRead *job,
				      gboolean            can_seek)
{
  job->can_seek = can_seek;
}

/* Might be called on an i/o thread */
static void
create_reply (GVfsJob *job,
              GVfsDBusMount *object,
              GDBusMethodInvocation *invocation)
{
  GVfsJobOpenForRead *open_job = G_VFS_JOB_OPEN_FOR_READ (job);
  GVfsReadChannel *channel;
  GError *error;
  int remote_fd;
  int fd_id;
  GUnixFDList *fd_list;

  g_assert (open_job->backend_handle != NULL);

  channel = g_vfs_read_channel_new (open_job->backend,
                                    open_job->pid);

  remote_fd = g_vfs_channel_steal_remote_fd (G_VFS_CHANNEL (channel));
  if (remote_fd < 0)
    {
      /* expecting we're out of fds when remote_fd == -1 */
      g_dbus_method_invocation_return_error_literal (invocation,
                                                     G_IO_ERROR,
                                                     G_IO_ERROR_TOO_MANY_OPEN_FILES,
                                                     _("Couldn’t get stream file descriptor"));
      g_object_unref (channel);
      return;
    }

  fd_list = g_unix_fd_list_new ();
  error = NULL;
  fd_id = g_unix_fd_list_append (fd_list, remote_fd, &error);
  if (fd_id == -1)
    {
      g_warning ("create_reply: %s (%s, %d)\n", error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }

  g_vfs_channel_set_backend_handle (G_VFS_CHANNEL (channel), open_job->backend_handle);
  open_job->backend_handle = NULL;
  open_job->read_channel = channel;

  g_signal_emit_by_name (job, "new-source", channel);

  if (open_job->read_icon)
    gvfs_dbus_mount_complete_open_icon_for_read (object, invocation,
                                                 fd_list, g_variant_new_handle (fd_id),
                                                 open_job->can_seek);
  else
    gvfs_dbus_mount_complete_open_for_read (object, invocation,
                                            fd_list, g_variant_new_handle (fd_id),
                                            open_job->can_seek);
  
  /* FIXME: this could cause issues as long as fd_list closes all its fd's when it's finalized */
  close (remote_fd);
  g_object_unref (fd_list);
}

static void
finished (GVfsJob *job)
{
}

GPid
g_vfs_job_open_for_read_get_pid (GVfsJobOpenForRead *job)
{
  return job->pid;
}
