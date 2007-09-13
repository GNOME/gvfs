#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsjobmove.h"
#include "gdbusutils.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsjobsetattribute.h"

G_DEFINE_TYPE (GVfsJobSetAttribute, g_vfs_job_set_attribute, G_VFS_TYPE_JOB_DBUS);

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static DBusMessage *create_reply (GVfsJob        *job,
				  DBusConnection *connection,
				  DBusMessage    *message);

static void
g_vfs_job_set_attribute_finalize (GObject *object)
{
  GVfsJobSetAttribute *job;

  job = G_VFS_JOB_SET_ATTRIBUTE (object);

  g_free (job->filename);
  g_free (job->attribute);
  
  if (G_OBJECT_CLASS (g_vfs_job_set_attribute_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_set_attribute_parent_class)->finalize) (object);
}

static void
g_vfs_job_set_attribute_class_init (GVfsJobSetAttributeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_set_attribute_finalize;
  job_class->run = run;
  job_class->try = try;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_set_attribute_init (GVfsJobSetAttribute *job)
{
}

GVfsJob *
g_vfs_job_set_attribute_new (DBusConnection *connection,
			DBusMessage *message,
			GVfsBackend *backend)
{
  GVfsJobSetAttribute *job;
  DBusMessage *reply;
  DBusMessageIter iter, array_iter;
  DBusError derror;
  const gchar *filename = NULL;
  gint filename_len;
  GFileAttributeType type;
  GFileAttributeValue value;
  GFileGetInfoFlags flags;
  gchar *attribute;
  dbus_uint32_t flags_u32 = 0;
  
  dbus_error_init (&derror);

  dbus_message_iter_init (message, &iter);

  if (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_ARRAY &&
      dbus_message_iter_get_element_type (&iter) == DBUS_TYPE_BYTE)
    {
      dbus_message_iter_recurse (&iter, &array_iter);
      dbus_message_iter_get_fixed_array (&array_iter, &filename, &filename_len);
    }

  dbus_message_iter_next (&iter);

  if (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_UINT32)
    {
      dbus_message_iter_get_basic (&iter, &flags_u32);
      dbus_message_iter_next (&iter);
    }

  flags = flags_u32;

  if (!(filename && _g_dbus_get_file_attribute (&iter, &attribute, &type, &value)))
    {
      reply = dbus_message_new_error (message,
				      DBUS_ERROR_FAILED,
                                      _("Failed to demarshal message"));
      dbus_error_free (&derror);

      dbus_connection_send (connection, reply, NULL);
      return NULL;
    }

  job = g_object_new (G_VFS_TYPE_JOB_SET_ATTRIBUTE,
		      "message", message,
		      "connection", connection,
		      NULL);

  job->backend = backend;
  job->filename = g_strndup (filename, filename_len);
  job->attribute = attribute;
  job->type = type;
  job->value = value;
  job->flags = flags;

  return G_VFS_JOB (job);
}

static void
run (GVfsJob *job)
{
  GVfsJobSetAttribute *op_job = G_VFS_JOB_SET_ATTRIBUTE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->set_attribute == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported by backend"));
      return;
    }
  
  class->set_attribute (op_job->backend,
			op_job,
			op_job->filename,
			op_job->attribute,
			op_job->type,
			&op_job->value,
			op_job->flags);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobSetAttribute *op_job = G_VFS_JOB_SET_ATTRIBUTE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_set_attribute == NULL)
    return FALSE;
  
  return class->try_set_attribute (op_job->backend,
				   op_job,
				   op_job->filename,
				   op_job->attribute,
				   op_job->type,
				   &op_job->value,
				   op_job->flags);
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
