#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsjobmount.h"
#include "gdbusutils.h"
#include "gvfsdaemonprotocol.h"

G_DEFINE_TYPE (GVfsJobMount, g_vfs_job_mount, G_TYPE_VFS_JOB);

static void     run        (GVfsJob *job);
static gboolean try        (GVfsJob *job);
static void     send_reply (GVfsJob *job);

static void
g_vfs_job_mount_finalize (GObject *object)
{
  GVfsJobMount *job;

  job = G_VFS_JOB_MOUNT (object);

  g_mount_spec_unref (job->mount_spec);
  g_object_unref (job->mount_source);
  
  if (G_OBJECT_CLASS (g_vfs_job_mount_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_mount_parent_class)->finalize) (object);
}

static void
g_vfs_job_mount_class_init (GVfsJobMountClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_mount_finalize;
  job_class->run = run;
  job_class->try = try;
  job_class->send_reply = send_reply;
}

static void
g_vfs_job_mount_init (GVfsJobMount *job)
{
}

GVfsJob *
g_vfs_job_mount_new (GMountSpec *spec,
		     GMountSource *source,
		     GVfsBackend *backend)
{
  GVfsJobMount *job;

  job = g_object_new (G_TYPE_VFS_JOB_MOUNT,
		      NULL);

  job->mount_spec = g_mount_spec_ref (spec);
  job->mount_source = g_object_ref (source);
  job->backend = backend;
  
  return G_VFS_JOB (job);
}

static void
run (GVfsJob *job)
{
  GVfsJobMount *op_job = G_VFS_JOB_MOUNT (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  
  class->mount (op_job->backend,
		op_job,
		op_job->mount_spec,
		op_job->mount_source);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobMount *op_job = G_VFS_JOB_MOUNT (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_mount == NULL)
    return FALSE;

  return class->try_mount (op_job->backend,
			   op_job,
			   op_job->mount_spec,
			   op_job->mount_source);
}

static void
mount_failed (GVfsJobMount *op_job, GError *error)
{
  GVfsBackend *backend;
 
  backend = g_object_ref (op_job->backend);
  g_mount_source_failed (op_job->mount_source, error);
  g_vfs_job_emit_finished (G_VFS_JOB (op_job));
  
  /* Remove failed backend from daemon */
  g_vfs_job_source_closed  (G_VFS_JOB_SOURCE (backend));
  g_object_unref (backend);
}

static void
register_mount_callback (DBusMessage *reply,
			 GError *error,
			 gpointer user_data)
{
  GVfsJobMount *op_job = G_VFS_JOB_MOUNT (user_data);

  g_print ("register_mount_callback, reply: %p, error: %p\n", reply, error);
  
  if (reply == NULL)
    mount_failed (op_job, error);
  else
    {
      g_mount_source_done (op_job->mount_source);
      g_vfs_job_emit_finished (G_VFS_JOB (op_job));
    }
}

/* Might be called on an i/o thread */
static void
send_reply (GVfsJob *job)
{
  GVfsJobMount *op_job = G_VFS_JOB_MOUNT (job);

  g_print ("send_reply, failed: %d\n", job->failed);
  
  if (job->failed)
    mount_failed (op_job, job->error);
  else
    g_vfs_backend_register_mount (op_job->backend,
				  register_mount_callback,
				  job);
}
