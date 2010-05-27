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
#include "gvfsjobenumerate.h"
#include "gvfsdbusutils.h"
#include "gvfsdaemonprotocol.h"

G_DEFINE_TYPE (GVfsJobEnumerate, g_vfs_job_enumerate, G_VFS_TYPE_JOB_DBUS)

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
  g_file_attribute_matcher_unref (job->attribute_matcher);
  g_free (job->object_path);
  g_free (job->uri);
  
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
  char *attributes, *uri;
  dbus_uint32_t flags;
  DBusMessageIter iter;
  
  dbus_message_iter_init (message, &iter);
  dbus_error_init (&derror);
  if (!_g_dbus_message_iter_get_args (&iter, &derror, 
				      DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
				      &path_data, &path_len,
				      DBUS_TYPE_STRING, &obj_path,
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

  /* Optional uri arg for thumbnail info */
  if (!_g_dbus_message_iter_get_args (&iter, NULL,
				      DBUS_TYPE_STRING, &uri,
				      0))
    uri = NULL;

  job = g_object_new (G_VFS_TYPE_JOB_ENUMERATE,
		      "message", message,
		      "connection", connection,
		      NULL);
  
  job->object_path = g_strdup (obj_path);
  job->filename = g_strndup (path_data, path_len);
  job->backend = backend;
  job->attributes = g_strdup (attributes);
  job->attribute_matcher = g_file_attribute_matcher_new (attributes);
  job->flags = flags;
  job->uri = g_strdup (uri);
  
  return G_VFS_JOB (job);
}

static void
send_infos (GVfsJobEnumerate *job)
{
  if (!dbus_message_iter_close_container (&job->building_iter, &job->building_array_iter))
    _g_dbus_oom ();
  
  dbus_connection_send (g_vfs_job_dbus_get_connection (G_VFS_JOB_DBUS (job)),
			job->building_infos, NULL);
  dbus_message_unref (job->building_infos);
  job->building_infos = NULL;
  job->n_building_infos = 0;
}

void
g_vfs_job_enumerate_add_info (GVfsJobEnumerate *job,
			      GFileInfo *info)
{
  DBusMessage *message, *orig_message;
  char *uri, *escaped_name;
  
  if (job->building_infos == NULL)
    {
      orig_message = g_vfs_job_dbus_get_message (G_VFS_JOB_DBUS (job));
      
      message = dbus_message_new_method_call (dbus_message_get_sender (orig_message),
					      job->object_path,
					      G_VFS_DBUS_ENUMERATOR_INTERFACE,
					      G_VFS_DBUS_ENUMERATOR_OP_GOT_INFO);
      dbus_message_set_no_reply (message, TRUE);
      
      dbus_message_iter_init_append (message, &job->building_iter);
      
      if (!dbus_message_iter_open_container (&job->building_iter,
					     DBUS_TYPE_ARRAY,
					     G_FILE_INFO_TYPE_AS_STRING, 
					     &job->building_array_iter))
	_g_dbus_oom ();

      job->building_infos = message;
      job->n_building_infos = 0;
    }

  

  uri = NULL;
  if (job->uri != NULL &&
      g_file_info_get_name (info) != NULL)
    {
      escaped_name = g_uri_escape_string (g_file_info_get_name (info),
					  G_URI_RESERVED_CHARS_ALLOWED_IN_PATH,
					  FALSE);
      uri = g_build_path ("/", job->uri, escaped_name, NULL);
      g_free (escaped_name);
    }
  
  g_vfs_backend_add_auto_info (job->backend,
			       job->attribute_matcher,
			       info,
			       uri);
  g_free (uri);

  g_file_info_set_attribute_mask (info, job->attribute_matcher);
  
  _g_dbus_append_file_info (&job->building_array_iter, info);
  job->n_building_infos++;

  if (job->n_building_infos == 50)
    send_infos (job);
}

void
g_vfs_job_enumerate_add_infos (GVfsJobEnumerate *job,
			       const GList *infos)
{
  const GList *l;
  GFileInfo *info;

  for (l = infos; l != NULL; l = l->next)
    {
      info = l->data;
      g_vfs_job_enumerate_add_info (job, info);
    }
}

void
g_vfs_job_enumerate_done (GVfsJobEnumerate *job)
{
  DBusMessage *message, *orig_message;
  
  g_assert (!G_VFS_JOB (job)->failed);

  if (job->building_infos != NULL)
    send_infos (job);
  
  orig_message = g_vfs_job_dbus_get_message (G_VFS_JOB_DBUS (job));
  
  message = dbus_message_new_method_call (dbus_message_get_sender (orig_message),
					  job->object_path,
					  G_VFS_DBUS_ENUMERATOR_INTERFACE,
					  G_VFS_DBUS_ENUMERATOR_OP_DONE);
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
  
  if (class->enumerate == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported by backend"));
      return;
    }
  
  class->enumerate (op_job->backend,
		    op_job,
		    op_job->filename,
		    op_job->attribute_matcher,
		    op_job->flags);
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
			       op_job->attribute_matcher,
			       op_job->flags);
}

static void
send_reply (GVfsJob *job)
{
  GVfsJobDBus *dbus_job = G_VFS_JOB_DBUS (job);
  DBusMessage *reply;
  GVfsJobDBusClass *class;

  g_debug ("send_reply(%p), failed=%d (%s)\n", job, job->failed, job->failed?job->error->message:"");
  
  class = G_VFS_JOB_DBUS_GET_CLASS (job);
  
  if (job->failed) 
    reply = _dbus_message_new_from_gerror (dbus_job->message, job->error);
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
  DBusMessage *reply;

  reply = dbus_message_new_method_return (message);

  return reply;
}
