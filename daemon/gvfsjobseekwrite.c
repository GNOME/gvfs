#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gi18n.h>
#include "gvfswritechannel.h"
#include "gvfsjobseekwrite.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobSeekWrite, g_vfs_job_seek_write, G_VFS_TYPE_JOB);

static void     run        (GVfsJob *job);
static gboolean try        (GVfsJob *job);
static void     send_reply (GVfsJob *job);

static void
g_vfs_job_seek_write_finalize (GObject *object)
{
  GVfsJobSeekWrite *job;

  job = G_VFS_JOB_SEEK_WRITE (object);
  g_object_unref (job->channel);

  if (G_OBJECT_CLASS (g_vfs_job_seek_write_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_seek_write_parent_class)->finalize) (object);
}

static void
g_vfs_job_seek_write_class_init (GVfsJobSeekWriteClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_seek_write_finalize;

  job_class->run = run;
  job_class->try = try;
  job_class->send_reply = send_reply;
}

static void
g_vfs_job_seek_write_init (GVfsJobSeekWrite *job)
{
}

GVfsJob *
g_vfs_job_seek_write_new (GVfsWriteChannel *channel,
			 GVfsBackendHandle handle,
			 GSeekType seek_type,
			 goffset offset,
			 GVfsBackend *backend)
{
  GVfsJobSeekWrite *job;
  
  job = g_object_new (G_VFS_TYPE_JOB_SEEK_WRITE,
		      NULL);

  job->backend = backend;
  job->channel = g_object_ref (channel);
  job->handle = handle;
  job->requested_offset = offset;
  job->seek_type = seek_type;
 
  return G_VFS_JOB (job);
}

/* Might be called on an i/o thwrite */
static void
send_reply (GVfsJob *job)
{
  GVfsJobSeekWrite *op_job = G_VFS_JOB_SEEK_WRITE (job);
  
  g_print ("job_seek_write send reply, pos %d\n", (int)op_job->final_offset);

  if (job->failed)
    g_vfs_channel_send_error (G_VFS_CHANNEL (op_job->channel), job->error);
  else
    {
      g_vfs_write_channel_send_seek_offset (op_job->channel,
					   op_job->final_offset);
    }
}

static void
run (GVfsJob *job)
{
  GVfsJobSeekWrite *op_job = G_VFS_JOB_SEEK_WRITE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  class->seek_on_write (op_job->backend,
			op_job,
			op_job->handle,
			op_job->requested_offset,
			op_job->seek_type);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobSeekWrite *op_job = G_VFS_JOB_SEEK_WRITE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_seek_on_write == NULL)
    return FALSE;
  
  return class->try_seek_on_write (op_job->backend,
				   op_job,
				   op_job->handle,
				   op_job->requested_offset,
				   op_job->seek_type);
}

void
g_vfs_job_seek_write_set_offset (GVfsJobSeekWrite *job,
				goffset offset)
{
  job->final_offset = offset;
}
