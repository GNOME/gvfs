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
#include "soup-output-stream.h"


G_DEFINE_TYPE (GVfsBackendHttp, g_vfs_backend_http, G_VFS_TYPE_BACKEND);

static void
g_vfs_backend_http_finalize (GObject *object)
{
  GVfsBackendHttp *backend;

  backend = G_VFS_BACKEND_HTTP (object);

  if (backend->mount_base)
    soup_uri_free (backend->mount_base);

  soup_session_abort (backend->session);
  g_object_unref (backend->session);

  if (G_OBJECT_CLASS (g_vfs_backend_http_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_http_parent_class)->finalize) (object);
}

static void
g_vfs_backend_http_init (GVfsBackendHttp *backend)
{
  g_vfs_backend_set_user_visible (G_VFS_BACKEND (backend), FALSE);  

  backend->session = soup_session_async_new ();
}

SoupURI *
g_vfs_backend_uri_for_filename (GVfsBackend *backend, const char *filename)
{
  GVfsBackendHttp *op_backend;
  SoupURI         *uri;
  char            *path;

  op_backend = G_VFS_BACKEND_HTTP (backend);
  uri = soup_uri_copy (op_backend->mount_base);

  /* "/" means "whatever mount_base is" */
  if (!strcmp (filename, "/"))
    return uri;

  /* Otherwise, we append filename to mount_base (which is assumed to
   * be a directory in this case).
   */
  path = g_build_path ("/", uri->path, filename, NULL);
  soup_uri_set_path (uri, path);
  g_free (path);

  return uri;
}

char *
uri_get_basename (const char *uri_str)
{
    const char *parent;
    const char *path;
    char       *to_free;
    char       *basename;
    size_t      len;

    if (uri_str == NULL || *uri_str == '\0')
      return NULL;

    path =  uri_str;

    /* remove any leading slashes */
    while (*path == '/' || *path == ' ')
        path++;

    len = strlen (path);

    if (len == 0)
      return g_strdup ("/");

    /* remove any trailing slashes */
    while (path[len - 1] == '/' || path[len - 1] == ' ')
        len--;

    parent = g_strrstr_len (path, len, "/");

    if (parent)
      {
        parent++; /* skip the found / char */
        to_free = g_strndup (parent, (len - (parent - path)));
      }
    else
      to_free = g_strndup (path, len);

    basename = soup_uri_decode (to_free);
    g_free (to_free);

    return basename;
}

/* ************************************************************************* */
/*  */

typedef void (*StatCallback) (GFileInfo *info, gpointer user_data, GError *error);

typedef struct _StatData {

  StatCallback callback;
  gpointer     user_data;

  GFileQueryInfoFlags    flags;
  GFileAttributeMatcher *matcher;

} StatData;


static void
gvfs_error_from_http_status (GError **error, guint status_code, const char *message)
{
  switch (status_code) {

    case SOUP_STATUS_NOT_FOUND:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   message);
      break;

    case SOUP_STATUS_UNAUTHORIZED:
    case SOUP_STATUS_PAYMENT_REQUIRED:
    case SOUP_STATUS_FORBIDDEN:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                   _("HTTP Client Error: %s"), message);
      break;
    default:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("HTTP Error: %s"), message);
  }
}

static void 
stat_location_ready (SoupSession *session,
                     SoupMessage *msg,
                     gpointer     user_data)
{
  GFileInfo  *info;
  StatData   *data;
  GError     *error;
  const char *text;

  data  = (StatData *) user_data; 
  info  = NULL;
  error = NULL;

  if (SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
      char          *basename;
      const SoupURI *uri;

      info = g_file_info_new ();

      uri = soup_message_get_uri (msg);
      basename = uri_get_basename (uri->path);
      
      g_print ("basename:%s\n", basename);

      /* read http/1.1 rfc, until then we copy the local files
       * behaviour */ 
      if (basename != NULL &&
          g_file_attribute_matcher_matches (data->matcher,
                                            G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME))
        {
          char *display_name = g_filename_display_name (basename);

          if (strstr (display_name, "\357\277\275") != NULL)
            {
              char *p = display_name;
              display_name = g_strconcat (display_name, _(" (invalid encoding)"), NULL);
              g_free (p);
            }

          g_file_info_set_display_name (info, display_name);
          g_free (display_name);
        }

      if (basename != NULL &&
          g_file_attribute_matcher_matches (data->matcher,
                                            G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME))
        {
          char *edit_name = g_filename_display_name (basename);
          g_file_info_set_edit_name (info, edit_name);
          g_free (edit_name);
        } 

      g_free (basename);

      text = soup_message_headers_get (msg->response_headers,
                                       "Content-Length");
      if (text)
        {
          guint64 size = g_ascii_strtoull (text, NULL, 10);
          g_file_info_set_size (info, size);
        }


      text = soup_message_headers_get (msg->response_headers,
                                       "Content-Type");
      if (text)
        {
          char *p = strchr (text, ';');

          if (p != NULL)
            {
              char *tmp = g_strndup (text, p - text);
              g_file_info_set_content_type (info, tmp);
              g_free (tmp);
            }
          else
            g_file_info_set_content_type (info, text);
        }


      text = soup_message_headers_get (msg->response_headers,
                                       "Last-Modified");
      if (text)
        {
          GTimeVal tv;
          if (g_time_val_from_iso8601 (text, &tv))
            g_file_info_set_modification_time (info, &tv);
        }

      text = soup_message_headers_get (msg->response_headers,
                                       "ETag");
      if (text)
        {
          g_file_info_set_attribute_string (info,
                                            G_FILE_ATTRIBUTE_ETAG_VALUE,
                                            text);
        }
    }
  else
    {
      gvfs_error_from_http_status (&error, msg->status_code,
                                   msg->reason_phrase);
    }

  data->callback (info, data->user_data, error);
  
  if (error)
    g_error_free (error);

  g_free (data);
}

static void
stat_location (GVfsBackend           *backend,
               const char            *filename,
               GFileQueryInfoFlags    flags,
               GFileAttributeMatcher *attribute_matcher,
               StatCallback           callback,
               gpointer               user_data)
{
  GVfsBackendHttp *op_backend;
  StatData        *data;
  SoupMessage     *msg;

  op_backend = G_VFS_BACKEND_HTTP (backend);

  msg = message_new_from_filename (backend, "HEAD", filename);

  data = g_new0 (StatData, 1);

  data->user_data = user_data;
  data->callback = callback;
  data->flags = flags;
  data->matcher = attribute_matcher;


  soup_session_queue_message (op_backend->session, msg,
                              stat_location_ready, data);
}

/* ************************************************************************* */
/* public utility functions */

SoupMessage *
message_new_from_uri (const char *method,
                      SoupURI    *uri)
{
  SoupMessage *msg;

  msg = soup_message_new_from_uri (method, uri);

  /* Add standard headers */
  soup_message_headers_append (msg->request_headers,
                           "User-Agent", "gvfs/" VERSION);
  return msg;
}

SoupMessage *
message_new_from_filename (GVfsBackend *backend,
                           const char  *method,
                           const char  *filename)
{
  GVfsBackendHttp *op_backend;
  SoupMessage     *msg;
  SoupURI         *uri;

  op_backend = G_VFS_BACKEND_HTTP (backend);

  uri = g_vfs_backend_uri_for_filename (backend, filename);
  msg = message_new_from_uri (method, uri);

  soup_uri_free (uri);
  return msg;
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

  g_print ("+ try_mount: %s\n", uri_str ? uri_str : "(null)");

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
  SoupMessage     *msg;

  op_backend = G_VFS_BACKEND_HTTP (backend);
  msg = message_new_from_filename (backend, "GET", filename);

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

static gboolean
try_seek_on_read (GVfsBackend *backend,
                  GVfsJobSeekRead *job,
                  GVfsBackendHandle handle,
                  goffset    offset,
                  GSeekType  type)
{
  GVfsBackendHttp *op_backend;
  GInputStream    *stream;
  GError          *error = NULL;

  op_backend = G_VFS_BACKEND_HTTP (backend);
  stream = G_INPUT_STREAM (handle);

  if (!g_seekable_seek (G_SEEKABLE (stream), offset, type,
                        G_VFS_JOB (job)->cancellable, &error))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
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

/* *** create () *** */
static void
try_create_tested_existence (SoupSession *session, SoupMessage *msg,
                             gpointer user_data)
{
  GVfsJob *job = G_VFS_JOB (user_data);
  GVfsBackendHttp *op_backend = job->backend_data;
  GOutputStream   *stream;
  SoupMessage     *put_msg;

  if (SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
      g_vfs_job_failed (job,
                        G_IO_ERROR,
                        G_IO_ERROR_EXISTS,
                        _("Target file already exists"));
      return;
    }
  /* FIXME: other errors */

  put_msg = message_new_from_uri ("PUT", soup_message_get_uri (msg));

  soup_message_headers_append (put_msg->request_headers, "If-None-Match", "*");
  stream = soup_output_stream_new (op_backend->session, put_msg, -1);
  g_object_unref (put_msg);

  g_vfs_job_open_for_write_set_handle (G_VFS_JOB_OPEN_FOR_WRITE (job), stream);
  g_vfs_job_succeeded (job);
}  

static gboolean
try_create (GVfsBackend *backend,
            GVfsJobOpenForWrite *job,
            const char *filename,
            GFileCreateFlags flags)
{
  GVfsBackendHttp *op_backend;
  SoupMessage     *msg;

  /* FIXME: if SoupOutputStream supported chunked requests, we could
   * use a PUT with "If-None-Match: *" and "Expect: 100-continue"
   */

  op_backend = G_VFS_BACKEND_HTTP (backend);

  msg = message_new_from_filename (backend, "HEAD", filename);

  g_vfs_job_set_backend_data (G_VFS_JOB (job), op_backend, NULL);
  soup_session_queue_message (op_backend->session, msg,
                              try_create_tested_existence, job);
  return TRUE;
}

/* *** replace () *** */
static void
open_for_replace_succeeded (GVfsBackendHttp *op_backend, GVfsJob *job,
                            SoupURI *uri, const char *etag)
{
  SoupMessage     *put_msg;
  GOutputStream   *stream;

  put_msg = message_new_from_uri (SOUP_METHOD_PUT, uri);

  if (etag)
    soup_message_headers_append (put_msg->request_headers, "If-Match", etag);

  stream = soup_output_stream_new (op_backend->session, put_msg, -1);
  g_object_unref (put_msg);

  g_vfs_job_open_for_write_set_handle (G_VFS_JOB_OPEN_FOR_WRITE (job), stream);
  g_vfs_job_succeeded (job);
}

static void
try_replace_checked_etag (SoupSession *session, SoupMessage *msg,
                          gpointer user_data)
{
  GVfsJob *job = G_VFS_JOB (user_data);
  GVfsBackendHttp *op_backend = job->backend_data;

  if (msg->status_code == SOUP_STATUS_PRECONDITION_FAILED)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_WRONG_ETAG,
                        _("The file was externally modified"));
      return;
    }
  /* FIXME: other errors */

  open_for_replace_succeeded (op_backend, job, soup_message_get_uri (msg),
                              soup_message_headers_get (msg->request_headers, "If-Match"));
}  

static gboolean
try_replace (GVfsBackend *backend,
             GVfsJobOpenForWrite *job,
             const char *filename,
             const char *etag,
             gboolean make_backup,
             GFileCreateFlags flags)
{
  GVfsBackendHttp *op_backend;
  SoupURI         *uri;

  /* FIXME: if SoupOutputStream supported chunked requests, we could
   * use a PUT with "If-Match: ..." and "Expect: 100-continue"
   */

  op_backend = G_VFS_BACKEND_HTTP (backend);

  if (make_backup)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_CANT_CREATE_BACKUP,
                        _("Backup file creation failed"));
      return TRUE;
    }



  uri = g_vfs_backend_uri_for_filename (backend, filename);

  if (etag)
    {
      SoupMessage *msg;

      msg = soup_message_new_from_uri (SOUP_METHOD_HEAD, uri);
      soup_uri_free (uri);
      soup_message_headers_append (msg->request_headers, "User-Agent", "gvfs/" VERSION);
      soup_message_headers_append (msg->request_headers, "If-Match", etag);

      g_vfs_job_set_backend_data (G_VFS_JOB (job), op_backend, NULL);
      soup_session_queue_message (op_backend->session, msg,
                                  try_replace_checked_etag, job);
      return TRUE;
    }

  open_for_replace_succeeded (op_backend, G_VFS_JOB (job), uri, NULL);
  soup_uri_free (uri);
  return TRUE;
}

/* *** write () *** */
static void
write_ready (GObject      *source_object,
             GAsyncResult *result,
             gpointer      user_data)
{
  GOutputStream *stream;
  GVfsJob       *job;
  GError        *error;
  gssize         nwrote;

  stream = G_OUTPUT_STREAM (source_object); 
  error  = NULL;
  job    = G_VFS_JOB (user_data);

  nwrote = g_output_stream_write_finish (stream, result, &error);

  if (nwrote < 0)
   {
     g_vfs_job_failed (G_VFS_JOB (job),
                       error->domain,
                       error->code,
                       error->message);

     g_error_free (error);
     return;
   }

  g_vfs_job_write_set_written_size (G_VFS_JOB_WRITE (job), nwrote);
  g_vfs_job_succeeded (job);
}

static gboolean
try_write (GVfsBackend *backend,
           GVfsJobWrite *job,
           GVfsBackendHandle handle,
           char *buffer,
           gsize buffer_size)
{
  GVfsBackendHttp *op_backend;
  GOutputStream   *stream;

  op_backend = G_VFS_BACKEND_HTTP (backend);
  stream = G_OUTPUT_STREAM (handle);

  g_output_stream_write_async (stream,
                               buffer,
                               buffer_size,
                               G_PRIORITY_DEFAULT,
                               G_VFS_JOB (job)->cancellable,
                               write_ready,
                               job);
  return TRUE;
}

/* *** close_write () *** */
static void
close_write_ready (GObject      *source_object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  GOutputStream *stream;
  GVfsJob       *job;
  GError        *error;
  gboolean       res;

  job = G_VFS_JOB (user_data);
  stream = G_OUTPUT_STREAM (source_object);
  res = g_output_stream_close_finish (stream,
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
try_close_write (GVfsBackend *backend,
                 GVfsJobCloseWrite *job,
                 GVfsBackendHandle handle)
{
  GVfsBackendHttp *op_backend;
  GOutputStream   *stream;

  op_backend = G_VFS_BACKEND_HTTP (backend);
  stream = G_OUTPUT_STREAM (handle);

  g_output_stream_close_async (stream,
                               G_PRIORITY_DEFAULT,
                               G_VFS_JOB (job)->cancellable,
                               close_write_ready,
                               job);

  return TRUE;
}

/* *** query_info () *** */

static void 
query_info_ready (GFileInfo *info,
                  gpointer   user_data,
                  GError    *error)
{
  GVfsJobQueryInfo *job;

  job = G_VFS_JOB_QUERY_INFO (user_data);

  if (info == NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      return;
    }
  
  g_file_info_copy_into (info, job->file_info);
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
  stat_location (backend, 
                 filename,
                 flags,
                 attribute_matcher, 
                 query_info_ready,
                 job);
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
  backend_class->try_seek_on_read  = try_seek_on_read;
  backend_class->try_close_read    = try_close_read;
  backend_class->try_create        = try_create;
  backend_class->try_replace       = try_replace;
  backend_class->try_write         = try_write;
  backend_class->try_close_write   = try_close_write;
  backend_class->try_query_info    = try_query_info;

}
