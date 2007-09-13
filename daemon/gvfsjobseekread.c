#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gi18n.h>
#include "gvfsreadchannel.h"
#include "gvfsjobseekread.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobSeekRead, g_vfs_job_seek_read, G_TYPE_VFS_JOB);

static void     run        (GVfsJob *job);
static gboolean try        (GVfsJob *job);
static void     send_reply (GVfsJob *job);

static void
g_vfs_job_seek_read_finalize (GObject *object)
{
  GVfsJobSeekRead *job;

  job = G_VFS_JOB_SEEK_READ (object);
  g_object_unref (job->channel);

  if (G_OBJECT_CLASS (g_vfs_job_seek_read_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_seek_read_parent_class)->finalize) (object);
}

static void
g_vfs_job_seek_read_class_init (GVfsJobSeekReadClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_seek_read_finalize;

  job_class->run = run;
  job_class->try = try;
  job_class->send_reply = send_reply;
}

static void
g_vfs_job_seek_read_init (GVfsJobSeekRead *job)
{
}

GVfsJob *
g_vfs_job_seek_read_new (GVfsReadChannel *channel,
			 GVfsBackendHandle handle,
			 GSeekType seek_type,
			 goffset offset,
			 GVfsBackend *backend)
{
  GVfsJobSeekRead *job;
  
  job = g_object_new (G_TYPE_VFS_JOB_SEEK_READ,
		      NULL);

  job->backend = backend;
  job->channel = g_object_ref (channel);
  job->handle = handle;
  job->requested_offset = offset;
  job->seek_type = seek_type;
 
  return G_VFS_JOB (job);
}

/* Might be called on an i/o thread */
static void
send_reply (GVfsJob *job)
{
  GVfsJobSeekRead *op_job = G_VFS_JOB_SEEK_READ (job);
  
  g_print ("job_seek_read send reply, pos %d\n", (int)op_job->final_offset);

  if (job->failed)
    g_vfs_read_channel_send_error (op_job->channel, job->error);
  else
    {
      g_vfs_read_channel_send_seek_offset (op_job->channel,
					   op_job->final_offset);
    }
}

static void
run (GVfsJob *job)
{
  GVfsJobSeekRead *op_job = G_VFS_JOB_SEEK_READ (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  class->seek_on_read (op_job->backend,
		       op_job,
		       op_job->handle,
		       op_job->requested_offset,
		       op_job->seek_type);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobSeekRead *op_job = G_VFS_JOB_SEEK_READ (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_seek_on_read == NULL)
    return FALSE;
  
  return class->try_seek_on_read (op_job->backend,
				  op_job,
				  op_job->handle,
				  op_job->requested_offset,
				  op_job->seek_type);
}

void
g_vfs_job_seek_read_set_offset (GVfsJobSeekRead *job,
				goffset offset)
{
  job->final_offset = offset;
}
