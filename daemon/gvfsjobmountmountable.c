/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsjobmountmountable.h"
#include "gvfsdbusutils.h"
#include "gvfsdaemonutils.h"

G_DEFINE_TYPE (GVfsJobMountMountable, g_vfs_job_mount_mountable, G_VFS_TYPE_JOB_DBUS)

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static DBusMessage *create_reply (GVfsJob        *job,
				  DBusConnection *connection,
				  DBusMessage    *message);

static void
g_vfs_job_mount_mountable_finalize (GObject *object)
{
  GVfsJobMountMountable *job;

  job = G_VFS_JOB_MOUNT_MOUNTABLE (object);

  if (job->mount_source)
    g_object_unref (job->mount_source);

  if (job->mount_spec)
    g_mount_spec_unref (job->mount_spec);
  
  g_free (job->filename);
  g_free (job->target_filename);
  
  if (G_OBJECT_CLASS (g_vfs_job_mount_mountable_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_mount_mountable_parent_class)->finalize) (object);
}

static void
g_vfs_job_mount_mountable_class_init (GVfsJobMountMountableClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_mount_mountable_finalize;
  job_class->run = run;
  job_class->try = try;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_mount_mountable_init (GVfsJobMountMountable *job)
{
}

GVfsJob *
g_vfs_job_mount_mountable_new (DBusConnection *connection,
			       DBusMessage *message,
			       GVfsBackend *backend)
{
  GVfsJobMountMountable *job;
  DBusMessage *reply;
  DBusMessageIter iter;
  DBusError derror;
  char *path;
  const char *dbus_id, *obj_path;
  
  dbus_error_init (&derror);
  dbus_message_iter_init (message, &iter);

  path = NULL;
  if (!_g_dbus_message_iter_get_args (&iter, &derror, 
				      G_DBUS_TYPE_CSTRING, &path,
				      DBUS_TYPE_STRING, &dbus_id,
				      DBUS_TYPE_OBJECT_PATH, &obj_path,
				      0))
    {
      g_free (path);
      reply = dbus_message_new_error (message,
				      derror.name,
                                      derror.message);
      dbus_error_free (&derror);

      dbus_connection_send (connection, reply, NULL);
      return NULL;
    }

  job = g_object_new (G_VFS_TYPE_JOB_MOUNT_MOUNTABLE,
		      "message", message,
		      "connection", connection,
		      NULL);

  job->filename = path;
  job->backend = backend;
  job->mount_source = g_mount_source_new (dbus_id, obj_path);
  
  return G_VFS_JOB (job);
}

void
g_vfs_job_mount_mountable_set_target (GVfsJobMountMountable *job,
				      GMountSpec *mount_spec,
				      const char *filename,
				      gboolean must_mount_location)
{
  job->mount_spec = g_mount_spec_ref (mount_spec);
  job->target_filename = g_strdup (filename);
  job->must_mount_location = must_mount_location;
}

void
g_vfs_job_mount_mountable_set_target_uri (GVfsJobMountMountable *job,
					  const char *uri,
					  gboolean must_mount_location)
{
  job->target_uri = g_strdup (uri);
  job->must_mount_location = must_mount_location;
}

static void
run (GVfsJob *job)
{
  GVfsJobMountMountable *op_job = G_VFS_JOB_MOUNT_MOUNTABLE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->mount_mountable == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported by backend"));
      return;
    }
  
  class->mount_mountable (op_job->backend,
			  op_job,
			  op_job->filename,
			  op_job->mount_source);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobMountMountable *op_job = G_VFS_JOB_MOUNT_MOUNTABLE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_mount_mountable == NULL)
    return FALSE;
  
  return class->try_mount_mountable (op_job->backend,
				     op_job,
				     op_job->filename,
				     op_job->mount_source);
}

/* Might be called on an i/o thread */
static DBusMessage *
create_reply (GVfsJob *job,
	      DBusConnection *connection,
	      DBusMessage *message)
{
  GVfsJobMountMountable *op_job = G_VFS_JOB_MOUNT_MOUNTABLE (job);
  DBusMessage *reply;
  DBusMessageIter iter;
  dbus_bool_t must_mount, is_uri;

  reply = dbus_message_new_method_return (message);

  must_mount = op_job->must_mount_location;
  is_uri = op_job->target_uri != NULL;
  if (is_uri)
    {
      _g_dbus_message_append_args (reply,
				   DBUS_TYPE_BOOLEAN, &is_uri,
				   G_DBUS_TYPE_CSTRING, &op_job->target_uri,
				   DBUS_TYPE_BOOLEAN, &must_mount,
				   0);
    }
  else
    {
      _g_dbus_message_append_args (reply,
				   DBUS_TYPE_BOOLEAN, &is_uri,
				   G_DBUS_TYPE_CSTRING, &op_job->target_filename,
				   DBUS_TYPE_BOOLEAN, &must_mount,
				   0);
      dbus_message_iter_init_append (reply, &iter);
      g_mount_spec_to_dbus (&iter, op_job->mount_spec);
    }
  
  return reply;
}
