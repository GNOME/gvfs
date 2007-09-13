#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfswritechannel.h"
#include "gvfsjobopenforwrite.h"
#include "gdbusutils.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobOpenForWrite, g_vfs_job_open_for_write, G_VFS_TYPE_JOB_DBUS);

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static void         finished     (GVfsJob        *job);
static DBusMessage *create_reply (GVfsJob        *job,
				  DBusConnection *connection,
				  DBusMessage    *message);

static void
g_vfs_job_open_for_write_finalize (GObject *object)
{
  GVfsJobOpenForWrite *job;

  job = G_VFS_JOB_OPEN_FOR_WRITE (object);

  /* TODO: manage backend_handle if not put in write channel */

  if (job->write_channel)
    g_object_unref (job->write_channel);
  
  g_free (job->filename);
  
  if (G_OBJECT_CLASS (g_vfs_job_open_for_write_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_open_for_write_parent_class)->finalize) (object);
}

static void
g_vfs_job_open_for_write_class_init (GVfsJobOpenForWriteClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_open_for_write_finalize;
  job_class->run = run;
  job_class->try = try;
  job_class->finished = finished;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_open_for_write_init (GVfsJobOpenForWrite *job)
{
}

GVfsJob *
g_vfs_job_open_for_write_new (DBusConnection *connection,
			      DBusMessage *message,
			      GVfsBackend *backend)
{
  GVfsJobOpenForWrite *job;
  DBusMessageIter iter;
  DBusMessage *reply;
  DBusError derror;
  char *path;
  guint16 mode;
  dbus_bool_t make_backup;
  guint64 mtime;

  path = NULL;
  dbus_error_init (&derror);
  dbus_message_iter_init (message, &iter);
  if (!_g_dbus_message_iter_get_args (&iter, &derror, 
				      G_DBUS_TYPE_CSTRING, &path,
				      DBUS_TYPE_UINT16, &mode,
				      DBUS_TYPE_UINT64, &mtime,
				      DBUS_TYPE_BOOLEAN, &make_backup,
				      0))
    {
      reply = dbus_message_new_error (message,
				      derror.name,
                                      derror.message);
      dbus_error_free (&derror);

      dbus_connection_send (connection, reply, NULL);
      g_free (path);
      return NULL;
    }
  
  job = g_object_new (G_VFS_TYPE_JOB_OPEN_FOR_WRITE,
		      "message", message,
		      "connection", connection,
		      NULL);

  job->filename = path;
  job->mode = mode;
  job->mtime = (time_t)mtime;
  job->make_backup = make_backup;
  job->backend = backend;
  
  return G_VFS_JOB (job);
}

static void
run (GVfsJob *job)
{
  GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (op_job->mode == OPEN_FOR_WRITE_CREATE)
    {
      class->create (op_job->backend,
		     op_job,
		     op_job->filename);
    }
  else if (op_job->mode == OPEN_FOR_WRITE_APPEND)
    {
      class->append_to (op_job->backend,
			op_job,
			op_job->filename);
    }
  else if (op_job->mode == OPEN_FOR_WRITE_REPLACE)
    {
      class->replace (op_job->backend,
		      op_job,
		      op_job->filename,
		      op_job->mtime,
		      op_job->make_backup);
    }
  else
    g_assert_not_reached (); /* Handled in try */
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (op_job->mode == OPEN_FOR_WRITE_CREATE)
    {
      if (class->try_create == NULL)
	return FALSE;
      return class->try_create (op_job->backend,
				op_job,
				op_job->filename);
    }
  else if (op_job->mode == OPEN_FOR_WRITE_APPEND)
    {
      if (class->try_append_to == NULL)
	return FALSE;
      return class->try_append_to (op_job->backend,
				   op_job,
				   op_job->filename);
    }
  else if (op_job->mode == OPEN_FOR_WRITE_REPLACE)
    {
      if (class->try_replace == NULL)
	return FALSE;
      return class->try_replace (op_job->backend,
				 op_job,
				 op_job->filename,
				 op_job->mtime,
				 op_job->make_backup);
    }
  else
    {
      GError *error = NULL;
      g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
		   "Wrong open for write type");
      g_vfs_job_failed_from_error (job, error);
      g_error_free (error);
      return TRUE;
    }
}

void
g_vfs_job_open_for_write_set_handle (GVfsJobOpenForWrite *job,
				     GVfsBackendHandle handle)
{
  job->backend_handle = handle;
}

void
g_vfs_job_open_for_write_set_can_seek (GVfsJobOpenForWrite *job,
				       gboolean            can_seek)
{
  job->can_seek = can_seek;
}

/* Might be called on an i/o thwrite */
static DBusMessage *
create_reply (GVfsJob *job,
	      DBusConnection *connection,
	      DBusMessage *message)
{
  GVfsJobOpenForWrite *open_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  GVfsWriteChannel *channel;
  DBusMessage *reply;
  GError *error;
  int remote_fd;
  int fd_id;
  gboolean res;
  dbus_bool_t can_seek;

  g_assert (open_job->backend_handle != NULL);

  error = NULL;
  channel = g_vfs_write_channel_new (open_job->backend);

  remote_fd = g_vfs_channel_steal_remote_fd (G_VFS_CHANNEL (channel));
  if (!dbus_connection_send_fd (connection, 
				remote_fd,
				&fd_id, &error))
    {
      close (remote_fd);
      reply = _dbus_message_new_error_from_gerror (message, error);
      g_error_free (error);
      g_object_unref (channel);
      return reply;
    }
  close (remote_fd);

  reply = dbus_message_new_method_return (message);
  can_seek = open_job->can_seek;
  res = dbus_message_append_args (reply,
				  DBUS_TYPE_UINT32, &fd_id,
				  DBUS_TYPE_BOOLEAN, &can_seek,
				  DBUS_TYPE_INVALID);

  g_vfs_channel_set_backend_handle (G_VFS_CHANNEL (channel), open_job->backend_handle);
  open_job->backend_handle = NULL;
  open_job->write_channel = channel;
  
  return reply;
}

static void
finished (GVfsJob *job)
{
  GVfsJobOpenForWrite *open_job = G_VFS_JOB_OPEN_FOR_WRITE (job);

  if (open_job->write_channel)
    g_signal_emit_by_name (job, "new-source", open_job->write_channel);
  
}
