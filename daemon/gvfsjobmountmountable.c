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
#include "gvfsjobmountmountable.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobMountMountable, g_vfs_job_mount_mountable, G_VFS_TYPE_JOB_DBUS)

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static void         create_reply (GVfsJob               *job,
                                  GVfsDBusMount         *object,
                                  GDBusMethodInvocation *invocation);

static void
g_vfs_job_mount_mountable_finalize (GObject *object)
{
  GVfsJobMountMountable *job;

  job = G_VFS_JOB_MOUNT_MOUNTABLE (object);

  if (job->mount_source)
    g_object_unref (job->mount_source);

  if (job->mount_spec)
    g_mount_spec_unref (job->mount_spec);
  
  g_free (job->filename);
  g_free (job->target_filename);
  
  if (G_OBJECT_CLASS (g_vfs_job_mount_mountable_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_mount_mountable_parent_class)->finalize) (object);
}

static void
g_vfs_job_mount_mountable_class_init (GVfsJobMountMountableClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_mount_mountable_finalize;
  job_class->run = run;
  job_class->try = try;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_mount_mountable_init (GVfsJobMountMountable *job)
{
}

gboolean
g_vfs_job_mount_mountable_new_handle (GVfsDBusMount *object,
                                      GDBusMethodInvocation *invocation,
                                      const gchar *arg_path_data,
                                      const gchar *arg_dbus_id,
                                      const gchar *arg_obj_path,
                                      GVfsBackend *backend)
{
  GVfsJobMountMountable *job;

  if (g_vfs_backend_invocation_first_handler (object, invocation, backend))
    return TRUE;
  
  job = g_object_new (G_VFS_TYPE_JOB_MOUNT_MOUNTABLE,
                      "object", object,
                      "invocation", invocation,
                      NULL);

  job->filename = g_strdup (arg_path_data);
  job->backend = backend;
  job->mount_source = g_mount_source_new (arg_dbus_id, arg_obj_path);

  g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), G_VFS_JOB (job));
  g_object_unref (job);

  return TRUE;
}

void
g_vfs_job_mount_mountable_set_target (GVfsJobMountMountable *job,
				      GMountSpec *mount_spec,
				      const char *filename,
				      gboolean must_mount_location)
{
  job->mount_spec = g_mount_spec_ref (mount_spec);
  job->target_filename = g_strdup (filename);
  job->must_mount_location = must_mount_location;
}

void
g_vfs_job_mount_mountable_set_target_uri (GVfsJobMountMountable *job,
					  const char *uri,
					  gboolean must_mount_location)
{
  job->target_uri = g_strdup (uri);
  job->must_mount_location = must_mount_location;
}

static void
run (GVfsJob *job)
{
  GVfsJobMountMountable *op_job = G_VFS_JOB_MOUNT_MOUNTABLE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->mount_mountable == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported"));
      return;
    }
  
  class->mount_mountable (op_job->backend,
			  op_job,
			  op_job->filename,
			  op_job->mount_source);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobMountMountable *op_job = G_VFS_JOB_MOUNT_MOUNTABLE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_mount_mountable == NULL)
    return FALSE;
  
  return class->try_mount_mountable (op_job->backend,
				     op_job,
				     op_job->filename,
				     op_job->mount_source);
}

/* Might be called on an i/o thread */
static void
create_reply (GVfsJob *job,
              GVfsDBusMount *object,
              GDBusMethodInvocation *invocation)
{
  GVfsJobMountMountable *op_job = G_VFS_JOB_MOUNT_MOUNTABLE (job);
  gboolean is_uri, must_mount;
  GMountSpec *fake_mountspec = NULL;
  
  must_mount = op_job->must_mount_location;
  is_uri = op_job->target_uri != NULL;

  if (is_uri)
    fake_mountspec = g_mount_spec_new (NULL);

  gvfs_dbus_mount_complete_mount_mountable (object,
                                            invocation,
                                            is_uri,
                                            is_uri ? op_job->target_uri : op_job->target_filename,
                                            must_mount,
                                            g_mount_spec_to_dbus (is_uri ? fake_mountspec : op_job->mount_spec));
  
  if (fake_mountspec)
    g_mount_spec_unref (fake_mountspec);
}
