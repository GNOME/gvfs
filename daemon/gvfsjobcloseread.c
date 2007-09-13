#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gi18n.h>
#include "gvfsreadchannel.h"
#include "gvfsjobcloseread.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobCloseRead, g_vfs_job_close_read, G_TYPE_VFS_JOB);

static gboolean start (GVfsJob *job);
static void send_reply (GVfsJob *job);

static void
g_vfs_job_close_read_finalize (GObject *object)
{
  GVfsJobCloseRead *job;

  job = G_VFS_JOB_CLOSE_READ (object);
  g_object_unref (job->channel);

  if (G_OBJECT_CLASS (g_vfs_job_close_read_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_close_read_parent_class)->finalize) (object);
}

static void
g_vfs_job_close_read_class_init (GVfsJobCloseReadClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_close_read_finalize;

  job_class->start = start;
  job_class->send_reply = send_reply;
}

static void
g_vfs_job_close_read_init (GVfsJobCloseRead *job)
{
}

GVfsJob *
g_vfs_job_close_read_new (GVfsReadChannel *channel,
			  GVfsBackendHandle handle,
			  GVfsBackend *backend)
{
  GVfsJobCloseRead *job;
  
  job = g_object_new (G_TYPE_VFS_JOB_CLOSE_READ,
		      "backend", backend,
		      NULL);

  job->channel = g_object_ref (channel);
  job->handle = handle;
  
  return G_VFS_JOB (job);
}

/* Might be called on an i/o thread */
static void
send_reply (GVfsJob *job)
{
  GVfsJobCloseRead *op_job = G_VFS_JOB_CLOSE_READ (job);
  
  g_print ("job_close_read send reply\n");

  if (job->failed)
    g_vfs_read_channel_send_error (op_job->channel, job->error);
  else
    g_vfs_read_channel_send_closed (op_job->channel);
}

static gboolean
start (GVfsJob *job)
{
  GVfsJobCloseRead *op_job = G_VFS_JOB_CLOSE_READ (job);

  return g_vfs_backend_close_read (job->backend,
				   op_job,
				   op_job->handle);
}
