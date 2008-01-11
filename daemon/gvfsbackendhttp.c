/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2008 Red Hat, Inc.
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
 * Author: Christian Kellner <gicmo@gnome.org>
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include <libsoup/soup.h>

#include "gvfsbackendhttp.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsdaemonprotocol.h"

#include "soup-input-stream.h"


struct _GVfsBackendHttp
{
  GVfsBackend parent_instance;

  SoupUri     *mount_base;
  SoupSession *session;
};

G_DEFINE_TYPE (GVfsBackendHttp, g_vfs_backend_http, G_VFS_TYPE_BACKEND);

static void
g_vfs_backend_http_finalize (GObject *object)
{
  GVfsBackendHttp *backend;

  backend = G_VFS_BACKEND_HTTP (object);

  if (G_OBJECT_CLASS (g_vfs_backend_http_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_http_parent_class)->finalize) (object);
}

static void
g_vfs_backend_http_init (GVfsBackendHttp *backend)
{
}

static gboolean
try_mount (GVfsBackend  *backend,
           GVfsJobMount *job,
           GMountSpec   *mount_spec,
           GMountSource *mount_source,
           gboolean      is_automount)
{
  GVfsBackendHttp *op_backend;
  const char      *uri_str;
  SoupUri         *uri;

  op_backend = G_VFS_BACKEND_HTTP (backend);

  uri = NULL;
  uri_str = g_mount_spec_get (mount_spec, "uri");

  if (uri_str)
    uri = soup_uri_new (uri_str);

  g_print ("+ try_mount: %s\n", uri_str ? uri_str : "(null)");

  if (uri == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
			G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			_("Invalid mount spec"));
      return TRUE;
    }

  op_backend->mount_base = uri;

  op_backend->session = soup_session_async_new_with_options (SOUP_SESSION_ASYNC_CONTEXT,
                                                             g_main_context_default (),
                                                             NULL);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
}

/* *** open_read () *** */
static void
open_for_read_ready (GObject      *source_object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  GInputStream *stream;
  GVfsJob      *job;
  gboolean      res;
  gboolean      can_seek;
  GError       *error;

  stream = G_INPUT_STREAM (source_object); 
  error  = NULL;
  job    = G_VFS_JOB (user_data);

  res = soup_input_stream_send_finish (stream,
                                       result,
                                       &error);

  if (res == FALSE)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        error->domain,
                        error->code,
                        error->message);

      g_error_free (error);
      return;
    }

  can_seek = G_IS_SEEKABLE (stream) && g_seekable_can_seek (G_SEEKABLE (stream));

  g_vfs_job_open_for_read_set_can_seek (G_VFS_JOB_OPEN_FOR_READ (job), can_seek);
  g_vfs_job_open_for_read_set_handle (G_VFS_JOB_OPEN_FOR_READ (job), stream);
  g_vfs_job_succeeded (job);
}

static gboolean 
try_open_for_read (GVfsBackend        *backend,
                   GVfsJobOpenForRead *job,
                   const char         *filename)
{
  GVfsBackendHttp *op_backend;
  GInputStream    *stream;
  SoupUri         *uri;
  SoupMessage     *msg;

  op_backend = G_VFS_BACKEND_HTTP (backend);
  uri = soup_uri_new_with_base (op_backend->mount_base, filename);

  msg = soup_message_new_from_uri (SOUP_METHOD_GET, uri);
  soup_message_add_header (msg->request_headers, "User-Agent", "gvfs/" VERSION);

  stream = soup_input_stream_new (op_backend->session, msg);
  g_object_unref (msg);

  soup_input_stream_send_async (stream,
                                G_PRIORITY_DEFAULT,
                                G_VFS_JOB (job)->cancellable,
                                open_for_read_ready,
                                job);
  return TRUE;
}

/* *** read () *** */
static void
read_ready (GObject      *source_object,
            GAsyncResult *result,
            gpointer      user_data)
{
  GInputStream *stream;
  GVfsJob      *job;
  GError       *error;
  gssize        nread;

  stream = G_INPUT_STREAM (source_object); 
  error  = NULL;
  job    = G_VFS_JOB (user_data);

  nread = g_input_stream_read_finish (stream, result, &error);

  if (nread < 0)
   {
     g_vfs_job_failed (G_VFS_JOB (job),
                       error->domain,
                       error->code,
                       error->message);

     g_error_free (error);
     return;
   }

  g_vfs_job_read_set_size (G_VFS_JOB_READ (job), nread);
  g_vfs_job_succeeded (job);

}

static gboolean
try_read (GVfsBackend        *backend,
          GVfsJobRead        *job,
          GVfsBackendHandle   handle,
          char               *buffer,
          gsize               bytes_requested)
{
  GVfsBackendHttp *op_backend;
  GInputStream    *stream;

  op_backend = G_VFS_BACKEND_HTTP (backend);
  stream = G_INPUT_STREAM (handle);

  g_input_stream_read_async (stream,
                             buffer,
                             bytes_requested,
                             G_PRIORITY_DEFAULT,
                             G_VFS_JOB (job)->cancellable,
                             read_ready,
                             job);
  return TRUE;
}

/* *** read_close () *** */
static void
close_read_ready (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  GInputStream *stream;
  GVfsJob      *job;
  GError       *error;
  gboolean      res;

  job = G_VFS_JOB (user_data);
  stream = G_INPUT_STREAM (source_object);
  res = g_input_stream_close_finish (stream,
                                     result,
                                     &error);
  if (res == FALSE)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        error->domain,
                        error->code,
                        error->message);

      g_error_free (error);
    }
  else
    g_vfs_job_succeeded (job);

  g_object_unref (stream);
}

static gboolean 
try_close_read (GVfsBackend       *backend,
                GVfsJobCloseRead  *job,
                GVfsBackendHandle  handle)
{
  GVfsBackendHttp *op_backend;
  GInputStream    *stream;

  op_backend = G_VFS_BACKEND_HTTP (backend);
  stream = G_INPUT_STREAM (handle);

  g_input_stream_close_async (stream,
                              G_PRIORITY_DEFAULT,
                              G_VFS_JOB (job)->cancellable,
                              close_read_ready,
                              job);

  return TRUE;
}

/* *** query_info () *** */
static gboolean
try_query_info (GVfsBackend           *backend,
                GVfsJobQueryInfo      *job,
                const char            *filename,
                GFileQueryInfoFlags    flags,
                GFileInfo             *info,
                GFileAttributeMatcher *attribute_matcher)
{

  return TRUE;
}


static void
g_vfs_backend_http_class_init (GVfsBackendHttpClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class;
  
  gobject_class->finalize  = g_vfs_backend_http_finalize;

  backend_class = G_VFS_BACKEND_CLASS (klass); 

  backend_class->try_mount         = try_mount;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_read          = try_read;
  backend_class->try_close_read    = try_close_read;
  backend_class->try_query_info    = try_query_info;

}
