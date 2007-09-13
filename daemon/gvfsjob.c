#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsjob.h"

G_DEFINE_TYPE (GVfsJob, g_vfs_job, G_TYPE_VFS_JOB);

enum {
  CANCEL,
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

  signals[CANCEL] =
    g_signal_new ("done",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GVfsJobClass, cancel),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);

}

static void
g_vfs_job_init (GVfsJob *job)
{
}

void
g_vfs_job_cancel (GVfsJob *job)
{
  if (job->cancelled)
    return;

  job->cancelled = TRUE;
  g_signal_emit (job, signals[CANCEL], 0);
}

void
g_vfs_job_set_failed (GVfsJob *job,
		      GError *error)
{
  if (job->failed)
    return;

  job->failed = TRUE;
  job->error = g_error_copy (error);
}
