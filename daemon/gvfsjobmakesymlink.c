#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsjobmakesymlink.h"
#include "gdbusutils.h"
#include "gvfsdaemonprotocol.h"

G_DEFINE_TYPE (GVfsJobMakeSymlink, g_vfs_job_make_symlink, G_VFS_TYPE_JOB_DBUS);

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static DBusMessage *create_reply (GVfsJob        *job,
				  DBusConnection *connection,
				  DBusMessage    *message);

static void
g_vfs_job_make_symlink_finalize (GObject *object)
{
  GVfsJobMakeSymlink *job;

  job = G_VFS_JOB_MAKE_SYMLINK (object);
  
  g_free (job->filename);
  g_free (job->symlink_value);
  
  if (G_OBJECT_CLASS (g_vfs_job_make_symlink_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_make_symlink_parent_class)->finalize) (object);
}

static void
g_vfs_job_make_symlink_class_init (GVfsJobMakeSymlinkClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_make_symlink_finalize;
  job_class->run = run;
  job_class->try = try;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_make_symlink_init (GVfsJobMakeSymlink *job)
{
}

GVfsJob *
g_vfs_job_make_symlink_new (DBusConnection *connection,
			DBusMessage *message,
			GVfsBackend *backend)
{
  GVfsJobMakeSymlink *job;
  DBusMessage *reply;
  DBusError derror;
  int path_len, symlink_len;
  const char *path_data, *symlink_data;
  
  dbus_error_init (&derror);
  if (!dbus_message_get_args (message, &derror, 
			      DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			      &path_data, &path_len,
			      DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			      &symlink_data, &symlink_len,
			      0))
    {
      reply = dbus_message_new_error (message,
				      derror.name,
                                      derror.message);
      dbus_error_free (&derror);

      dbus_connection_send (connection, reply, NULL);
      return NULL;
    }

  job = g_object_new (G_VFS_TYPE_JOB_MAKE_SYMLINK,
		      "message", message,
		      "connection", connection,
		      NULL);

  job->filename = g_strndup (path_data, path_len);
  job->symlink_value = g_strndup (path_data, path_len);
  job->backend = backend;
  
  return G_VFS_JOB (job);
}

static void
run (GVfsJob *job)
{
  GVfsJobMakeSymlink *op_job = G_VFS_JOB_MAKE_SYMLINK (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->delete == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Symlinks not supported by backend"));
      return;
    }
  
  class->make_symlink (op_job->backend,
		       op_job,
		       op_job->filename,
		       op_job->symlink_value);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobMakeSymlink *op_job = G_VFS_JOB_MAKE_SYMLINK (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_delete == NULL)
    return FALSE;
  
  return class->try_make_symlink (op_job->backend,
				  op_job,
				  op_job->filename,
				  op_job->symlink_value);
}

/* Might be called on an i/o thread */
static DBusMessage *
create_reply (GVfsJob *job,
	      DBusConnection *connection,
	      DBusMessage *message)
{
  DBusMessage *reply;

  reply = dbus_message_new_method_return (message);
  
  return reply;
}
