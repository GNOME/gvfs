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
#include "gvfsjobcreatemonitor.h"

G_DEFINE_TYPE (GVfsJobCreateMonitor, g_vfs_job_create_monitor, G_VFS_TYPE_JOB_DBUS)

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static void         create_reply (GVfsJob               *job,
                                  GVfsDBusMount         *object,
                                  GDBusMethodInvocation *invocation);

static void
g_vfs_job_create_monitor_finalize (GObject *object)
{
  GVfsJobCreateMonitor *job;

  job = G_VFS_JOB_CREATE_MONITOR (object);
  
  g_free (job->filename);
  if (job->monitor)
    g_object_unref (job->monitor);
  
  if (G_OBJECT_CLASS (g_vfs_job_create_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_create_monitor_parent_class)->finalize) (object);
}

static void
g_vfs_job_create_monitor_class_init (GVfsJobCreateMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_create_monitor_finalize;
  job_class->run = run;
  job_class->try = try;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_create_monitor_init (GVfsJobCreateMonitor *job)
{
}


static gboolean
create_monitor_new_handle (GVfsDBusMount *object,
                           GDBusMethodInvocation *invocation,
                           const gchar *arg_path_data,
                           guint arg_flags,
                           GVfsBackend *backend,
                           gboolean is_directory)
{
  GVfsJobCreateMonitor *job;

  if (g_vfs_backend_invocation_first_handler (object, invocation, backend))
    return TRUE;
  
  job = g_object_new (G_VFS_TYPE_JOB_CREATE_MONITOR,
                      "object", object,
                      "invocation", invocation,
                      NULL);
  
  job->is_directory = is_directory;
  job->filename = g_strdup (arg_path_data);
  job->backend = backend;
  job->flags = arg_flags;

  g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), G_VFS_JOB (job));
  g_object_unref (job);

  return TRUE;
}

gboolean
g_vfs_job_create_file_monitor_new_handle (GVfsDBusMount *object,
                                          GDBusMethodInvocation *invocation,
                                          const gchar *arg_path_data,
                                          guint arg_flags,
                                          GVfsBackend *backend)
{
  return create_monitor_new_handle (object, invocation, arg_path_data, arg_flags, backend, FALSE);
}


gboolean
g_vfs_job_create_directory_monitor_new_handle (GVfsDBusMount *object,
                                               GDBusMethodInvocation *invocation,
                                               const gchar *arg_path_data,
                                               guint arg_flags,
                                               GVfsBackend *backend)
{
  return create_monitor_new_handle (object, invocation, arg_path_data, arg_flags, backend, TRUE);
}

void
g_vfs_job_create_monitor_set_monitor (GVfsJobCreateMonitor *job,
				      GVfsMonitor *monitor)
{
  job->monitor = g_object_ref (monitor);
}

static void
run (GVfsJob *job)
{
  GVfsJobCreateMonitor *op_job = G_VFS_JOB_CREATE_MONITOR (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (op_job->is_directory)
    {
      if (class->create_dir_monitor == NULL)
	g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			  _("Operation not supported"));
      else
	class->create_dir_monitor (op_job->backend,
				   op_job,
				   op_job->filename,
				   op_job->flags);
    }
  else
    {
      if (class->create_file_monitor == NULL)
	g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			  _("Operation not supported"));
      else
	class->create_file_monitor (op_job->backend,
				    op_job,
				    op_job->filename,
				    op_job->flags);
    }
  
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobCreateMonitor *op_job = G_VFS_JOB_CREATE_MONITOR (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (op_job->is_directory)
    {
      if (class->try_create_dir_monitor == NULL)
	{	
	  if (class->create_dir_monitor == NULL)
	    {
	      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				_("Operation not supported"));
	      return TRUE;
	    }
	  return FALSE;	
	}
  
      return class->try_create_dir_monitor (op_job->backend,
					    op_job,
					    op_job->filename,
					    op_job->flags);
    }
  else
    {
      if (class->try_create_file_monitor == NULL)
	{	
	  if (class->create_file_monitor == NULL)
	    {
	      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				_("Operation not supported"));
	      return TRUE;
	    }
	  return FALSE;	
	}
  
      return class->try_create_file_monitor (op_job->backend,
					     op_job,
					     op_job->filename,
					     op_job->flags);
    }
}

static gboolean
unref_monitor_timeout (gpointer data)
{
  GVfsMonitor *monitor = data;

  /* Unref the refcount for the VfsMonitor that we returned.
     If we didn't get an initial subscriber this is where we free the
     monitor */
  g_object_unref (monitor);
  
  return FALSE;
}

/* Might be called on an i/o thread */
static void
create_reply (GVfsJob *job,
              GVfsDBusMount *object,
              GDBusMethodInvocation *invocation)
{
  GVfsJobCreateMonitor *op_job = G_VFS_JOB_CREATE_MONITOR (job);
  const char *obj_path;

  /* Keep the monitor alive for at least 5 seconds
     to allow for a subscribe call to come in and bump
     the refcount */
  g_object_ref (op_job->monitor);
  g_timeout_add_seconds (5,
			 unref_monitor_timeout,
			 op_job->monitor);
  
  obj_path = g_vfs_monitor_get_object_path (op_job->monitor);
  
  if (op_job->is_directory)
    gvfs_dbus_mount_complete_create_directory_monitor (object, invocation, obj_path);
  else
    gvfs_dbus_mount_complete_create_file_monitor (object, invocation, obj_path);
}
