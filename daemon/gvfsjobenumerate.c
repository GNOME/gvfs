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
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"
#include <gvfsdbus.h>

G_DEFINE_TYPE (GVfsJobEnumerate, g_vfs_job_enumerate, G_VFS_TYPE_JOB_DBUS)

static void         run        (GVfsJob        *job);
static gboolean     try        (GVfsJob        *job);
static void         send_reply   (GVfsJob        *job);
static void         create_reply (GVfsJob               *job,
                                  GVfsDBusMount         *object,
                                  GDBusMethodInvocation *invocation);

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

gboolean 
g_vfs_job_enumerate_new_handle (GVfsDBusMount *object,
                                GDBusMethodInvocation *invocation,
                                const gchar *arg_path_data,
                                const gchar *arg_obj_path,
                                const gchar *arg_attributes,
                                guint arg_flags,
                                const gchar *arg_uri,
                                GVfsBackend *backend)
{
  GVfsJobEnumerate *job;

  if (g_vfs_backend_invocation_first_handler (object, invocation, backend))
    return TRUE;
  
  job = g_object_new (G_VFS_TYPE_JOB_ENUMERATE,
                      "object", object,
                      "invocation", invocation,
                      NULL);
  
  job->object_path = g_strdup (arg_obj_path);
  job->filename = g_strdup (arg_path_data);
  job->backend = backend;
  job->attributes = g_strdup (arg_attributes);
  job->attribute_matcher = g_file_attribute_matcher_new (arg_attributes);
  job->flags = arg_flags;
  job->uri = g_strdup (arg_uri);

  g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), G_VFS_JOB (job));
  g_object_unref (job);

  return TRUE;
}

static GVfsDBusEnumerator *
create_enumerator_proxy (GVfsJobEnumerate *job)
{
  GDBusConnection *connection;
  const gchar *sender;
  GVfsDBusEnumerator *proxy;

  connection = g_dbus_method_invocation_get_connection (G_VFS_JOB_DBUS (job)->invocation);
  sender = g_dbus_method_invocation_get_sender (G_VFS_JOB_DBUS (job)->invocation);

  proxy = gvfs_dbus_enumerator_proxy_new_sync (connection,
                                               G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                               sender,
                                               job->object_path,
                                               NULL,
                                               NULL);
  g_assert (proxy != NULL);
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), G_MAXINT);

  return proxy;
}

static void
send_infos_cb (GVfsDBusEnumerator *proxy,
               GAsyncResult *res,
               gpointer user_data)
{
  GError *error = NULL;
  
  gvfs_dbus_enumerator_call_got_info_finish (proxy, res, &error);
  if (error != NULL)
    {
      g_dbus_error_strip_remote_error (error);
      g_debug ("send_infos_cb: %s (%s, %d)\n", error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }
}

static void
send_infos (GVfsJobEnumerate *job)
{
  GVfsDBusEnumerator *proxy;

  proxy = create_enumerator_proxy (job);
  
  gvfs_dbus_enumerator_call_got_info (proxy,
                                      g_variant_builder_end (job->building_infos),
                                      NULL,
                                      (GAsyncReadyCallback) send_infos_cb,
                                      NULL);
  g_object_unref (proxy);

  g_variant_builder_unref (job->building_infos);
  job->building_infos = NULL;
  job->n_building_infos = 0;
}

void
g_vfs_job_enumerate_add_info (GVfsJobEnumerate *job,
			      GFileInfo *info)
{
  char *uri, *escaped_name;
  GVariant *v;
  
  if (job->building_infos == NULL)
    {
      job->building_infos = g_variant_builder_new (G_VARIANT_TYPE ("aa(suv)"));
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

  v = _g_dbus_append_file_info (info);
  g_variant_builder_add_value (job->building_infos, v);
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

static void
send_done_cb (GVfsDBusEnumerator *proxy,
               GAsyncResult *res,
               gpointer user_data)
{
  GError *error = NULL;

  gvfs_dbus_enumerator_call_done_finish (proxy, res, &error);
  if (error != NULL)
    {
      g_dbus_error_strip_remote_error (error);
      g_debug ("send_done_cb: %s (%s, %d)\n", error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }
}

void
g_vfs_job_enumerate_done (GVfsJobEnumerate *job)
{
  GVfsDBusEnumerator *proxy;
  
  g_assert (!G_VFS_JOB (job)->failed);

  if (job->building_infos != NULL)
    send_infos (job);

  proxy = create_enumerator_proxy (job);
  
  gvfs_dbus_enumerator_call_done (proxy,
                                  NULL,
                                  (GAsyncReadyCallback) send_done_cb,
                                  NULL);
  g_object_unref (proxy);

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
			_("Operation not supported"));
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
  GVfsJobDBusClass *class;

  g_debug ("send_reply(%p), failed=%d (%s)\n", job, job->failed,
           job->failed ? job->error->message : "");
  
  class = G_VFS_JOB_DBUS_GET_CLASS (job);
  
  if (job->failed)
    g_dbus_method_invocation_return_gerror (dbus_job->invocation, job->error);
  else
    class->create_reply (job, dbus_job->object, dbus_job->invocation);
 
  if (job->failed)
    g_vfs_job_emit_finished (job);
}

/* Might be called on an i/o thread */
static void
create_reply (GVfsJob *job,
              GVfsDBusMount *object,
              GDBusMethodInvocation *invocation)
{
  gvfs_dbus_mount_complete_enumerate (object, invocation);
}
