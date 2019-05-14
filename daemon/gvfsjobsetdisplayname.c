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
#include "gvfsjobsetdisplayname.h"

G_DEFINE_TYPE (GVfsJobSetDisplayName, g_vfs_job_set_display_name, G_VFS_TYPE_JOB_DBUS)

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static void         create_reply (GVfsJob               *job,
                                  GVfsDBusMount         *object,
                                  GDBusMethodInvocation *invocation);

static void
g_vfs_job_set_display_name_finalize (GObject *object)
{
  GVfsJobSetDisplayName *job;

  job = G_VFS_JOB_SET_DISPLAY_NAME (object);

  g_free (job->filename);
  g_free (job->display_name);
  g_free (job->new_path);
  
  if (G_OBJECT_CLASS (g_vfs_job_set_display_name_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_set_display_name_parent_class)->finalize) (object);
}

static void
g_vfs_job_set_display_name_class_init (GVfsJobSetDisplayNameClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_set_display_name_finalize;
  job_class->run = run;
  job_class->try = try;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_set_display_name_init (GVfsJobSetDisplayName *job)
{
}

gboolean
g_vfs_job_set_display_name_new_handle (GVfsDBusMount *object,
                                       GDBusMethodInvocation *invocation,
                                       const gchar *arg_path_data,
                                       const gchar *arg_display_name,
                                       GVfsBackend *backend)
{
  GVfsJobSetDisplayName *job;

  if (g_vfs_backend_invocation_first_handler (object, invocation, backend))
    return TRUE;
  
  job = g_object_new (G_VFS_TYPE_JOB_SET_DISPLAY_NAME,
                      "object", object,
                      "invocation", invocation,
                      NULL);

  job->filename = g_strdup (arg_path_data);
  job->backend = backend;
  job->display_name = g_strdup (arg_display_name);
  
  g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), G_VFS_JOB (job));
  g_object_unref (job);

  return TRUE;
}

static void
run (GVfsJob *job)
{
  GVfsJobSetDisplayName *op_job = G_VFS_JOB_SET_DISPLAY_NAME (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->set_display_name == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported"));
      return;
    }
  
  class->set_display_name (op_job->backend,
			   op_job,
			   op_job->filename,
			   op_job->display_name);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobSetDisplayName *op_job = G_VFS_JOB_SET_DISPLAY_NAME (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (g_vfs_backend_get_readonly_lockdown (op_job->backend))
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        _("Filesystem is read-only"));
      return TRUE;
    }

  if (class->try_set_display_name == NULL)
    return FALSE;
  
  return class->try_set_display_name (op_job->backend,
				 op_job,
				 op_job->filename,
				 op_job->display_name);
}

void
g_vfs_job_set_display_name_set_new_path (GVfsJobSetDisplayName *job,
					 const char *new_path)
{
  job->new_path = g_strdup (new_path);
}

/* Might be called on an i/o thread */
static void
create_reply (GVfsJob *job,
              GVfsDBusMount *object,
              GDBusMethodInvocation *invocation)
{
  GVfsJobSetDisplayName *op_job = G_VFS_JOB_SET_DISPLAY_NAME (job);

  g_assert (op_job->new_path != NULL);
  
  gvfs_dbus_mount_complete_set_display_name (object, invocation, op_job->new_path);
}
