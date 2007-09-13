#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gi18n.h>
#include "gvfsreadstream.h"
#include "gvfsjobseekread.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobSeekRead, g_vfs_job_seek_read, G_TYPE_VFS_JOB);

static gboolean start (GVfsJob *job);
static void send_reply (GVfsJob *job);

static void
g_vfs_job_seek_read_finalize (GObject *object)
{
  GVfsJobSeekRead *job;

  job = G_VFS_JOB_SEEK_READ (object);
  g_object_unref (job->stream);

  if (G_OBJECT_CLASS (g_vfs_job_seek_read_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_seek_read_parent_class)->finalize) (object);
}

static void
g_vfs_job_seek_read_class_init (GVfsJobSeekReadClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_seek_read_finalize;

  job_class->start = start;
  job_class->send_reply = send_reply;
}

static void
g_vfs_job_seek_read_init (GVfsJobSeekRead *job)
{
}

GVfsJob *
g_vfs_job_seek_read_new (GVfsReadStream *stream,
			 gpointer           handle,
			 GSeekType          seek_type,
			 goffset            offset)
{
  GVfsJobSeekRead *job;
  
  job = g_object_new (G_TYPE_VFS_JOB_SEEK_READ, NULL);

  job->stream = g_object_ref (stream);
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
    g_vfs_read_stream_send_error (op_job->stream, job->error);
  else
    {
      g_vfs_read_stream_send_seek_offset (op_job->stream,
					  op_job->final_offset);
    }
}

static gboolean
start (GVfsJob *job)
{
  GVfsJobSeekRead *op_job = G_VFS_JOB_SEEK_READ (job);

  return g_vfs_backend_seek_on_read (job->backend,
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
