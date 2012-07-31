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

#include <strings.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gi18n.h>
#include "gvfsjobcopy.h"
#include <gvfsdbus.h>

G_DEFINE_TYPE (GVfsJobCopy, g_vfs_job_copy, G_VFS_TYPE_JOB_DBUS)

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static void         create_reply (GVfsJob               *job,
                                  GVfsDBusMount         *object,
                                  GDBusMethodInvocation *invocation);

static void
g_vfs_job_copy_finalize (GObject *object)
{
  GVfsJobCopy *job;

  job = G_VFS_JOB_COPY (object);
  
  g_free (job->source);
  g_free (job->destination);
  g_free (job->callback_obj_path);
  
  if (G_OBJECT_CLASS (g_vfs_job_copy_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_copy_parent_class)->finalize) (object);
}

static void
g_vfs_job_copy_class_init (GVfsJobCopyClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_copy_finalize;
  job_class->run = run;
  job_class->try = try;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_copy_init (GVfsJobCopy *job)
{
}

gboolean
g_vfs_job_copy_new_handle (GVfsDBusMount *object,
                           GDBusMethodInvocation *invocation,
                           const gchar *arg_path1_data,
                           const gchar *arg_path2_data,
                           guint arg_flags,
                           const gchar *arg_progress_obj_path,
                           GVfsBackend *backend)
{
  GVfsJobCopy *job;
  
  g_print ("called Copy()\n");

  if (g_vfs_backend_invocation_first_handler (object, invocation, backend))
    return TRUE;
  
  job = g_object_new (G_VFS_TYPE_JOB_COPY,
                      "object", object,
                      "invocation", invocation,
                      NULL);

  job->source = g_strdup (arg_path1_data);
  job->destination = g_strdup (arg_path2_data);
  job->backend = backend;
  job->flags = arg_flags;
  if (strcmp (arg_progress_obj_path, "/org/gtk/vfs/void") != 0)
    job->callback_obj_path = g_strdup (arg_progress_obj_path);

  g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), G_VFS_JOB (job));
  g_object_unref (job);

  return TRUE;
}

typedef struct {
  goffset current_num_bytes;
  goffset total_num_bytes;
} ProgressCallbackData;

static void
progress_cb (GVfsDBusProgress *proxy,
             GAsyncResult *res,
             gpointer user_data)
{
  GError *error = NULL;
  
  g_print ("progress_cb\n");
  
  if (! gvfs_dbus_progress_call_progress_finish (proxy, res, &error))
    {
      g_warning ("progress_cb: %s (%s, %d)\n", error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }
}

static void
progress_proxy_new_cb (GObject *source_object,
                       GAsyncResult *res,
                       gpointer user_data)
{
  ProgressCallbackData *data = user_data;
  GVfsDBusProgress *proxy;
  GError *error = NULL;

  g_print ("progress_proxy_new_cb\n");

  proxy = gvfs_dbus_progress_proxy_new_finish (res, &error);
  if (proxy == NULL)
    {
      g_warning ("progress_proxy_new_cb: %s (%s, %d)\n", error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      goto out;
    }
  
  gvfs_dbus_progress_call_progress (proxy,
                                    data->current_num_bytes,
                                    data->total_num_bytes,
                                    NULL,
                                    (GAsyncReadyCallback) progress_cb,
                                    NULL);
  g_object_unref (proxy);
  
 out:
  g_free (data);
}

static void
progress_callback (goffset current_num_bytes,
		   goffset total_num_bytes,
		   gpointer user_data)
{
  GVfsJob *job = G_VFS_JOB (user_data);
  GVfsJobDBus *dbus_job = G_VFS_JOB_DBUS (job);
  GVfsJobCopy *op_job = G_VFS_JOB_COPY (job);
  ProgressCallbackData *data;

  g_debug ("progress_callback %" G_GOFFSET_FORMAT "/%" G_GOFFSET_FORMAT "\n", current_num_bytes, total_num_bytes);

  if (op_job->callback_obj_path == NULL)
    return;

  data = g_new0 (ProgressCallbackData, 1);
  data->current_num_bytes = current_num_bytes;
  data->total_num_bytes = total_num_bytes;
  
  gvfs_dbus_progress_proxy_new (g_dbus_method_invocation_get_connection (dbus_job->invocation),
                                G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                g_dbus_method_invocation_get_sender (dbus_job->invocation),
                                op_job->callback_obj_path,
                                NULL,
                                progress_proxy_new_cb,
                                data);
}

static void
run (GVfsJob *job)
{
  GVfsJobCopy *op_job = G_VFS_JOB_COPY (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->copy == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported by backend"));
      return;
    }
  
  class->copy (op_job->backend,
	       op_job,
	       op_job->source,
	       op_job->destination,
	       op_job->flags,
	       progress_callback,
	       job);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobCopy *op_job = G_VFS_JOB_COPY (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_copy == NULL)
    return FALSE;
  
  return class->try_copy (op_job->backend,
			  op_job,
			  op_job->source,
			  op_job->destination,
			  op_job->flags,
			  progress_callback,
			  job);
}

/* Might be called on an i/o thread */
static void
create_reply (GVfsJob *job,
              GVfsDBusMount *object,
              GDBusMethodInvocation *invocation)
{
  gvfs_dbus_mount_complete_copy (object, invocation);
}
