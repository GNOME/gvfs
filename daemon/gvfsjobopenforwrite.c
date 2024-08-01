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
#include "gvfswritechannel.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobOpenForWrite, g_vfs_job_open_for_write, G_VFS_TYPE_JOB_DBUS)

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static void         finished     (GVfsJob        *job);
static void         create_reply (GVfsJob               *job,
                                  GVfsDBusMount         *object,
                                  GDBusMethodInvocation *invocation);

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

static gboolean
open_for_write_new_handle_common (GVfsDBusMount *object,
                                  GDBusMethodInvocation *invocation,
                                  GUnixFDList *fd_list,
                                  const gchar *arg_path_data,
                                  guint16 arg_mode,
                                  const gchar *arg_etag,
                                  gboolean arg_make_backup,
                                  guint arg_flags,
                                  guint arg_pid,
                                  GVfsBackend *backend,
                                  GVfsJobOpenForWriteVersion version)
{
  GVfsJobOpenForWrite *job;

  if (g_vfs_backend_invocation_first_handler (object, invocation, backend))
    return TRUE;

  job = g_object_new (G_VFS_TYPE_JOB_OPEN_FOR_WRITE,
                      "object", object,
                      "invocation", invocation,
                      NULL);

  job->filename = g_strdup (arg_path_data);
  job->mode = arg_mode;
  if (*arg_etag != 0)
    job->etag = g_strdup (arg_etag);
  job->make_backup = arg_make_backup;
  job->flags = arg_flags;
  job->backend = backend;
  job->pid = arg_pid;
  job->version = version;

  g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), G_VFS_JOB (job));
  g_object_unref (job);

  return TRUE;
}

gboolean
g_vfs_job_open_for_write_new_handle (GVfsDBusMount *object,
                                     GDBusMethodInvocation *invocation,
                                     GUnixFDList *fd_list,
                                     const gchar *arg_path_data,
                                     guint16 arg_mode,
                                     const gchar *arg_etag,
                                     gboolean arg_make_backup,
                                     guint arg_flags,
                                     guint arg_pid,
                                     GVfsBackend *backend)
{
  return open_for_write_new_handle_common(object,
                                          invocation,
                                          fd_list,
                                          arg_path_data,
                                          arg_mode,
                                          arg_etag,
                                          arg_make_backup,
                                          arg_flags,
                                          arg_pid,
                                          backend,
                                          OPEN_FOR_WRITE_VERSION_ORIGINAL);
}

gboolean
g_vfs_job_open_for_write_new_handle_with_flags (GVfsDBusMount *object,
                                                GDBusMethodInvocation *invocation,
                                                GUnixFDList *fd_list,
                                                const gchar *arg_path_data,
                                                guint16 arg_mode,
                                                const gchar *arg_etag,
                                                gboolean arg_make_backup,
                                                guint arg_flags,
                                                guint arg_pid,
                                                GVfsBackend *backend)
{
  return open_for_write_new_handle_common(object,
                                          invocation,
                                          fd_list,
                                          arg_path_data,
                                          arg_mode,
                                          arg_etag,
                                          arg_make_backup,
                                          arg_flags,
                                          arg_pid,
                                          backend,
                                          OPEN_FOR_WRITE_VERSION_WITH_FLAGS);
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
			    _("Operation not supported"));
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
			    _("Operation not supported"));
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
			    _("Operation not supported"));
	  return;
	}
      
      class->replace (op_job->backend,
		      op_job,
		      op_job->filename,
		      op_job->etag,
		      op_job->make_backup,
		      op_job->flags);
    }
  else if (op_job->mode == OPEN_FOR_WRITE_EDIT)
    {
      if (class->edit == NULL)
        {
          g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            _("Operation not supported"));
          return;
        }

      class->edit (op_job->backend,
                   op_job,
                   op_job->filename,
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

  if (g_vfs_backend_get_readonly_lockdown (op_job->backend))
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        _("Filesystem is read-only"));
      return TRUE;
    }

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
  else if (op_job->mode == OPEN_FOR_WRITE_EDIT)
    {
      if (class->try_edit == NULL)
        return FALSE;
      return class->try_edit (op_job->backend,
                              op_job,
                              op_job->filename,
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
g_vfs_job_open_for_write_set_can_truncate (GVfsJobOpenForWrite *job,
                                           gboolean can_truncate)
{
  job->can_truncate = can_truncate;
}

void
g_vfs_job_open_for_write_set_initial_offset (GVfsJobOpenForWrite *job,
					     goffset              initial_offset)
{
  job->initial_offset = initial_offset;
}

/* Might be called on an i/o thread */
static void
create_reply (GVfsJob *job,
              GVfsDBusMount *object,
              GDBusMethodInvocation *invocation)
{
  GVfsJobOpenForWrite *open_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  GVfsWriteChannel *channel;
  GError *error;
  int remote_fd;
  int fd_id;
  GUnixFDList *fd_list;

  g_assert (open_job->backend_handle != NULL);

  channel = g_vfs_write_channel_new (open_job->backend,
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
  open_job->write_channel = channel;

  g_signal_emit_by_name (job, "new-source", open_job->write_channel);

  switch (open_job->version)
    {
      case OPEN_FOR_WRITE_VERSION_ORIGINAL:
        gvfs_dbus_mount_complete_open_for_write (object, invocation,
                                                 fd_list, g_variant_new_handle (fd_id),
                                                 open_job->can_seek ? OPEN_FOR_WRITE_FLAG_CAN_SEEK : 0,
                                                 open_job->initial_offset);
        break;
      case OPEN_FOR_WRITE_VERSION_WITH_FLAGS:
        gvfs_dbus_mount_complete_open_for_write_flags (object, invocation,
                                                 fd_list, g_variant_new_handle (fd_id),
                                                 (open_job->can_seek ? OPEN_FOR_WRITE_FLAG_CAN_SEEK : 0) |
                                                 (open_job->can_truncate ? OPEN_FOR_WRITE_FLAG_CAN_TRUNCATE : 0),
                                                 open_job->initial_offset);
        break;
    }
  
  close (remote_fd);
  g_object_unref (fd_list);
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
