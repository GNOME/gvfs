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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
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
#include "gvfshttpinputstream.h"
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
#include "gvfsdaemonutils.h"

static SoupSession *the_session;

G_DEFINE_TYPE (GVfsBackendHttp, g_vfs_backend_http, G_VFS_TYPE_BACKEND)

static void
g_vfs_backend_http_finalize (GObject *object)
{
  GVfsBackendHttp *backend;

  backend = G_VFS_BACKEND_HTTP (object);

  if (backend->mount_base)
    soup_uri_free (backend->mount_base);

  g_object_unref (backend->session);


  if (G_OBJECT_CLASS (g_vfs_backend_http_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_http_parent_class)->finalize) (object);
}

static void
g_vfs_backend_http_init (GVfsBackendHttp *backend)
{
  g_vfs_backend_set_user_visible (G_VFS_BACKEND (backend), FALSE);

  backend->session = g_object_ref (the_session);
}

/* ************************************************************************* */
/* public utility functions */

SoupURI *
http_backend_get_mount_base (GVfsBackend *backend)
{
  return  G_VFS_BACKEND_HTTP (backend)->mount_base;
}

char *
http_path_get_basename (const char *path)
{
  const char *parent;
  char       *basename;
  size_t      len;

  if (path == NULL || *path == '\0')
    return NULL;

  /* remove any leading slashes */
  while (*path != '\0' && *path == '/')
    path++;

  len = strlen (path);
  if (len == 0)
    return g_strdup ("/");

  /* remove any trailing slashes */
  while (len)
    {
      char c = path[len - 1];
      if (c != '/')
	break;

      len--;
    }

  parent = g_strrstr_len (path, len, "/");

  if (parent)
    {
      parent++; /* skip the found / char */
      basename = g_strndup (parent, (len - (parent - path)));
    }
  else
    basename = g_strndup (path, len);

  return basename;
}

char *
http_uri_get_basename (const char *uri_str)
{
  char *decoded;
  char *basename;

  basename = http_path_get_basename (uri_str);

  decoded = soup_uri_decode (basename);
  g_free (basename);

  return decoded;
}

int
http_error_code_from_status (guint status)
{
  switch (status) {

  case SOUP_STATUS_CANT_RESOLVE:
  case SOUP_STATUS_CANT_RESOLVE_PROXY:
    return G_IO_ERROR_HOST_NOT_FOUND;

  case SOUP_STATUS_CANCELLED:
    return G_IO_ERROR_CANCELLED;

  case SOUP_STATUS_UNAUTHORIZED:
  case SOUP_STATUS_PAYMENT_REQUIRED:
  case SOUP_STATUS_FORBIDDEN:
    return G_IO_ERROR_PERMISSION_DENIED;

  case SOUP_STATUS_NOT_FOUND:
  case SOUP_STATUS_GONE:
    return G_IO_ERROR_NOT_FOUND;

  case SOUP_STATUS_GATEWAY_TIMEOUT:
  case SOUP_STATUS_REQUEST_TIMEOUT:
    return G_IO_ERROR_TIMED_OUT;

  case SOUP_STATUS_NOT_IMPLEMENTED:
    return G_IO_ERROR_NOT_SUPPORTED;

  case SOUP_STATUS_INSUFFICIENT_STORAGE:
    return G_IO_ERROR_NO_SPACE;

  }

  return G_IO_ERROR_FAILED;
}


void
http_job_failed (GVfsJob *job, SoupMessage *msg)
{
  switch (msg->status_code) {

  case SOUP_STATUS_NOT_FOUND:
    g_vfs_job_failed_literal (job, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              msg->reason_phrase);
    break;

  case SOUP_STATUS_UNAUTHORIZED:
  case SOUP_STATUS_PAYMENT_REQUIRED:
  case SOUP_STATUS_FORBIDDEN:
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                      _("HTTP Client Error: %s"), msg->reason_phrase);
    break;
  default:
    g_vfs_job_failed (job, G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("HTTP Error: %s"), msg->reason_phrase);
  }
}

guint
http_backend_send_message (GVfsBackend *backend,
                           SoupMessage *msg)
{
  GVfsBackendHttp *op_backend = G_VFS_BACKEND_HTTP (backend);

  return soup_session_send_message (op_backend->session, msg);
}

void
http_backend_queue_message (GVfsBackend         *backend,
                            SoupMessage         *msg,
                            SoupSessionCallback  callback,
                            gpointer             user_data)
{
  GVfsBackendHttp *op_backend = G_VFS_BACKEND_HTTP (backend);

  soup_session_queue_message (op_backend->session, msg,
                              callback, user_data);
}
/* ************************************************************************* */
/* virtual functions overrides */

static gboolean
try_mount (GVfsBackend  *backend,
           GVfsJobMount *job,
           GMountSpec   *mount_spec,
           GMountSource *mount_source,
           gboolean      is_automount)
{
  GVfsBackendHttp *op_backend;
  const char      *uri_str;
  char            *path;
  SoupURI         *uri;
  GMountSpec      *real_mount_spec;

  op_backend = G_VFS_BACKEND_HTTP (backend);

  uri = NULL;
  uri_str = g_mount_spec_get (mount_spec, "uri");

  if (uri_str)
    uri = soup_uri_new (uri_str);

  g_debug ("+ try_mount: %s\n", uri_str ? uri_str : "(null)");

  if (uri == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Invalid mount spec"));
      return TRUE;
    }

  real_mount_spec = g_mount_spec_new ("http");
  g_mount_spec_set (real_mount_spec, "uri", uri_str);

  if (uri->path != NULL)
    {
      path = g_uri_unescape_string (uri->path, "/");
      g_free (real_mount_spec->mount_prefix);
      real_mount_spec->mount_prefix = g_mount_spec_canonicalize_path (path);
      g_free (path);
    }

  g_vfs_backend_set_mount_spec (backend, real_mount_spec);

  op_backend->mount_base = uri;

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
  SoupMessage  *msg;
  gboolean      res;
  gboolean      can_seek;
  GError       *error;

  stream = G_INPUT_STREAM (source_object);
  error  = NULL;
  job    = G_VFS_JOB (user_data);

  res = g_vfs_http_input_stream_send_finish (stream,
					     result,
					     &error);
  if (res == FALSE)
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                error->domain,
                                error->code,
                                error->message);

      g_error_free (error);
      g_object_unref (stream);
      return;
    }

  msg = g_vfs_http_input_stream_get_message (stream);
  if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
      http_job_failed (G_VFS_JOB (job), msg);
      g_object_unref (msg);
      g_object_unref (stream);
      return;
    }
  g_object_unref (msg);

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
  SoupURI *uri;

  uri = http_backend_get_mount_base (backend);
  http_backend_open_for_read (backend, G_VFS_JOB (job), uri);

  return TRUE;
}

void
http_backend_open_for_read (GVfsBackend *backend,
			    GVfsJob     *job,
			    SoupURI     *uri)
{
  GVfsBackendHttp *op_backend;
  GInputStream    *stream;

  op_backend = G_VFS_BACKEND_HTTP (backend);

  stream = g_vfs_http_input_stream_new (op_backend->session, uri);

  g_vfs_http_input_stream_send_async (stream,
				      G_PRIORITY_DEFAULT,
				      job->cancellable,
				      open_for_read_ready,
				      job);
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
     g_vfs_job_failed_literal (G_VFS_JOB (job),
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
  GInputStream    *stream;

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

static gboolean
try_seek_on_read (GVfsBackend *backend,
                  GVfsJobSeekRead *job,
                  GVfsBackendHandle handle,
                  goffset    offset,
                  GSeekType  type)
{
  GInputStream    *stream;
  GError          *error = NULL;

  stream = G_INPUT_STREAM (handle);

  if (!g_seekable_seek (G_SEEKABLE (stream), offset, type,
                        G_VFS_JOB (job)->cancellable, &error))
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                error->domain,
                                error->code,
                                error->message);
      g_error_free (error);
      return FALSE;
    }
  else
    {
      g_vfs_job_seek_read_set_offset (job, g_seekable_tell (G_SEEKABLE (stream)));
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }

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
  GError       *error = NULL;
  gboolean      res;

  job = G_VFS_JOB (user_data);
  stream = G_INPUT_STREAM (source_object);
  res = g_input_stream_close_finish (stream,
                                     result,
                                     &error);
  if (res == FALSE)
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job),
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
  GInputStream    *stream;

  stream = G_INPUT_STREAM (handle);

  g_input_stream_close_async (stream,
                              G_PRIORITY_DEFAULT,
                              G_VFS_JOB (job)->cancellable,
                              close_read_ready,
                              job);
  return TRUE;
}


/* *** query_info () *** */

static void
file_info_from_message (SoupMessage *msg,
                        GFileInfo *info,
                        GFileAttributeMatcher *matcher)
{
  const char *text;
  GHashTable *params;
  char       *basename;
  char       *ed_name;

  basename = ed_name = NULL;

  /* prefer the filename from the Content-Disposition (rfc2183) header
     if one if present. See bug 551298. */
  if (soup_message_headers_get_content_disposition (msg->response_headers,
                                                    NULL, &params))
    {
      const char *name = g_hash_table_lookup (params, "filename");

      if (name)
        basename = g_strdup (name);

      g_hash_table_destroy (params);
    }

  if (basename == NULL)
    {
      const SoupURI *uri;

      uri = soup_message_get_uri (msg);
      basename = http_uri_get_basename (uri->path);
    }

  g_debug ("basename:%s\n", basename);

  /* read http/1.1 rfc, until then we copy the local files
   * behaviour */
  if (basename != NULL &&
      (g_file_attribute_matcher_matches (matcher,
                                         G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME) ||
       g_file_attribute_matcher_matches (matcher,
                                         G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME)))
    ed_name = gvfs_file_info_populate_names_as_local (info, basename);

  g_free (basename);
  g_free (ed_name);

  if (soup_message_headers_get_encoding (msg->response_headers) == SOUP_ENCODING_CONTENT_LENGTH)
    {
      goffset start, end, length;
      gboolean ret;

      ret = soup_message_headers_get_content_range (msg->response_headers,
                                                    &start, &end, &length);
      if (ret && length != -1)
        {
          g_file_info_set_size (info, length);
        }
      else if (!ret)
        {
          length = soup_message_headers_get_content_length (msg->response_headers);
          g_file_info_set_size (info, length);
        }
    }

  g_file_info_set_file_type (info, G_FILE_TYPE_REGULAR);

  text = soup_message_headers_get_content_type (msg->response_headers, NULL);
  if (text)
    {
      GIcon *icon;

      g_file_info_set_content_type (info, text);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, text);

      icon = g_content_type_get_icon (text);
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);

      icon = g_content_type_get_symbolic_icon (text);
      g_file_info_set_symbolic_icon (info, icon);
      g_object_unref (icon);
    }


  text = soup_message_headers_get_one (msg->response_headers,
                                       "Last-Modified");
  if (text)
    {
      SoupDate *sd;
      GTimeVal tv;

      sd = soup_date_new_from_string(text);
      if (sd)
        {
          soup_date_to_timeval (sd, &tv);
	  g_file_info_set_modification_time (info, &tv);
          soup_date_free (sd);
        }
    }


  text = soup_message_headers_get_one (msg->response_headers,
                                       "ETag");
  if (text)
    {
      g_file_info_set_attribute_string (info,
                                        G_FILE_ATTRIBUTE_ETAG_VALUE,
                                        text);
    }
}

static void
query_info_ready (SoupSession *session,
                  SoupMessage *msg,
                  gpointer     user_data)
{
  GFileAttributeMatcher *matcher;
  GVfsJobQueryInfo      *job;
  GFileInfo             *info;

  job     = G_VFS_JOB_QUERY_INFO (user_data);
  info    = job->file_info;
  matcher = job->attribute_matcher;

  if (! SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
      http_job_failed (G_VFS_JOB (job), msg);
      return;
    }

  file_info_from_message (msg, info, matcher);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}


static gboolean
try_query_info (GVfsBackend           *backend,
                GVfsJobQueryInfo      *job,
                const char            *filename,
                GFileQueryInfoFlags    flags,
                GFileInfo             *info,
                GFileAttributeMatcher *attribute_matcher)
{
  SoupMessage *msg;
  SoupURI     *uri;

  if (g_file_attribute_matcher_matches_only (attribute_matcher,
                                             G_FILE_ATTRIBUTE_THUMBNAIL_PATH))
    {
      g_vfs_job_succeeded (G_VFS_JOB (job));
      return TRUE;
    }

  uri = http_backend_get_mount_base (backend);
  msg = soup_message_new_from_uri (SOUP_METHOD_HEAD, uri);

  http_backend_queue_message (backend, msg, query_info_ready, job);

  return TRUE;
}


static gboolean
try_query_info_on_read (GVfsBackend           *backend,
                        GVfsJobQueryInfoRead  *job,
                        GVfsBackendHandle      handle,
                        GFileInfo             *info,
                        GFileAttributeMatcher *attribute_matcher)
{
    SoupMessage *msg = g_vfs_http_input_stream_get_message (G_INPUT_STREAM (handle));

    file_info_from_message (msg, info, attribute_matcher);
    g_object_unref (msg);

    g_vfs_job_succeeded (G_VFS_JOB (job));

    return TRUE;
}

static gboolean
try_query_fs_info (GVfsBackend *backend,
                   GVfsJobQueryFsInfo *job,
                   const char *filename,
                   GFileInfo *info,
                   GFileAttributeMatcher *matcher)
{
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "http");
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, TRUE);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
}


#define DEBUG_MAX_BODY_SIZE (100 * 1024 * 1024)

static void
g_vfs_backend_http_class_init (GVfsBackendHttpClass *klass)
{
  const char         *debug;
  SoupSessionFeature *cookie_jar;

  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class;

  gobject_class->finalize  = g_vfs_backend_http_finalize;

  backend_class = G_VFS_BACKEND_CLASS (klass);

  backend_class->try_mount              = try_mount;
  backend_class->try_open_for_read      = try_open_for_read;
  backend_class->try_read               = try_read;
  backend_class->try_seek_on_read       = try_seek_on_read;
  backend_class->try_close_read         = try_close_read;
  backend_class->try_query_info         = try_query_info;
  backend_class->try_query_info_on_read = try_query_info_on_read;
  backend_class->try_query_fs_info      = try_query_fs_info;

  /* Initialize the SoupSession, common to all backend instances */
  the_session = soup_session_new_with_options ("user-agent",
                                               "gvfs/" VERSION,
                                               NULL);

  g_object_set (the_session, "ssl-strict", FALSE, NULL);

  /* Cookie handling - stored temporarlly in memory, mostly useful for
   * authentication in WebDAV. */
  cookie_jar = g_object_new (SOUP_TYPE_COOKIE_JAR, NULL);
  soup_session_add_feature (the_session, cookie_jar);
  g_object_unref (cookie_jar);

  /* Send Accept-Language header (see bug 166795) */
  g_object_set (the_session, "accept-language-auto", TRUE, NULL);

  /* Prevent connection timeouts during long operations like COPY. */
  g_object_set (the_session, "timeout", 0, NULL);

  /* Logging */
  debug = g_getenv ("GVFS_HTTP_DEBUG");
  if (debug)
    {
      SoupLogger         *logger;
      SoupLoggerLogLevel  level;

      if (g_ascii_strcasecmp (debug, "all") == 0 ||
          g_ascii_strcasecmp (debug, "body") == 0)
        level = SOUP_LOGGER_LOG_BODY;
      else if (g_ascii_strcasecmp (debug, "header") == 0)
        level = SOUP_LOGGER_LOG_HEADERS;
      else
        level = SOUP_LOGGER_LOG_MINIMAL;

      logger = soup_logger_new (level, DEBUG_MAX_BODY_SIZE);
      soup_session_add_feature (the_session, SOUP_SESSION_FEATURE (logger));
      g_object_unref (logger);
    }
}
