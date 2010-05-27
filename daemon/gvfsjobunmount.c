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
#include "gvfsjobunmount.h"
#include "gvfsdbusutils.h"
#include "gvfsdaemonprotocol.h"

G_DEFINE_TYPE (GVfsJobUnmount, g_vfs_job_unmount, G_VFS_TYPE_JOB_DBUS)

static void     run        (GVfsJob *job);
static gboolean try        (GVfsJob *job);
static void     send_reply (GVfsJob *job);
static DBusMessage *create_reply (GVfsJob *job,
				  DBusConnection *connection,
				  DBusMessage *message);

static void
g_vfs_job_unmount_finalize (GObject *object)
{
  GVfsJobUnmount *job;

   job = G_VFS_JOB_UNMOUNT (object);

  if (job->mount_source)
    g_object_unref (job->mount_source);

  if (G_OBJECT_CLASS (g_vfs_job_unmount_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_unmount_parent_class)->finalize) (object);
}

static void
g_vfs_job_unmount_class_init (GVfsJobUnmountClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_unmount_finalize;
  job_class->run = run;
  job_class->try = try;
  job_class->send_reply = send_reply;

  job_dbus_class->create_reply = create_reply;

}

static void
g_vfs_job_unmount_init (GVfsJobUnmount *job)
{
}

GVfsJob *
g_vfs_job_unmount_new (DBusConnection *connection,
		       DBusMessage *message,
		       GVfsBackend *backend)
{
  GVfsJobUnmount *job;
  DBusMessage *reply;
  DBusMessageIter iter;
  DBusError derror;
  const char *dbus_id, *obj_path;
  guint32 flags;
  
  
  dbus_error_init (&derror);
  dbus_message_iter_init (message, &iter);

  if (!_g_dbus_message_iter_get_args (&iter, &derror, 
                                      DBUS_TYPE_STRING, &dbus_id,
                                      DBUS_TYPE_OBJECT_PATH, &obj_path,
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

  g_debug ("g_vfs_job_unmount_new request: %p\n", message);
  
  job = g_object_new (G_VFS_TYPE_JOB_UNMOUNT,
		      "message", message,
		      "connection", connection,
		      NULL);

  job->backend = backend;
  job->flags = flags;
  job->mount_source = g_mount_source_new (dbus_id, obj_path);
  
  return G_VFS_JOB (job);
}

static void
run (GVfsJob *job)
{
  GVfsJobUnmount *op_job = G_VFS_JOB_UNMOUNT (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  class->unmount (op_job->backend,
		  op_job,
                  op_job->flags,
                  op_job->mount_source);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobUnmount *op_job = G_VFS_JOB_UNMOUNT (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_unmount == NULL)
    {
      if (class->unmount == NULL)
	{
	  /* If unmount is not implemented we always succeed */
	  g_vfs_job_succeeded (G_VFS_JOB (job));
	  return TRUE;
	}
  
      return FALSE;
    }

  return class->try_unmount (op_job->backend,
			     op_job,
                             op_job->flags,
                             op_job->mount_source);
}

static void
unregister_mount_callback (DBusMessage *unmount_reply,
			   GError *error,
			   gpointer user_data)
{
  GVfsBackend *backend;
  GVfsJobUnmount *op_job = G_VFS_JOB_UNMOUNT (user_data);

  g_debug ("unregister_mount_callback, unmount_reply: %p, error: %p\n", unmount_reply, error);

  backend = op_job->backend;
  (*G_VFS_JOB_CLASS (g_vfs_job_unmount_parent_class)->send_reply) (G_VFS_JOB (op_job));

  /* Unlink job source from daemon */
  g_vfs_job_source_closed (G_VFS_JOB_SOURCE (backend));
}

/* Might be called on an i/o thread */
static void
send_reply (GVfsJob *job)
{
  GVfsJobUnmount *op_job = G_VFS_JOB_UNMOUNT (job);

  g_debug ("send_reply, failed: %d\n", job->failed);

  if (job->failed)
    (*G_VFS_JOB_CLASS (g_vfs_job_unmount_parent_class)->send_reply) (G_VFS_JOB (op_job));
  else
    g_vfs_backend_unregister_mount (op_job->backend,
                                    unregister_mount_callback,
                                    job);
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
