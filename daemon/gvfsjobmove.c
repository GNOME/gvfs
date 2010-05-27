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

#include <strings.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsjobmove.h"
#include "gvfsdbusutils.h"
#include "gvfsdaemonprotocol.h"

G_DEFINE_TYPE (GVfsJobMove, g_vfs_job_move, G_VFS_TYPE_JOB_DBUS)

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static DBusMessage *create_reply (GVfsJob        *job,
				  DBusConnection *connection,
				  DBusMessage    *message);

static void
g_vfs_job_move_finalize (GObject *object)
{
  GVfsJobMove *job;

  job = G_VFS_JOB_MOVE (object);
  
  g_free (job->source);
  g_free (job->destination);
  g_free (job->callback_obj_path);
  
  if (G_OBJECT_CLASS (g_vfs_job_move_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_move_parent_class)->finalize) (object);
}

static void
g_vfs_job_move_class_init (GVfsJobMoveClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_move_finalize;
  job_class->run = run;
  job_class->try = try;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_move_init (GVfsJobMove *job)
{
}

GVfsJob *
g_vfs_job_move_new (DBusConnection *connection,
			DBusMessage *message,
			GVfsBackend *backend)
{
  GVfsJobMove *job;
  DBusMessage *reply;
  DBusError derror;
  int path1_len, path2_len;
  const char *path1_data, *path2_data, *callback_obj_path;
  dbus_uint32_t flags;
  
  dbus_error_init (&derror);
  if (!dbus_message_get_args (message, &derror, 
			      DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			      &path1_data, &path1_len,
			      DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			      &path2_data, &path2_len,
                              DBUS_TYPE_UINT32, &flags,
			      DBUS_TYPE_OBJECT_PATH, &callback_obj_path,
			      0))
    {
      reply = dbus_message_new_error (message,
				      derror.name,
                                      derror.message);
      dbus_error_free (&derror);

      dbus_connection_send (connection, reply, NULL);
      return NULL;
    }

  job = g_object_new (G_VFS_TYPE_JOB_MOVE,
		      "message", message,
		      "connection", connection,
		      NULL);

  job->source = g_strndup (path1_data, path1_len);
  job->destination = g_strndup (path2_data, path2_len);
  job->backend = backend;
  job->flags = flags;
  if (strcmp (callback_obj_path, "/org/gtk/vfs/void") != 0)
    job->callback_obj_path = g_strdup (callback_obj_path);
  
  return G_VFS_JOB (job);
}

void
g_vfs_job_move_progress_callback (goffset current_num_bytes,
				  goffset total_num_bytes,
				  GVfsJob *job)
{
  GVfsJobDBus *dbus_job = G_VFS_JOB_DBUS (job);
  GVfsJobMove *op_job = G_VFS_JOB_MOVE (job);
  dbus_uint64_t current_dbus, total_dbus;
  DBusMessage *message;

  g_debug ("progress_callback %" G_GOFFSET_FORMAT "/%" G_GOFFSET_FORMAT "\n", current_num_bytes, total_num_bytes);

  if (op_job->callback_obj_path == NULL)
    return;

  message =
    dbus_message_new_method_call (dbus_message_get_sender (dbus_job->message),
				  op_job->callback_obj_path,
				  G_VFS_DBUS_PROGRESS_INTERFACE,
				  G_VFS_DBUS_PROGRESS_OP_PROGRESS);
  dbus_message_set_no_reply (message, TRUE);

  current_dbus = current_num_bytes;
  total_dbus = total_num_bytes;
  dbus_message_append_args (message,
			    DBUS_TYPE_UINT64, &current_dbus,
			    DBUS_TYPE_UINT64, &total_dbus,
			    0);

  /* Queues reply (threadsafely), actually sends it in mainloop */
  dbus_connection_send (dbus_job->connection, message, NULL);
  dbus_message_unref (message);
}

static void
run (GVfsJob *job)
{
  GVfsJobMove *op_job = G_VFS_JOB_MOVE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->move == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported by backend"));
      return;
    }
  
  class->move (op_job->backend,
	       op_job,
	       op_job->source,
	       op_job->destination,
	       op_job->flags,
	       (GFileProgressCallback)g_vfs_job_move_progress_callback,
	       job);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobMove *op_job = G_VFS_JOB_MOVE (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);

  if (class->try_move == NULL)
    return FALSE;
  
  return class->try_move (op_job->backend,
			  op_job,
			  op_job->source,
			  op_job->destination,
			  op_job->flags,
			  (GFileProgressCallback)g_vfs_job_move_progress_callback,
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
