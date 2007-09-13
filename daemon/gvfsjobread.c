#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsreadhandle.h"
#include "gvfsjobopenforread.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobRead, g_vfs_job_read, G_TYPE_VFS_JOB);

static gboolean start (GVfsJob *job);
static void send_reply (GVfsJob *job);

static void
g_vfs_job_read_finalize (GObject *object)
{
  GVfsJobRead *job;

  job = G_VFS_JOB_READ (object);

  if (G_OBJECT_CLASS (g_vfs_job_read_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_read_parent_class)->finalize) (object);
}

static void
g_vfs_job_read_class_init (GVfsJobReadClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_read_finalize;

  job_class->start = start;
  job_class->send_reply = send_reply;
}

static void
g_vfs_job_read_init (GVfsJobRead *job)
{
}

GVfsJob *
g_vfs_job_read_new (GVfsDaemonBackend *backend,
		    GVfsReadHandle *handle,
		    gsize bytes_requested);
{
  GVfsJobRead *job;
  DBusMessage *reply;
  DBusError derror;
  int path_len;
  const char *path_data;
  
  job = g_object_new (G_TYPE_VFS_JOB_READ, NULL);

  G_VFS_JOB (job)->daemon = daemon;
  job->handle = handle; /* TODO: ref? */
  job->bytes_requested = bytes_requested;
  
  return G_VFS_JOB (job);
}

static gboolean
start (GVfsJob *job)
{
  GVfsDaemonBackendClass *class;
  GVfsJobRead *op_job = G_VFS_JOB_READ (job);

  class = G_VFS_DAEMON_BACKEND_GET_CLASS (job->daemon->backend);
  
  return class->read (job->daemon->backend,
		      op_job,
		      op_job->bytes_requested);
}

/* Takes ownership */
void
g_vfs_job_read_set_result (GVfsJobRead *job,
			   char *data,
			   gsize data_size)
{
  job->data = data;
  job->data_size = data_size;
}

