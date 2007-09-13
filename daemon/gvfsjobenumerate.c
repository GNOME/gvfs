#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsjobenumerate.h"
#include "gdbusutils.h"
#include "gvfsdaemonprotocol.h"

G_DEFINE_TYPE (GVfsJobEnumerate, g_vfs_job_enumerate, G_TYPE_VFS_JOB_DBUS);

static void         run        (GVfsJob        *job);
static gboolean     try        (GVfsJob        *job);
static void         send_reply   (GVfsJob        *job);
static DBusMessage *create_reply (GVfsJob        *job,
				  DBusConnection *connection,
				  DBusMessage    *message);

static void
g_vfs_job_enumerate_finalize (GObject *object)
{
  GVfsJobEnumerate *job;

  job = G_VFS_JOB_ENUMERATE (object);

  g_free (job->filename);
  g_free (job->attributes);
  g_free (job->object_path);
  
  if (G_OBJECT_CLASS (g_vfs_job_enumerate_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_enumerate_parent_class)->finalize) (object);
}

static void
g_vfs_job_enumerate_class_init (GVfsJobEnumerateClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_enumerate_finalize;
  job_class->run = run;
  job_class->try = try;
  job_class->send_reply = send_reply;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_enumerate_init (GVfsJobEnumerate *job)
{
}

GVfsJob *
g_vfs_job_enumerate_new (DBusConnection *connection,
			 DBusMessage *message,
			 GVfsBackend *backend)
{
  GVfsJobEnumerate *job;
  DBusMessage *reply;
  DBusError derror;
  int path_len;
  const char *obj_path;
  const char *path_data;
  guint32 requested;
  char *attributes;
  dbus_bool_t follow_symlinks;
  
  dbus_error_init (&derror);
  if (!dbus_message_get_args (message, &derror, 
			      DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			      &path_data, &path_len,
			      DBUS_TYPE_STRING, &obj_path,
			      DBUS_TYPE_UINT32, &requested,
			      DBUS_TYPE_STRING, &attributes,
			      DBUS_TYPE_BOOLEAN, &follow_symlinks,
			      0))
    {
      reply = dbus_message_new_error (message,
				      derror.name,
                                      derror.message);
      dbus_error_free (&derror);

      dbus_connection_send (connection, reply, NULL);
      return NULL;
    }

  job = g_object_new (G_TYPE_VFS_JOB_ENUMERATE,
		      "message", message,
		      "connection", connection,
		      NULL);
  
  job->object_path = g_strdup (obj_path);
  job->filename = g_strndup (path_data, path_len);
  job->backend = backend;
  job->requested = requested;
  job->attributes = g_strdup (attributes);
  job->follow_symlinks = follow_symlinks;
  
  return G_VFS_JOB (job);
}


void
g_vfs_job_enumerate_set_result (GVfsJobEnumerate *job,
				GFileInfoRequestFlags requested_result)
{
  job->requested_result = requested_result;
}

void
g_vfs_job_enumerate_add_info (GVfsJobEnumerate *job,
			      GList *infos)
{
  DBusMessage *message, *orig_message;
  DBusMessageIter iter, array_iter;
  char *sig;

  orig_message = g_vfs_job_dbus_get_message (G_VFS_JOB_DBUS (job));
  
  message = dbus_message_new_method_call (dbus_message_get_sender (orig_message),
					  job->object_path,
					  G_VFS_DBUS_ENUMERATOR_INTERFACE,
					  G_VFS_DBUS_ENUMERATOR_GOT_INFO);
  dbus_message_set_no_reply (message, TRUE);

  dbus_message_iter_init_append (message, &iter);

  sig = g_dbus_get_file_info_signature (job->requested_result);

  if (!dbus_message_iter_open_container (&iter,
					 DBUS_TYPE_ARRAY, sig, 
					 &array_iter))
    _g_dbus_oom ();
  
  g_free (sig);

  while (infos != NULL)
    {
      g_dbus_append_file_info (&array_iter, 
			       job->requested_result,
			       infos->data);
      infos = infos->next;
    }

  if (!dbus_message_iter_close_container (&iter, &array_iter))
    _g_dbus_oom ();
  
  dbus_connection_send (g_vfs_job_dbus_get_connection (G_VFS_JOB_DBUS (job)),
			message, NULL);
  dbus_message_unref (message);
}

void
g_vfs_job_enumerate_done (GVfsJobEnumerate *job)
{
  DBusMessage *message, *orig_message;
  
  g_assert (!G_VFS_JOB (job)->failed);

  orig_message = g_vfs_job_dbus_get_message (G_VFS_JOB_DBUS (job));
  
  message = dbus_message_new_method_call (dbus_message_get_sender (orig_message),
					  job->object_path,
					  G_VFS_DBUS_ENUMERATOR_INTERFACE,
					  G_VFS_DBUS_ENUMERATOR_DONE);
  dbus_message_set_no_reply (message, TRUE);

  dbus_connection_send (g_vfs_job_dbus_get_connection (G_VFS_JOB_DBUS (job)),
			message, NULL);
  dbus_message_unref (message);

  g_vfs_job_emit_finished (G_VFS_JOB (job));
}

static void
run (GVfsJob *job)
{
  GVfsJobEnumerate *op_job = G_VFS_JOB_ENUMERATE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  
  class->enumerate (op_job->backend,
		    op_job,
		    op_job->filename,
		    op_job->requested,
		    op_job->attributes,
		    op_job->follow_symlinks);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobEnumerate *op_job = G_VFS_JOB_ENUMERATE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  
  if (class->try_enumerate == NULL)
    return FALSE;
  
  return class->try_enumerate (op_job->backend,
			       op_job,
			       op_job->filename,
			       op_job->requested,
			       op_job->attributes,
			       op_job->follow_symlinks);
}

static void
send_reply (GVfsJob *job)
{
  GVfsJobDBus *dbus_job = G_VFS_JOB_DBUS (job);
  DBusMessage *reply;
  GVfsJobDBusClass *class;

  g_print ("send_reply, failed=%d (%s)\n", job->failed, job->failed?job->error->message:"");
  
  class = G_VFS_JOB_DBUS_GET_CLASS (job);
  
  if (job->failed) 
    reply = _dbus_message_new_error_from_gerror (dbus_job->message, job->error);
  else
    reply = class->create_reply (job, dbus_job->connection, dbus_job->message);
 
  g_assert (reply != NULL);

  /* Queues reply (threadsafely), actually sends it in mainloop */
  dbus_connection_send (dbus_job->connection, reply, NULL);
  dbus_message_unref (reply);
  
  if (job->failed)
    g_vfs_job_emit_finished (job);
}

/* Might be called on an i/o thread */
static DBusMessage *
create_reply (GVfsJob *job,
	      DBusConnection *connection,
	      DBusMessage *message)
{
  GVfsJobEnumerate *op_job = G_VFS_JOB_ENUMERATE (job);
  DBusMessage *reply;
  DBusMessageIter iter;
  guint32 requested_32;

  reply = dbus_message_new_method_return (message);

  dbus_message_iter_init_append (reply, &iter);

  requested_32 = op_job->requested_result;
  if (!dbus_message_iter_append_basic (&iter,
				       DBUS_TYPE_UINT32,
				       &requested_32))
    _g_dbus_oom ();

  return reply;
}
