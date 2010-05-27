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
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsreadchannel.h"
#include "gvfsjobopenforread.h"
#include "gvfsdbusutils.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobOpenForRead, g_vfs_job_open_for_read, G_VFS_TYPE_JOB_DBUS)

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static void         finished     (GVfsJob        *job);
static DBusMessage *create_reply (GVfsJob        *job,
				  DBusConnection *connection,
				  DBusMessage    *message);

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

GVfsJob *
g_vfs_job_open_for_read_new (DBusConnection *connection,
			     DBusMessage *message,
			     GVfsBackend *backend)
{
  GVfsJobOpenForRead *job;
  DBusMessage *reply;
  DBusError derror;
  int path_len;
  const char *path_data;
  guint32 pid;
  
  dbus_error_init (&derror);
  if (!dbus_message_get_args (message, &derror, 
			      DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			      &path_data, &path_len,
                              DBUS_TYPE_UINT32, &pid,
			      0))
    {
      reply = dbus_message_new_error (message,
				      derror.name,
                                      derror.message);
      dbus_error_free (&derror);

      dbus_connection_send (connection, reply, NULL);
      return NULL;
    }

  job = g_object_new (G_VFS_TYPE_JOB_OPEN_FOR_READ,
		      "message", message,
		      "connection", connection,
		      NULL);

  job->filename = g_strndup (path_data, path_len);
  job->backend = backend;
  job->pid = pid;
  
  return G_VFS_JOB (job);
}

static void
run (GVfsJob *job)
{
  GVfsJobOpenForRead *op_job = G_VFS_JOB_OPEN_FOR_READ (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->open_for_read == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported by backend"));
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
static DBusMessage *
create_reply (GVfsJob *job,
	      DBusConnection *connection,
	      DBusMessage *message)
{
  GVfsJobOpenForRead *open_job = G_VFS_JOB_OPEN_FOR_READ (job);
  GVfsReadChannel *channel;
  DBusMessage *reply;
  GError *error;
  int remote_fd;
  int fd_id;
  dbus_bool_t can_seek;

  g_assert (open_job->backend_handle != NULL);

  error = NULL;
  channel = g_vfs_read_channel_new (open_job->backend,
                                    open_job->pid);

  remote_fd = g_vfs_channel_steal_remote_fd (G_VFS_CHANNEL (channel));
  if (!dbus_connection_send_fd (connection, 
				remote_fd,
				&fd_id, &error))
    {
      close (remote_fd);
      reply = _dbus_message_new_from_gerror (message, error);
      g_error_free (error);
      g_object_unref (channel);
      return reply;
    }
  close (remote_fd);

  reply = dbus_message_new_method_return (message);
  can_seek = open_job->can_seek;
  dbus_message_append_args (reply,
			    DBUS_TYPE_UINT32, &fd_id,
			    DBUS_TYPE_BOOLEAN, &can_seek,
			    DBUS_TYPE_INVALID);

  g_vfs_channel_set_backend_handle (G_VFS_CHANNEL (channel), open_job->backend_handle);
  open_job->backend_handle = NULL;
  open_job->read_channel = channel;

  g_signal_emit_by_name (job, "new-source", open_job->read_channel);
  
  return reply;
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
