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
#include "gvfsjobunmount.h"

G_DEFINE_TYPE (GVfsJobUnmount, g_vfs_job_unmount, G_VFS_TYPE_JOB_DBUS)

static void     run        (GVfsJob *job);
static gboolean try        (GVfsJob *job);
static void     send_reply (GVfsJob *job);
static void     create_reply (GVfsJob               *job,
                              GVfsDBusMount         *object,
                              GDBusMethodInvocation *invocation);

static void
g_vfs_job_unmount_finalize (GObject *object)
{
  GVfsJobUnmount *job;

   job = G_VFS_JOB_UNMOUNT (object);

  if (job->mount_source)
    g_object_unref (job->mount_source);

  if (G_OBJECT_CLASS (g_vfs_job_unmount_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_unmount_parent_class)->finalize) (object);
}

static void
g_vfs_job_unmount_class_init (GVfsJobUnmountClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_unmount_finalize;
  job_class->run = run;
  job_class->try = try;
  job_class->send_reply = send_reply;

  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_unmount_init (GVfsJobUnmount *job)
{
}

gboolean
g_vfs_job_unmount_new_handle (GVfsDBusMount *object,
                              GDBusMethodInvocation *invocation,
                              const gchar *arg_dbus_id,
                              const gchar *arg_obj_path,
                              guint arg_flags,
                              GVfsBackend *backend)
{
  GVfsJobUnmount *job;

  if (g_vfs_backend_invocation_first_handler (object, invocation, backend))
    return TRUE;

  g_debug ("g_vfs_job_unmount_new request: %p\n", invocation);

  job = g_object_new (G_VFS_TYPE_JOB_UNMOUNT,
                      "object", object,
                      "invocation", invocation,
                      NULL);

  job->backend = backend;
  job->flags = arg_flags;
  job->mount_source = g_mount_source_new (arg_dbus_id, arg_obj_path);
  
  g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), G_VFS_JOB (job));
  g_object_unref (job);

  return TRUE;
}

static void
unmount_progress_clear (GVfsJobUnmount *op_job)
{
  gchar *message;

  if (op_job->unmount_progress_id > 0)
    {
      g_source_remove (op_job->unmount_progress_id);
      op_job->unmount_progress_id = 0;
    }

  if (!op_job->unmount_progress_fired)
    return;

  g_debug ("gvfsjobunmount progress clear\n");

  message = g_strdup_printf (_("%s has been unmounted\n"),
                             g_vfs_backend_get_display_name (op_job->backend));
  g_mount_source_show_unmount_progress (op_job->mount_source,
                                        message, 0, 0);
  g_free (message);
}

static gboolean
unmount_progress_timeout (gpointer user_data)
{
  GVfsJobUnmount *op_job = user_data;
  gchar *message;

  op_job->unmount_progress_id = 0;
  op_job->unmount_progress_fired = TRUE;

  g_debug ("gvfsjobunmount progress timeout reached\n");

  message = g_strdup_printf (_("Unmounting %s\nPlease wait"),
                             g_vfs_backend_get_display_name (op_job->backend));
  /* TODO: report estimated bytes and time left */
  g_mount_source_show_unmount_progress (op_job->mount_source,
                                        message, -1, -1);
  g_free (message);

  return FALSE;
}

static void
unmount_progress_start (GVfsJobUnmount *op_job)
{
  if (op_job->unmount_progress_id > 0)
    return;

  g_debug ("gvfsjobunmount progress timeout start\n");
  op_job->unmount_progress_id = g_timeout_add (1500, unmount_progress_timeout, op_job);
}

static void
run (GVfsJob *job)
{
  GVfsJobUnmount *op_job = G_VFS_JOB_UNMOUNT (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->unmount == NULL)
    return;

  unmount_progress_start (op_job);

  class->unmount (op_job->backend,
		  op_job,
                  op_job->flags,
                  op_job->mount_source);

  unmount_progress_clear (op_job);
}

static gboolean
job_finish_immediately_if_possible (GVfsJobUnmount *op_job)
{
  GVfsBackend      *backend = op_job->backend;
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  gboolean is_busy;
  gboolean force_unmount;

  if (class->try_unmount != NULL || class->unmount != NULL)
    return FALSE;

  is_busy = g_vfs_daemon_has_blocking_processes (g_vfs_backend_get_daemon (backend));
  force_unmount = op_job->flags & G_MOUNT_UNMOUNT_FORCE;

  if (is_busy && ! force_unmount)
    g_vfs_job_failed_literal (G_VFS_JOB (op_job),
			      G_IO_ERROR, G_IO_ERROR_BUSY,
			      _("File system is busy"));
  else
    g_vfs_job_succeeded (G_VFS_JOB (op_job));

  return TRUE;
}

static void
unmount_cb (GVfsBackend  *backend,
            GAsyncResult *res,
            gpointer      user_data)
{
  GVfsJobUnmount *op_job = G_VFS_JOB_UNMOUNT (user_data);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  gboolean should_unmount;
  gboolean finished;
  GError *error = NULL;

  should_unmount = g_vfs_backend_unmount_with_operation_finish (backend,
                                                                res,
                                                                &error);
  if (!should_unmount)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (op_job), error);
      g_error_free (error);
      return;
    }

  op_job->flags |= G_MOUNT_UNMOUNT_FORCE;
  finished = job_finish_immediately_if_possible (op_job);

  if (! finished)
    {
      gboolean run_in_thread = TRUE;

      if (class->try_unmount != NULL)
	run_in_thread = ! class->try_unmount (op_job->backend,
					      op_job,
					      op_job->flags,
					      op_job->mount_source);

      if (run_in_thread)
        {
          g_vfs_backend_set_block_requests (backend, TRUE);
          g_vfs_daemon_run_job_in_thread (g_vfs_backend_get_daemon (backend),
                                          G_VFS_JOB (op_job));
        }
    }
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobUnmount *op_job = G_VFS_JOB_UNMOUNT (job);
  GVfsBackend    *backend = op_job->backend;
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  gboolean is_busy;
  gboolean force_unmount;

  is_busy = g_vfs_daemon_has_blocking_processes (g_vfs_backend_get_daemon (backend));
  force_unmount = op_job->flags & G_MOUNT_UNMOUNT_FORCE;
  
  if (is_busy && !force_unmount)
    {
      if (g_mount_source_is_dummy (op_job->mount_source))
        g_vfs_job_failed_literal (G_VFS_JOB (op_job),
                                  G_IO_ERROR, G_IO_ERROR_BUSY,
                                  _("File system is busy"));
      else
        g_vfs_backend_unmount_with_operation (backend,
                                              op_job->mount_source,
                                              (GAsyncReadyCallback)unmount_cb,
                                              op_job);
      return TRUE;
    }

  if (job_finish_immediately_if_possible (op_job))
    return TRUE;
  else if (class->try_unmount != NULL)
    return class->try_unmount (op_job->backend,
			       op_job,
			       op_job->flags,
			       op_job->mount_source);
  else
    {
      /* We're going to run the backend's unmount method on a thread, block
       * new jobs coming in. */
      g_vfs_backend_set_block_requests (backend, TRUE);
      return FALSE;
    }
}

static void
unregister_mount_callback (GVfsBackend *backend,
                           GAsyncResult *res,
                           gpointer user_data)
{
  GVfsDaemon *daemon;
  GVfsJobUnmount *op_job = G_VFS_JOB_UNMOUNT (user_data);
  GError *error = NULL;

  g_debug ("unregister_mount_callback\n");
  if (!g_vfs_backend_unregister_mount_finish (backend, res, &error))
    {
      g_warning ("Error unregistering mount: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }
  
  (*G_VFS_JOB_CLASS (g_vfs_job_unmount_parent_class)->send_reply) (G_VFS_JOB (op_job));

  /* Unlink job source from daemon */
  daemon = g_vfs_backend_get_daemon (backend);
  g_vfs_daemon_close_active_channels (daemon, backend);
  g_vfs_job_source_closed (G_VFS_JOB_SOURCE (backend));

}

/* Might be called on an i/o thread */
static void
send_reply (GVfsJob *job)
{
  GVfsJobUnmount *op_job = G_VFS_JOB_UNMOUNT (job);
  GVfsBackend *backend = op_job->backend;

  if (job->failed)
    {
      g_vfs_backend_set_block_requests (backend, FALSE);
      (*G_VFS_JOB_CLASS (g_vfs_job_unmount_parent_class)->send_reply) (G_VFS_JOB (op_job));
    }
  else
    {
      /* Setting the backend to block requests will also
         set active GVfsChannels to block requets  */
      g_vfs_backend_set_block_requests (backend, TRUE);
      g_vfs_backend_unregister_mount (backend,
				      (GAsyncReadyCallback) unregister_mount_callback,
				      job);
    }
}

/* Might be called on an i/o thread */
static void
create_reply (GVfsJob *job,
              GVfsDBusMount *object,
              GDBusMethodInvocation *invocation)
{
  gvfs_dbus_mount_complete_unmount (object, invocation);
}
