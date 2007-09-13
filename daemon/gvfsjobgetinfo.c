
#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsjobgetinfo.h"
#include "gdbusutils.h"
#include "gvfsdaemonprotocol.h"

G_DEFINE_TYPE (GVfsJobGetInfo, g_vfs_job_get_info, G_VFS_TYPE_JOB_DBUS);

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static DBusMessage *create_reply (GVfsJob        *job,
				  DBusConnection *connection,
				  DBusMessage    *message);

static void
g_vfs_job_get_info_finalize (GObject *object)
{
  GVfsJobGetInfo *job;

  job = G_VFS_JOB_GET_INFO (object);

  if (job->file_info)
    g_object_unref (job->file_info);
  
  g_free (job->filename);
  g_free (job->attributes);
  
  if (G_OBJECT_CLASS (g_vfs_job_get_info_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_get_info_parent_class)->finalize) (object);
}

static void
g_vfs_job_get_info_class_init (GVfsJobGetInfoClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_get_info_finalize;
  job_class->run = run;
  job_class->try = try;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_get_info_init (GVfsJobGetInfo *job)
{
}

GVfsJob *
g_vfs_job_get_info_new (DBusConnection *connection,
			DBusMessage *message,
			GVfsBackend *backend)
{
  GVfsJobGetInfo *job;
  DBusMessage *reply;
  DBusError derror;
  int path_len;
  const char *path_data;
  char *attributes;
  dbus_uint32_t flags;
  
  dbus_error_init (&derror);
  if (!dbus_message_get_args (message, &derror, 
			      DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			      &path_data, &path_len,
			      DBUS_TYPE_STRING, &attributes,
			      DBUS_TYPE_UINT32, &flags,
			      0))
    {
      reply = dbus_message_new_error (message,
				      derror.name,
                                      derror.message);
      dbus_error_free (&derror);

      dbus_connection_send (connection, reply, NULL);
      return NULL;
    }

  job = g_object_new (G_VFS_TYPE_JOB_GET_INFO,
		      "message", message,
		      "connection", connection,
		      NULL);

  job->filename = g_strndup (path_data, path_len);
  job->backend = backend;
  job->attributes = g_strdup (attributes);
  job->flags = flags;
  
  return G_VFS_JOB (job);
}

static void
run (GVfsJob *job)
{
  GVfsJobGetInfo *op_job = G_VFS_JOB_GET_INFO (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->get_info == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported by backend"));
      return;
    }
  
  class->get_info (op_job->backend,
		   op_job,
		   op_job->filename,
		   op_job->attributes,
		   op_job->flags);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobGetInfo *op_job = G_VFS_JOB_GET_INFO (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_get_info == NULL)
    return FALSE;

  return class->try_get_info (op_job->backend,
			      op_job,
			      op_job->filename,
			      op_job->attributes,
			      op_job->flags);
}

void
g_vfs_job_get_info_set_info (GVfsJobGetInfo *job,
			     GFileInfo *file_info)
{
  job->file_info = g_object_ref (file_info);
}

/* Might be called on an i/o thread */
static DBusMessage *
create_reply (GVfsJob *job,
	      DBusConnection *connection,
	      DBusMessage *message)
{
  GVfsJobGetInfo *op_job = G_VFS_JOB_GET_INFO (job);
  DBusMessage *reply;
  DBusMessageIter iter;

  reply = dbus_message_new_method_return (message);

  dbus_message_iter_init_append (reply, &iter);

  _g_dbus_append_file_info (&iter, 
			   op_job->file_info);
  
  return reply;
}
