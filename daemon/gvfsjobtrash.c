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
#include "gvfsjobtrash.h"
#include "gvfsdbusutils.h"
#include "gvfsdaemonprotocol.h"

G_DEFINE_TYPE (GVfsJobTrash, g_vfs_job_trash, G_VFS_TYPE_JOB_DBUS)

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static DBusMessage *create_reply (GVfsJob        *job,
				  DBusConnection *connection,
				  DBusMessage    *message);

static void
g_vfs_job_trash_finalize (GObject *object)
{
  GVfsJobTrash *job;

  job = G_VFS_JOB_TRASH (object);
  
  g_free (job->filename);
  
  if (G_OBJECT_CLASS (g_vfs_job_trash_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_trash_parent_class)->finalize) (object);
}

static void
g_vfs_job_trash_class_init (GVfsJobTrashClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_trash_finalize;
  job_class->run = run;
  job_class->try = try;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_trash_init (GVfsJobTrash *job)
{
}

GVfsJob *
g_vfs_job_trash_new (DBusConnection *connection,
			DBusMessage *message,
			GVfsBackend *backend)
{
  GVfsJobTrash *job;
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

  job = g_object_new (G_VFS_TYPE_JOB_TRASH,
		      "message", message,
		      "connection", connection,
		      NULL);

  job->filename = g_strndup (path_data, path_len);
  job->backend = backend;
  
  return G_VFS_JOB (job);
}

static void
run (GVfsJob *job)
{
  GVfsJobTrash *op_job = G_VFS_JOB_TRASH (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->trash == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported by backend"));
      return;
    }
  
  class->trash (op_job->backend,
		 op_job,
		 op_job->filename);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobTrash *op_job = G_VFS_JOB_TRASH (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_trash == NULL)
    return FALSE;
  
  return class->try_trash (op_job->backend,
			    op_job,
			    op_job->filename);
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
