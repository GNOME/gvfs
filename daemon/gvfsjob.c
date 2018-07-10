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
#include <gio/gio.h>
#include "gvfsjob.h"
#include "gvfsjobsource.h"

/* TODO: Real P_() */
#define P_(_x) (_x)

enum {
  PROP_0
};

enum {
  CANCELLED,
  SEND_REPLY,
  FINISHED,
  NEW_SOURCE,
  LAST_SIGNAL
};

struct _GVfsJobPrivate
{
  int dummy;
};

G_DEFINE_TYPE_WITH_PRIVATE (GVfsJob, g_vfs_job, G_TYPE_OBJECT)

static guint signals[LAST_SIGNAL] = { 0 };

static void g_vfs_job_get_property (GObject    *object,
				    guint       prop_id,
				    GValue     *value,
				    GParamSpec *pspec);
static void g_vfs_job_set_property (GObject         *object,
				    guint            prop_id,
				    const GValue    *value,
				    GParamSpec      *pspec);

static void
g_vfs_job_finalize (GObject *object)
{
  GVfsJob *job;

  job = G_VFS_JOB (object);

  if (job->error)
    g_error_free (job->error);

  if (job->backend_data_destroy)
    job->backend_data_destroy (job->backend_data);

  g_object_unref (job->cancellable);
  
  if (G_OBJECT_CLASS (g_vfs_job_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_parent_class)->finalize) (object);
}

static void
g_vfs_job_class_init (GVfsJobClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_vfs_job_finalize;
  gobject_class->set_property = g_vfs_job_set_property;
  gobject_class->get_property = g_vfs_job_get_property;

  signals[CANCELLED] =
    g_signal_new ("cancelled",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GVfsJobClass, cancelled),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
  signals[FINISHED] =
    g_signal_new ("finished",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_FIRST,
		  G_STRUCT_OFFSET (GVfsJobClass, finished),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
  signals[NEW_SOURCE] =
    g_signal_new ("new-source",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GVfsJobClass, new_source),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__OBJECT,
		  G_TYPE_NONE, 1, G_VFS_TYPE_JOB_SOURCE);
  signals[SEND_REPLY] =
    g_signal_new ("send-reply",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GVfsJobClass, send_reply),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
}

static void
g_vfs_job_init (GVfsJob *job)
{
  job->priv = g_vfs_job_get_instance_private (job);

  job->cancellable = g_cancellable_new ();
  
}

void
g_vfs_job_set_backend_data (GVfsJob     *job,
			    gpointer     backend_data,
			    GDestroyNotify destroy)
{
  if (job->backend_data_destroy)
    {
      job->backend_data_destroy (job->backend_data);
    }
    
  job->backend_data = backend_data;
  job->backend_data_destroy = destroy;
}

static void
g_vfs_job_set_property (GObject         *object,
			guint            prop_id,
			const GValue    *value,
			GParamSpec      *pspec)
{
  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_vfs_job_get_property (GObject    *object,
			guint       prop_id,
			GValue     *value,
			GParamSpec *pspec)
{
  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

void
g_vfs_job_run (GVfsJob *job)
{
  GVfsJobClass *class;

  class = G_VFS_JOB_GET_CLASS (job);

  /* Ensure that the job lives durint the whole
   * lifetime of the call, as it may disappear when
   * we call g_vfs_job_succeed/fail()
   */
  g_object_ref (job);
  
  class->run (job);
  
  g_object_unref (job);
}

gboolean
g_vfs_job_try (GVfsJob *job)
{
  GVfsJobClass *class;
  gboolean res;

  class = G_VFS_JOB_GET_CLASS (job);
  
  /* Ensure that the job lives during the whole
   * lifetime of the call, as it may disappear when
   * we call g_vfs_job_succeed/fail()
   */
  g_object_ref (job);
  res = class->try (job);
  g_object_unref (job);

  return res;
}

void
g_vfs_job_cancel (GVfsJob *job)
{
  if (job->cancelled || job->finished)
    return;

  job->cancelled = TRUE;
  g_signal_emit (job, signals[CANCELLED], 0);
  g_cancellable_cancel (job->cancellable);
}

static void 
g_vfs_job_send_reply (GVfsJob *job)
{
  job->sent_reply = TRUE;
  g_signal_emit (job, signals[SEND_REPLY], 0);
}

void
g_vfs_job_failed (GVfsJob *job,
		  GQuark         domain,
		  gint           code,
		  const gchar   *format,
		  ...)
{
  va_list args;
  char *message;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);

  g_vfs_job_failed_literal (job, domain, code, message);
  g_free (message);
}

void
g_vfs_job_failed_literal (GVfsJob *job,
                          GQuark        domain,
                          gint          code,
                          const gchar  *message)
{
  if (job->failed)
    return;

  job->failed = TRUE;

  job->error = g_error_new_literal (domain, code, message);

  g_vfs_job_send_reply (job);
}

void
g_vfs_job_failed_from_error (GVfsJob *     job,
			     const GError *error)
{
  if (job->failed)
    return;

  job->failed = TRUE;
  job->error = g_error_copy (error);
  g_vfs_job_send_reply (job);
}

void
g_vfs_job_failed_from_errno (GVfsJob     *job,
			     gint         errno_arg)
{
  GError *error = NULL;
  
  g_set_error_literal (&error, G_IO_ERROR,
		       g_io_error_from_errno (errno_arg),
		       g_strerror (errno_arg));
  g_vfs_job_failed_from_error (job, error);
  g_error_free (error);
}

void
g_vfs_job_succeeded (GVfsJob *job)
{
  job->failed = FALSE;
  g_vfs_job_send_reply (job);
}


gboolean
g_vfs_job_is_finished (GVfsJob *job)
{
  return job->finished;
}

gboolean
g_vfs_job_is_cancelled (GVfsJob *job)
{
  return job->cancelled;
}

/* Might be called on an i/o thread */
void
g_vfs_job_emit_finished (GVfsJob *job)
{
  g_assert (!job->finished);
  
  job->finished = TRUE;
  g_signal_emit (job, signals[FINISHED], 0);
}
