/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2012 Red Hat, Inc.
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
 * Public License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Tomas Bzatek <tbzatek@redhat.com>
 */

#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gi18n.h>
#include "gvfsjobprogress.h"

#define RATE_LIMIT_TIME 100000

struct _GVfsJobProgressPrivate
{
  gint64 last_time;
};

G_DEFINE_TYPE_WITH_PRIVATE (GVfsJobProgress, g_vfs_job_progress, G_VFS_TYPE_JOB_DBUS)

static void
g_vfs_job_progress_finalize (GObject *object)
{
  GVfsJobProgress *job;

  job = G_VFS_JOB_PROGRESS (object);

  g_free (job->callback_obj_path);
  g_clear_object (&job->progress_proxy);

  if (G_OBJECT_CLASS (g_vfs_job_progress_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_progress_parent_class)->finalize) (object);
}

static void
g_vfs_job_progress_class_init (GVfsJobProgressClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_vfs_job_progress_finalize;
}

static void
g_vfs_job_progress_init (GVfsJobProgress *job)
{
  job->priv = g_vfs_job_progress_get_instance_private (job);
}

void
g_vfs_job_progress_callback (goffset current_num_bytes,
                             goffset total_num_bytes,
                             gpointer user_data)
{
  GVfsJobProgress *job = G_VFS_JOB_PROGRESS (user_data);
  GVfsJobDBus *dbus_job = G_VFS_JOB_DBUS (job);
  gint64 current_time = g_get_monotonic_time ();

  if (current_time - job->priv->last_time < RATE_LIMIT_TIME &&
      current_num_bytes != total_num_bytes)
    return;
  job->priv->last_time = current_time;

  g_debug ("g_vfs_job_progress_callback %" G_GOFFSET_FORMAT "/%" G_GOFFSET_FORMAT "\n", current_num_bytes, total_num_bytes);

  if (job->callback_obj_path == NULL || job->progress_proxy == NULL)
    return;

  gvfs_dbus_progress_call_progress (job->progress_proxy,
                                    current_num_bytes,
                                    total_num_bytes,
                                    NULL,
                                    NULL,
                                    NULL);
  g_dbus_connection_flush_sync (g_dbus_method_invocation_get_connection (dbus_job->invocation), NULL, NULL);
}

void
g_vfs_job_progress_construct_proxy (GVfsJob *job)
{
  GVfsJobDBus *dbus_job = G_VFS_JOB_DBUS (job);
  GVfsJobProgress *progress_job = G_VFS_JOB_PROGRESS (job);
  GError *error = NULL;

  if (!progress_job->send_progress || progress_job->progress_proxy)
    return;
  
  progress_job->progress_proxy = gvfs_dbus_progress_proxy_new_sync (g_dbus_method_invocation_get_connection (dbus_job->invocation),
                                                                    G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                                                    g_dbus_method_invocation_get_sender (dbus_job->invocation),
                                                                    progress_job->callback_obj_path,
                                                                    NULL,
                                                                    &error);
  if (!progress_job->progress_proxy)
    {
      g_warning ("g_vfs_job_progress_construct_proxy: %s (%s, %d)\n", error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }
}
