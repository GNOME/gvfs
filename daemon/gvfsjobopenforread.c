#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsreadstream.h"
#include "gvfsjobopenforread.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobOpenForRead, g_vfs_job_open_for_read, G_TYPE_VFS_JOB);

static gboolean start (GVfsJob *job);
static void send_reply (GVfsJob *job);

static void
g_vfs_job_open_for_read_finalize (GObject *object)
{
  GVfsJobOpenForRead *job;

  job = G_VFS_JOB_OPEN_FOR_READ (object);

  if (job->message)
    dbus_message_unref (job->message);

  if (job->connection)
    dbus_connection_unref (job->connection);
  
  /* TODO: manage backend_handle if not put in readstream */

  if (job->read_stream)
    g_object_unref (job->read_stream);
  
  g_free (job->filename);
  
  if (G_OBJECT_CLASS (g_vfs_job_open_for_read_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_open_for_read_parent_class)->finalize) (object);
}

static void
g_vfs_job_open_for_read_class_init (GVfsJobOpenForReadClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_open_for_read_finalize;

  job_class->start = start;
  job_class->send_reply = send_reply;
}

static void
g_vfs_job_open_for_read_init (GVfsJobOpenForRead *job)
{
}

GVfsJob *
g_vfs_job_open_for_read_new (DBusConnection *connection,
			     DBusMessage *message,
			     GVfsBackend *backend)
{
  GVfsJobOpenForRead *job;
  DBusMessage *reply;
  DBusError derror;
  int path_len;
  const char *path_data;
  
  dbus_error_init (&derror);
  if (!dbus_message_get_args (message, &derror, 
			      DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			      &path_data, &path_len,
			      0))
    {
      reply = dbus_message_new_error (message,
				      derror.name,
                                      derror.message);
      dbus_error_free (&derror);

      dbus_connection_send (connection, reply, NULL);
      return NULL;
    }

  job = g_object_new (G_TYPE_VFS_JOB_OPEN_FOR_READ, NULL);

  g_vfs_job_set_backend (G_VFS_JOB (job), backend);
  
  job->connection = dbus_connection_ref (connection);
  job->message = dbus_message_ref (message);
  job->filename = g_strndup (path_data, path_len);
  
  return G_VFS_JOB (job);
}

static gboolean
start (GVfsJob *job)
{
  GVfsJobOpenForRead *op_job = G_VFS_JOB_OPEN_FOR_READ (job);

  return g_vfs_backend_open_for_read (job->backend,
				      op_job,
				      op_job->filename);
}

void
g_vfs_job_open_for_read_set_handle (GVfsJobOpenForRead *job,
				    GVfsBackendHandle handle)
{
  job->backend_handle = handle;
}

void
g_vfs_job_open_for_read_set_can_seek (GVfsJobOpenForRead *job,
				      gboolean            can_seek)
{
  job->can_seek = can_seek;
}

/* Might be called on an i/o thread */
static DBusMessage *
create_reply (GVfsJob *job,
	      DBusConnection *connection,
	      DBusMessage *message)
{
  GVfsJobOpenForRead *open_job = G_VFS_JOB_OPEN_FOR_READ (job);
  GVfsReadStream *stream;
  DBusMessage *reply;
  GError *error;
  int remote_fd;
  int fd_id;
  gboolean res;
  dbus_bool_t can_seek;

  g_assert (open_job->backend_handle != NULL);

  error = NULL;
  stream = g_vfs_read_stream_new (job->backend, &error);
  if (stream == NULL)
    {
      reply = dbus_message_new_error_from_gerror (message, error);
      g_error_free (error);
      return reply;
    }

  remote_fd = g_vfs_read_stream_steal_remote_fd (stream);
  if (!dbus_connection_send_fd (connection, 
				remote_fd,
				&fd_id, &error))
    {
      close (remote_fd);
      reply = dbus_message_new_error_from_gerror (message, error);
      g_error_free (error);
      g_object_unref (stream);
      return reply;
    }
  close (remote_fd);

  reply = dbus_message_new_method_return (message);
  can_seek = open_job->can_seek;
  res = dbus_message_append_args (reply,
				  DBUS_TYPE_UINT32, &fd_id,
				  DBUS_TYPE_BOOLEAN, &can_seek,
				  DBUS_TYPE_INVALID);

  g_vfs_read_stream_set_backend_handle (stream, open_job->backend_handle);
  open_job->backend_handle = NULL;
  open_job->read_stream = stream;
  
  return reply;
}

/* Might be called on an i/o thread */
static void
send_reply (GVfsJob *job)
{
  GVfsJobOpenForRead *open_job = G_VFS_JOB_OPEN_FOR_READ (job);
  DBusMessage *reply;
  
  if (job->failed) 
    reply = dbus_message_new_error_from_gerror (open_job->message, job->error);
  else
    reply = create_reply (job, open_job->connection, open_job->message);
 
  g_assert (reply != NULL);

  /* Queues reply (threadsafely), actually sends it in mainloop */
  dbus_connection_send (open_job->connection, reply, NULL);
  dbus_message_unref (reply);

  g_vfs_job_emit_finished (job);
}

GVfsReadStream *
g_vfs_job_open_for_read_steal_stream (GVfsJobOpenForRead *job)
{
  GVfsReadStream *stream;
  
  stream = job->read_stream;
  job->read_stream = NULL;
  
  return stream;

}
