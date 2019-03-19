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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gi18n.h>
#include "gvfsdaemonprotocol.h"
#include "gvfsjobqueryattributes.h"

G_DEFINE_TYPE (GVfsJobQueryAttributes, g_vfs_job_query_attributes, G_VFS_TYPE_JOB_DBUS)

static void         run          (GVfsJob        *job);
static gboolean     try          (GVfsJob        *job);
static void         create_reply (GVfsJob               *job,
                                  GVfsDBusMount         *object,
                                  GDBusMethodInvocation *invocation);

static void
g_vfs_job_query_attributes_finalize (GObject *object)
{
  GVfsJobQueryAttributes *job;

  job = G_VFS_JOB_QUERY_ATTRIBUTES (object);

  g_free (job->filename);
  if (job->list)
    g_file_attribute_info_list_unref (job->list);
  
  if (G_OBJECT_CLASS (g_vfs_job_query_attributes_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_job_query_attributes_parent_class)->finalize) (object);
}

static void
g_vfs_job_query_attributes_class_init (GVfsJobQueryAttributesClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsJobClass *job_class = G_VFS_JOB_CLASS (klass);
  GVfsJobDBusClass *job_dbus_class = G_VFS_JOB_DBUS_CLASS (klass);
  
  gobject_class->finalize = g_vfs_job_query_attributes_finalize;
  job_class->run = run;
  job_class->try = try;
  job_dbus_class->create_reply = create_reply;
}

static void
g_vfs_job_query_attributes_init (GVfsJobQueryAttributes *job)
{
}

gboolean
g_vfs_job_query_settable_attributes_new_handle (GVfsDBusMount *object,
                                                GDBusMethodInvocation *invocation,
                                                const gchar *arg_path_data,
                                                GVfsBackend *backend)
{
  GVfsJobQueryAttributes *job;

  if (g_vfs_backend_invocation_first_handler (object, invocation, backend))
    return TRUE;
  
  job = g_object_new (G_VFS_TYPE_JOB_QUERY_ATTRIBUTES,
                      "object", object,
                      "invocation", invocation,
                      NULL);
 
  job->backend = backend;
  job->filename = g_strdup (arg_path_data);
  job->namespaces = FALSE;

  g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), G_VFS_JOB (job));
  g_object_unref (job);

  return TRUE;
}

gboolean
g_vfs_job_query_writable_namespaces_new_handle (GVfsDBusMount *object,
                                                GDBusMethodInvocation *invocation,
                                                const gchar *arg_path_data,
                                                GVfsBackend *backend)
{
  GVfsJobQueryAttributes *job;

  if (g_vfs_backend_invocation_first_handler (object, invocation, backend))
    return TRUE;
  
  job = g_object_new (G_VFS_TYPE_JOB_QUERY_ATTRIBUTES,
                      "object", object,
                      "invocation", invocation,
                      NULL);
 
  job->backend = backend;
  job->filename = g_strdup (arg_path_data);
  job->namespaces = TRUE;

  g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), G_VFS_JOB (job));
  g_object_unref (job);

  return TRUE;
}

static void
run (GVfsJob *job)
{
  GVfsJobQueryAttributes *op_job = G_VFS_JOB_QUERY_ATTRIBUTES (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  void (*cb) (GVfsBackend *backend,
	      GVfsJobQueryAttributes *job,
	      const char *filename);

  if (op_job->namespaces)
    cb = class->query_writable_namespaces;
  else
    cb = class->query_settable_attributes;

  if (cb == NULL)
    {
      g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Operation not supported"));
      return;
    }
      
  cb (op_job->backend,
      op_job,
      op_job->filename);
}

static gboolean
try (GVfsJob *job)
{
  GVfsJobQueryAttributes *op_job = G_VFS_JOB_QUERY_ATTRIBUTES (job);
  GVfsBackendClass *class = G_VFS_BACKEND_GET_CLASS (op_job->backend);
  gboolean (*cb) (GVfsBackend *backend,
		  GVfsJobQueryAttributes *job,
		  const char *filename);

  if (op_job->namespaces)
    cb = class->try_query_writable_namespaces;
  else
    cb = class->try_query_settable_attributes;

  if (cb == NULL)
    return FALSE;
      
  return cb (op_job->backend,
	     op_job,
	     op_job->filename);
}

void
g_vfs_job_query_attributes_set_list (GVfsJobQueryAttributes *job,
				     GFileAttributeInfoList *list)
{
  job->list = g_file_attribute_info_list_ref (list);
}

/* Might be called on an i/o thread */
static void
create_reply (GVfsJob *job,
              GVfsDBusMount *object,
              GDBusMethodInvocation *invocation)
{
  GVfsJobQueryAttributes *op_job = G_VFS_JOB_QUERY_ATTRIBUTES (job);
  GVariant *list;
  
  list = _g_dbus_append_attribute_info_list (op_job->list);
  
  if (! op_job->namespaces)
    gvfs_dbus_mount_complete_query_settable_attributes (object, invocation, list);
  else
    gvfs_dbus_mount_complete_query_writable_namespaces (object, invocation, list);
}
