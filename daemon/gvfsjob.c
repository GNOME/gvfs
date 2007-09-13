#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsjob.h"

G_DEFINE_TYPE (GVfsJob, g_vfs_job, G_TYPE_OBJECT);

enum {
  CANCELLED,
  SEND_REPLY,
  FINISHED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
g_vfs_job_finalize (GObject *object)
{
  GVfsJob *job;

  job = G_VFS_JOB (object);

  if (job->error)
    g_error_free (job->error);
  
  if (G_OBJECT_CLASS (g_vfs_job_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_parent_class)->finalize) (object);
}

static void
g_vfs_job_class_init (GVfsJobClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_finalize;

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
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GVfsJobClass, finished),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
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
}

void
g_vfs_job_set_backend (GVfsJob     *job,
		       GVfsBackend *backend)
{
  job->backend = backend;
}

gboolean
g_vfs_job_start (GVfsJob *job)
{
  GVfsJobClass *class;

  class = G_VFS_JOB_GET_CLASS (job);
  return class->start (job);
}

void
g_vfs_job_cancel (GVfsJob *job)
{
  if (job->cancelled)
    return;

  job->cancelled = TRUE;
  g_signal_emit (job, signals[CANCELLED], 0);
}

static void 
g_vfs_job_send_reply (GVfsJob *job)
{
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

  if (job->failed)
    return;

  job->failed = TRUE;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);

  job->error = g_error_new (domain, code, message);
  g_free (message);

  g_vfs_job_send_reply (job);
}

void
g_vfs_job_failed_from_error (GVfsJob *job,
			     GError *error)
{
  if (job->failed)
    return;

  job->failed = TRUE;
  job->error = g_error_copy (error);
  g_vfs_job_send_reply (job);
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

/* Might be called on an i/o thread */
void
g_vfs_job_emit_finished (GVfsJob *job)
{
  g_assert (!job->finished);
  
  job->finished = TRUE;
  g_signal_emit (job, signals[FINISHED], 0);
}
