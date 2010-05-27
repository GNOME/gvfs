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
#include "gvfswritechannel.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsdbusutils.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobOpenForWrite, g_vfs_job_open_for_write, G_VFS_TYPE_JOB_DBUS)

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static void         finished     (GVfsJob        *job);
static DBusMessage *create_reply (GVfsJob        *job,
				  DBusConnection *connection,
				  DBusMessage    *message);

static void
g_vfs_job_open_for_write_finalize (GObject *object)
{
  GVfsJobOpenForWrite *job;

  job = G_VFS_JOB_OPEN_FOR_WRITE (object);

  /* TODO: manage backend_handle if not put in write channel */

  if (job->write_channel)
    g_object_unref (job->write_channel);
  
  g_free (job->filename);
  g_free (job->etag);
  
  if (G_OBJECT_CLASS (g_vfs_job_open_for_write_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_open_for_write_parent_class)->finalize) (object);
}

static void
g_vfs_job_open_for_write_class_init (GVfsJobOpenForWriteClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_open_for_write_finalize;
  job_class->run = run;
  job_class->try = try;
  job_class->finished = finished;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_open_for_write_init (GVfsJobOpenForWrite *job)
{
}

GVfsJob *
g_vfs_job_open_for_write_new (DBusConnection *connection,
			      DBusMessage *message,
			      GVfsBackend *backend)
{
  GVfsJobOpenForWrite *job;
  DBusMessageIter iter;
  DBusMessage *reply;
  DBusError derror;
  char *path;
  guint16 mode;
  dbus_bool_t make_backup;
  const char *etag;
  guint32 flags;
  guint32 pid;

  path = NULL;
  dbus_error_init (&derror);
  dbus_message_iter_init (message, &iter);
  if (!_g_dbus_message_iter_get_args (&iter, &derror, 
				      G_DBUS_TYPE_CSTRING, &path,
				      DBUS_TYPE_UINT16, &mode,
				      DBUS_TYPE_STRING, &etag,
				      DBUS_TYPE_BOOLEAN, &make_backup,
				      DBUS_TYPE_UINT32, &flags,
                                      DBUS_TYPE_UINT32, &pid,
				      0))
    {
      reply = dbus_message_new_error (message,
				      derror.name,
                                      derror.message);
      dbus_error_free (&derror);

      dbus_connection_send (connection, reply, NULL);
      g_free (path);
      return NULL;
    }
  
  job = g_object_new (G_VFS_TYPE_JOB_OPEN_FOR_WRITE,
		      "message", message,
		      "connection", connection,
		      NULL);

  job->filename = path;
  job->mode = mode;
  if (*etag != 0)
    job->etag = g_strdup (etag);
  job->make_backup = make_backup;
  job->flags = flags;
  job->backend = backend;
  job->pid = pid;
  
  return G_VFS_JOB (job);
}

static void
run (GVfsJob *job)
{
  GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (op_job->mode == OPEN_FOR_WRITE_CREATE)
    {
      if (class->create == NULL)
	{
	  g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			    _("Operation not supported by backend"));
	  return;
	}
      
      class->create (op_job->backend,
		     op_job,
		     op_job->filename,
		     op_job->flags);
    }
  else if (op_job->mode == OPEN_FOR_WRITE_APPEND)
    {
      if (class->append_to == NULL)
	{
	  g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			    _("Operation not supported by backend"));
	  return;
	}
      
      class->append_to (op_job->backend,
			op_job,
			op_job->filename,
			op_job->flags);
    }
  else if (op_job->mode == OPEN_FOR_WRITE_REPLACE)
    {
      if (class->replace == NULL)
	{
	  g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			    _("Operation not supported by backend"));
	  return;
	}
      
      class->replace (op_job->backend,
		      op_job,
		      op_job->filename,
		      op_job->etag,
		      op_job->make_backup,
		      op_job->flags);
    }
  else
    g_assert_not_reached (); /* Handled in try */
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (op_job->mode == OPEN_FOR_WRITE_CREATE)
    {
      if (class->try_create == NULL)
	return FALSE;
      return class->try_create (op_job->backend,
				op_job,
				op_job->filename,
				op_job->flags);
    }
  else if (op_job->mode == OPEN_FOR_WRITE_APPEND)
    {
      if (class->try_append_to == NULL)
	return FALSE;
      return class->try_append_to (op_job->backend,
				   op_job,
				   op_job->filename,
				   op_job->flags);
    }
  else if (op_job->mode == OPEN_FOR_WRITE_REPLACE)
    {
      if (class->try_replace == NULL)
	return FALSE;
      return class->try_replace (op_job->backend,
				 op_job,
				 op_job->filename,
				 op_job->etag,
				 op_job->make_backup,
				 op_job->flags);
    }
  else
    {
      GError *error = NULL;
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			   "Wrong open for write type");
      g_vfs_job_failed_from_error (job, error);
      g_error_free (error);
      return TRUE;
    }
}

void
g_vfs_job_open_for_write_set_handle (GVfsJobOpenForWrite *job,
				     GVfsBackendHandle handle)
{
  job->backend_handle = handle;
}

void
g_vfs_job_open_for_write_set_can_seek (GVfsJobOpenForWrite *job,
				       gboolean            can_seek)
{
  job->can_seek = can_seek;
}

void
g_vfs_job_open_for_write_set_initial_offset (GVfsJobOpenForWrite *job,
					     goffset              initial_offset)
{
  job->initial_offset = initial_offset;
}

/* Might be called on an i/o thwrite */
static DBusMessage *
create_reply (GVfsJob *job,
	      DBusConnection *connection,
	      DBusMessage *message)
{
  GVfsJobOpenForWrite *open_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  GVfsWriteChannel *channel;
  DBusMessage *reply;
  GError *error;
  int remote_fd;
  int fd_id;
  dbus_bool_t can_seek;
  guint64 initial_offset;
  
  g_assert (open_job->backend_handle != NULL);

  error = NULL;
  channel = g_vfs_write_channel_new (open_job->backend,
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
  initial_offset = open_job->initial_offset;
  dbus_message_append_args (reply,
			    DBUS_TYPE_UINT32, &fd_id,
			    DBUS_TYPE_BOOLEAN, &can_seek,
			    DBUS_TYPE_UINT64, &initial_offset,
			    DBUS_TYPE_INVALID);

  g_vfs_channel_set_backend_handle (G_VFS_CHANNEL (channel), open_job->backend_handle);
  open_job->backend_handle = NULL;
  open_job->write_channel = channel;

  g_signal_emit_by_name (job, "new-source", open_job->write_channel);
  
  return reply;
}

static void
finished (GVfsJob *job)
{
}

GPid
g_vfs_job_open_for_write_get_pid (GVfsJobOpenForWrite *job)
{
  return job->pid;
}
