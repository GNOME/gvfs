/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2015, 2016 Cosimo Cecchi <cosimoc@gnome.org>
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
 */

#include <config.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/fsuid.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <polkit/polkit.h>

#include "gvfsbackendadmin.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobqueryinforead.h"
#include "gvfsjobqueryinfowrite.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobwrite.h"
#include "gvfsmonitor.h"

struct _GVfsBackendAdmin
{
  GVfsBackend parent_instance;

  GMutex polkit_mutex;
  PolkitAuthority *authority;
};

struct _GVfsBackendAdminClass
{
  GVfsBackendClass parent_class;
};

G_DEFINE_TYPE(GVfsBackendAdmin, g_vfs_backend_admin, G_VFS_TYPE_BACKEND)

static void
do_finalize (GObject *object)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (object);

  g_clear_object (&self->authority);
  g_mutex_clear (&self->polkit_mutex);

  G_OBJECT_CLASS (g_vfs_backend_admin_parent_class)->finalize (object);
}

static gboolean
check_permission (GVfsBackendAdmin *self,
                  GVfsJob *job)
{
  GVfsJobDBus *dbus_job = G_VFS_JOB_DBUS (job);
  GError *error = NULL;
  GDBusMethodInvocation *invocation;
  GDBusConnection *connection;
  GCredentials *credentials;
  pid_t pid;
  uid_t uid;
  PolkitSubject *subject;
  PolkitAuthorizationResult *result;
  gboolean is_authorized;

  invocation = dbus_job->invocation;
  connection = g_dbus_method_invocation_get_connection (invocation);
  credentials = g_dbus_connection_get_peer_credentials (connection);
  if (!credentials)
    {
      g_warning ("The admin backend doesn't work with the session bus "
                 "fallback. Your application is probably missing "
                 "--filesystem=xdg-run/gvfsd privileges.");
      g_vfs_job_failed_literal (job, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                _("Operation not supported"));
      return FALSE;
    }

  pid = g_credentials_get_unix_pid (credentials, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (job, error);
      g_error_free (error);
      return FALSE;
    }

  uid = g_credentials_get_unix_user (credentials, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (job, error);
      g_error_free (error);
      return FALSE;
    }

  /* Only one polkit dialog at a time */
  g_mutex_lock (&self->polkit_mutex);

  subject = polkit_unix_process_new_for_owner (pid, 0, uid);
  result = polkit_authority_check_authorization_sync (self->authority,
                                                      subject,
                                                      "org.gtk.vfs.file-operations",
                                                      NULL, POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION,
                                                      NULL, &error);
  g_object_unref (subject);

  g_mutex_unlock (&self->polkit_mutex);

  if (error != NULL)
    {
      g_vfs_job_failed_from_error (job, error);
      g_error_free (error);
      return FALSE;
    }

  is_authorized = polkit_authorization_result_get_is_authorized (result);

  g_object_unref (result);

  if (!is_authorized)
    g_vfs_job_failed_literal (job, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                              _("Permission denied"));

  return is_authorized;
}

static void
complete_job (GVfsJob *job,
              GError *error)
{
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (job, error);
      g_error_free (error);
      return;
    }

  g_vfs_job_succeeded (job);
}

static void
do_query_info (GVfsBackend *backend,
               GVfsJobQueryInfo *query_info_job,
               const char *filename,
               GFileQueryInfoFlags flags,
               GFileInfo *info,
               GFileAttributeMatcher *matcher)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (query_info_job);
  GError *error = NULL;
  GFile *file;
  GFileInfo *real_info;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);
  real_info = g_file_query_info (file, query_info_job->attributes,
                                 flags, job->cancellable, &error);
  g_object_unref (file);

  if (error != NULL)
    goto out;

  g_file_info_copy_into (real_info, info);
  g_object_unref (real_info);

 out:
  complete_job (job, error);
}

static void
do_query_info_on_read (GVfsBackend *backend,
                       GVfsJobQueryInfoRead *query_info_job,
                       GVfsBackendHandle handle,
                       GFileInfo *info,
                       GFileAttributeMatcher *matcher)
{
  GVfsJob *job = G_VFS_JOB (query_info_job);
  GFileInputStream *stream = handle;
  GError *error = NULL;
  GFileInfo *real_info;

  real_info = g_file_input_stream_query_info (stream, query_info_job->attributes,
                                              job->cancellable, &error);
  if (error != NULL)
    goto out;

  g_file_info_copy_into (real_info, info);
  g_object_unref (real_info);

 out:
  complete_job (job, error);
}

static void
do_query_info_on_write (GVfsBackend *backend,
                        GVfsJobQueryInfoWrite *query_info_job,
                        GVfsBackendHandle handle,
                        GFileInfo *info,
                        GFileAttributeMatcher *matcher)
{
  GVfsJob *job = G_VFS_JOB (query_info_job);
  GFileOutputStream *stream = handle;
  GError *error = NULL;
  GFileInfo *real_info;

  real_info = g_file_output_stream_query_info (stream, query_info_job->attributes,
                                               job->cancellable, &error);
  if (error != NULL)
    goto out;

  g_file_info_copy_into (real_info, info);
  g_object_unref (real_info);

 out:
  complete_job (job, error);
}

static void
do_close_write (GVfsBackend *backend,
                GVfsJobCloseWrite *close_write_job,
                GVfsBackendHandle handle)
{
  GVfsJob *job = G_VFS_JOB (close_write_job);
  GOutputStream *stream = handle;
  GError *error = NULL;

  g_output_stream_close (stream, job->cancellable, &error);
  g_object_unref (stream);

  complete_job (job, error);
}

static void
do_write (GVfsBackend *backend,
          GVfsJobWrite *write_job,
          GVfsBackendHandle handle,
          char *buffer,
          gsize buffer_size)
{
  GVfsJob *job = G_VFS_JOB (write_job);
  GOutputStream *stream = handle;
  GError *error = NULL;
  gssize bytes_written;

  bytes_written = g_output_stream_write (stream, buffer, buffer_size,
                                         job->cancellable, &error);
  if (bytes_written > -1)
    g_vfs_job_write_set_written_size (write_job, bytes_written);

  complete_job (job, error);
}

static void
set_open_for_write_attributes (GVfsJobOpenForWrite *open_write_job,
                               GFileOutputStream *stream)
{
  GSeekable *seekable = G_SEEKABLE (stream);

  g_vfs_job_open_for_write_set_handle (open_write_job, stream);
  g_vfs_job_open_for_write_set_can_seek
    (open_write_job, g_seekable_can_seek (seekable));
  g_vfs_job_open_for_write_set_can_truncate
    (open_write_job, g_seekable_can_truncate (seekable));
}

static void
do_append_to (GVfsBackend *backend,
              GVfsJobOpenForWrite *open_write_job,
              const char *filename,
              GFileCreateFlags flags)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (open_write_job);
  GError *error = NULL;
  GFile *file;
  GFileOutputStream *stream;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);
  stream = g_file_append_to (file, flags, job->cancellable, &error);
  g_object_unref (file);

  if (error != NULL)
    goto out;

  set_open_for_write_attributes (open_write_job, stream);
  g_vfs_job_open_for_write_set_initial_offset (open_write_job,
                                               g_seekable_tell (G_SEEKABLE (stream)));

 out:
  complete_job (job, error);
}

static void
do_create (GVfsBackend *backend,
           GVfsJobOpenForWrite *open_write_job,
           const char *filename,
           GFileCreateFlags flags)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (open_write_job);
  GError *error = NULL;
  GFile *file;
  GFileOutputStream *stream;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);
  stream = g_file_create (file, flags, job->cancellable, &error);
  g_object_unref (file);

  if (error != NULL)
    goto out;

  set_open_for_write_attributes (open_write_job, stream);

 out:
  complete_job (job, error);
}

static void
do_replace (GVfsBackend *backend,
            GVfsJobOpenForWrite *open_write_job,
            const char *filename,
            const char *etag,
            gboolean make_backup,
            GFileCreateFlags flags)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (open_write_job);
  GError *error = NULL;
  GFile *file;
  GFileOutputStream *stream;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);
  stream = g_file_replace (file, etag, make_backup, flags,
                           job->cancellable, &error);
  g_object_unref (file);

  if (error != NULL)
    goto out;

  set_open_for_write_attributes (open_write_job, stream);

 out:
  complete_job (job, error);
}

static void
do_close_read (GVfsBackend *backend,
               GVfsJobCloseRead *close_read_job,
               GVfsBackendHandle handle)
{
  GVfsJob *job = G_VFS_JOB (close_read_job);
  GInputStream *stream = handle;
  GError *error = NULL;

  g_input_stream_close (stream, job->cancellable, &error);
  g_object_unref (stream);

  complete_job (job, error);
}

static void
do_read (GVfsBackend *backend,
         GVfsJobRead *read_job,
         GVfsBackendHandle handle,
         char *buffer,
         gsize bytes_requested)
{
  GVfsJob *job = G_VFS_JOB (read_job);
  GInputStream *stream = handle;
  GError *error = NULL;
  gssize bytes;

  bytes = g_input_stream_read (stream, buffer, bytes_requested,
                               job->cancellable, &error);
  if (bytes > -1)
    g_vfs_job_read_set_size (read_job, bytes);

  complete_job (job, error);
}

static void
do_open_for_read (GVfsBackend        *backend,
                  GVfsJobOpenForRead *open_read_job,
                  const char         *filename)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (open_read_job);
  GError *error = NULL;
  GFile *file;
  GFileInputStream *stream;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);
  stream = g_file_read (file, job->cancellable, &error);
  g_object_unref (file);

  if (error != NULL)
    goto out;

  g_vfs_job_open_for_read_set_handle (open_read_job, stream);
  g_vfs_job_open_for_read_set_can_seek (open_read_job,
                                        g_seekable_can_seek (G_SEEKABLE (stream)));

 out:
  complete_job (job, error);
}

static void
do_truncate (GVfsBackend *backend,
             GVfsJobTruncate *truncate_job,
             GVfsBackendHandle handle,
             goffset size)
{
  GVfsJob *job = G_VFS_JOB (truncate_job);
  GSeekable *seekable = handle;
  GError *error = NULL;

  g_seekable_truncate (seekable, size, job->cancellable, &error);

  complete_job (job, error);
}

static void
do_seek_on_read (GVfsBackend *backend,
                 GVfsJobSeekRead *seek_read_job,
                 GVfsBackendHandle handle,
                 goffset offset,
                 GSeekType type)
{
  GVfsJob *job = G_VFS_JOB (seek_read_job);
  GSeekable *seekable = handle;
  GError *error = NULL;

  if (g_seekable_seek (seekable, offset, type, job->cancellable, &error))
    g_vfs_job_seek_read_set_offset (seek_read_job, g_seekable_tell (seekable));

  complete_job (job, error);
}

static void
do_seek_on_write (GVfsBackend *backend,
                  GVfsJobSeekWrite *seek_write_job,
                  GVfsBackendHandle handle,
                  goffset offset,
                  GSeekType type)
{
  GVfsJob *job = G_VFS_JOB (seek_write_job);
  GSeekable *seekable = handle;
  GError *error = NULL;

  if (g_seekable_seek (seekable, offset, type, job->cancellable, &error))
    g_vfs_job_seek_write_set_offset (seek_write_job,
                                     g_seekable_tell (seekable));

  complete_job (job, error);
}

static void
do_enumerate (GVfsBackend *backend,
              GVfsJobEnumerate *enumerate_job,
              const char *filename,
              GFileAttributeMatcher *attribute_matcher,
              GFileQueryInfoFlags flags)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (enumerate_job);
  GError *error = NULL;
  GFile *file;
  GFileEnumerator *enumerator;
  GFileInfo *info;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);
  enumerator = g_file_enumerate_children (file, enumerate_job->attributes,
                                          flags, job->cancellable, &error);
  g_object_unref (file);

  if (error != NULL)
    goto out;

  while (TRUE)
    {
      if (!g_file_enumerator_iterate (enumerator, &info, NULL,
                                      job->cancellable, &error))
        {
          g_object_unref (enumerator);
          goto out;
        }

      if (!info)
        break;

      g_vfs_job_enumerate_add_info (enumerate_job, g_object_ref (info));
    }

  g_file_enumerator_close (enumerator, job->cancellable, &error);
  g_object_unref (enumerator);

  if (error != NULL)
    goto out;

  g_vfs_job_enumerate_done (enumerate_job);

 out:
  complete_job (job, error);
}

static void
do_make_directory (GVfsBackend *backend,
                   GVfsJobMakeDirectory *mkdir_job,
                   const char *filename)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (mkdir_job);
  GError *error = NULL;
  GFile *file;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);

  g_file_make_directory (file, job->cancellable, &error);
  g_object_unref (file);

  complete_job (job, error);
}

static void
do_make_symlink (GVfsBackend *backend,
                 GVfsJobMakeSymlink *symlink_job,
                 const char *filename,
                 const char *symlink_value)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (symlink_job);
  GError *error = NULL;
  GFile *file;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);
  g_file_make_symbolic_link (file, symlink_value, job->cancellable, &error);
  g_object_unref (file);

  complete_job (job, error);
}

static void
do_query_fs_info (GVfsBackend *backend,
                  GVfsJobQueryFsInfo *query_info_job,
                  const char *filename,
                  GFileInfo *info,
                  GFileAttributeMatcher *attribute_matcher)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (query_info_job);
  GError *error = NULL;
  GFile *file;
  char *attributes;
  GFileInfo *real_info;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);
  attributes = g_file_attribute_matcher_to_string (attribute_matcher);
  real_info = g_file_query_filesystem_info (file, attributes,
                                            job->cancellable, &error);
  g_object_unref (file);
  g_free (attributes);

  if (real_info != NULL)
    {
      g_file_info_copy_into (real_info, info);
      g_object_unref (real_info);
    }

  complete_job (job, error);
}

static void
monitor_changed (GFileMonitor* monitor,
                 GFile* file,
                 GFile* other_file,
                 GFileMonitorEvent event_type,
                 GVfsMonitor *vfs_monitor)
{
  char *file_path, *other_file_path;

  file_path = g_file_get_path (file);
  if (other_file)
    other_file_path = g_file_get_path (other_file);
  else
    other_file_path = NULL;

  g_vfs_monitor_emit_event (vfs_monitor,
                            event_type,
                            file_path,
                            other_file_path);

  g_free (file_path);
  g_free (other_file_path);
}

static void
create_dir_file_monitor (GVfsBackend *backend,
                         GVfsJobCreateMonitor *monitor_job,
                         const char *filename,
                         GFileMonitorFlags flags,
                         gboolean is_dir_monitor)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (monitor_job);
  GError *error = NULL;
  GFile *file;
  GFileMonitor *monitor;
  GVfsMonitor *vfs_monitor;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);

  if (is_dir_monitor)
    monitor = g_file_monitor_directory (file, flags, job->cancellable, &error);
  else
    monitor = g_file_monitor_file (file, flags, job->cancellable, &error);
  g_object_unref (file);

  if (error != NULL)
    goto out;

  vfs_monitor = g_vfs_monitor_new (backend);
  g_signal_connect (monitor, "changed",
                    G_CALLBACK (monitor_changed), vfs_monitor);

  g_object_set_data_full (G_OBJECT (vfs_monitor),
                          "real-monitor", monitor,
                          (GDestroyNotify) g_object_unref);

  g_vfs_job_create_monitor_set_monitor (monitor_job, vfs_monitor);
  g_object_unref (vfs_monitor);

 out:
  complete_job (job, error);
}

static void
do_create_dir_monitor (GVfsBackend *backend,
                       GVfsJobCreateMonitor *job,
                       const char *filename,
                       GFileMonitorFlags flags)
{
  create_dir_file_monitor (backend, job, filename, flags, TRUE);
}


static void
do_create_file_monitor (GVfsBackend *backend,
                        GVfsJobCreateMonitor *job,
                        const char *filename,
                        GFileMonitorFlags flags)
{
  create_dir_file_monitor (backend, job, filename, flags, FALSE);
}

static void
do_set_display_name (GVfsBackend *backend,
                     GVfsJobSetDisplayName *display_name_job,
                     const char *filename,
                     const char *display_name)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (display_name_job);
  GError *error = NULL;
  GFile *file, *new_file;
  char *new_path;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);
  new_file = g_file_set_display_name (file, display_name, job->cancellable, &error);
  g_object_unref (file);

  if (error != NULL)
    goto out;

  new_path = g_file_get_path (new_file);
  g_vfs_job_set_display_name_set_new_path (display_name_job, new_path);
  g_free (new_path);
  g_object_unref (new_file);

 out:
  complete_job (job, error);
}

static void
do_set_attribute (GVfsBackend *backend,
                  GVfsJobSetAttribute *set_attribute_job,
                  const char *filename,
                  const char *attribute,
                  GFileAttributeType type,
                  gpointer value_p,
                  GFileQueryInfoFlags flags)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (set_attribute_job);
  GError *error = NULL;
  GFile *file;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);
  g_file_set_attribute (file, attribute, type, value_p, flags,
                        job->cancellable, &error);
  g_object_unref (file);

  complete_job (job, error);
}

static void
do_delete (GVfsBackend *backend,
           GVfsJobDelete *delete_job,
           const char *filename)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (delete_job);
  GError *error = NULL;
  GFile *file;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);
  g_file_delete (file, job->cancellable, &error);
  g_object_unref (file);

  complete_job (job, error);
}

static void
do_move (GVfsBackend *backend,
         GVfsJobMove *move_job,
         const char *source,
         const char *destination,
         GFileCopyFlags flags,
         GFileProgressCallback progress_callback,
         gpointer progress_callback_data)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (move_job);
  GError *error = NULL;
  GFile *src_file, *dst_file;

  if (!check_permission (self, job))
    return;

  src_file = g_file_new_for_path (source);
  dst_file = g_file_new_for_path (destination);
  g_file_move (src_file, dst_file, flags,
               job->cancellable,
               progress_callback, progress_callback_data,
               &error);

  g_object_unref (src_file);
  g_object_unref (dst_file);

  complete_job (job, error);
}

static void
do_copy (GVfsBackend *backend,
         GVfsJobCopy *copy_job,
         const char *source,
         const char *destination,
         GFileCopyFlags flags,
         GFileProgressCallback progress_callback,
         gpointer progress_callback_data)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (copy_job);
  GError *error = NULL;
  GFile *src_file, *dst_file;

  if (!check_permission (self, job))
    return;

  src_file = g_file_new_for_path (source);
  dst_file = g_file_new_for_path (destination);
  g_file_copy (src_file, dst_file, flags,
               job->cancellable,
               progress_callback, progress_callback_data,
               &error);

  g_object_unref (src_file);
  g_object_unref (dst_file);

  complete_job (job, error);
}

static void
do_pull (GVfsBackend *backend,
         GVfsJobPull *pull_job,
         const char *source,
         const char *local_path,
         GFileCopyFlags flags,
         gboolean remove_source,
         GFileProgressCallback progress_callback,
         gpointer progress_callback_data)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (pull_job);
  GError *error = NULL;
  GFile *src_file, *dst_file;

  /* Pull method is necessary when user/group needs to be restored, return
   * G_IO_ERROR_NOT_SUPPORTED in other cases to proceed with the fallback code.
   */
  if (!(flags & G_FILE_COPY_ALL_METADATA))
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                                G_IO_ERROR_NOT_SUPPORTED,
                                _("Operation not supported"));
      return;
    }

  if (!check_permission (self, job))
    return;

  src_file = g_file_new_for_path (source);
  dst_file = g_file_new_for_path (local_path);

  if (remove_source)
    g_file_move (src_file, dst_file, flags, job->cancellable,
                 progress_callback, progress_callback_data, &error);
  else
    g_file_copy (src_file, dst_file, flags, job->cancellable,
                 progress_callback, progress_callback_data, &error);

  g_object_unref (src_file);
  g_object_unref (dst_file);

  complete_job (job, error);
}

static void
do_push (GVfsBackend *backend,
         GVfsJobPush *push_job,
         const char *destination,
         const char *local_path,
         GFileCopyFlags flags,
         gboolean remove_source,
         GFileProgressCallback progress_callback,
         gpointer progress_callback_data)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (push_job);
  GError *error = NULL;
  GFile *src_file, *dst_file;

  if (!check_permission (self, job))
    return;

  src_file = g_file_new_for_path (local_path);
  dst_file = g_file_new_for_path (destination);

  if (remove_source)
    g_file_move (src_file, dst_file, flags, job->cancellable,
                 progress_callback, progress_callback_data, &error);
  else
    g_file_copy (src_file, dst_file, flags, job->cancellable,
                 progress_callback, progress_callback_data, &error);

  g_object_unref (src_file);
  g_object_unref (dst_file);

  complete_job (job, error);
}

static void
do_query_settable_attributes (GVfsBackend *backend,
                              GVfsJobQueryAttributes *query_job,
                              const char *filename)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (query_job);
  GError *error = NULL;
  GFile *file;
  GFileAttributeInfoList *attr_list;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);
  attr_list = g_file_query_settable_attributes (file, job->cancellable, &error);
  g_object_unref (file);

  if (attr_list != NULL)
    {
      g_vfs_job_query_attributes_set_list (query_job, attr_list);
      g_file_attribute_info_list_unref (attr_list);
    }

  complete_job (job, error);
}

static void
do_query_writable_namespaces (GVfsBackend *backend,
                              GVfsJobQueryAttributes *query_job,
                              const char *filename)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (query_job);
  GError *error = NULL;
  GFile *file;
  GFileAttributeInfoList *attr_list;

  if (!check_permission (self, job))
    return;

  file = g_file_new_for_path (filename);
  attr_list = g_file_query_writable_namespaces (file, job->cancellable, &error);
  g_object_unref (file);

  if (attr_list != NULL)
    {
      g_vfs_job_query_attributes_set_list (query_job, attr_list);
      g_file_attribute_info_list_unref (attr_list);
    }

  complete_job (job, error);
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *mount_job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  GVfsBackendAdmin *self = G_VFS_BACKEND_ADMIN (backend);
  GVfsJob *job = G_VFS_JOB (mount_job);
  GError *error = NULL;
  GMountSpec *real_spec;
  const gchar *client;

  client = g_mount_spec_get (mount_spec, "client");
  if (client == NULL)
    {
      g_vfs_job_failed_literal (job, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                _("Invalid mount spec"));
      return;
    }

  g_debug ("client=%s\n", client);

  real_spec = g_mount_spec_new ("admin");
  g_mount_spec_set (real_spec, "client", client);
  g_vfs_backend_set_mount_spec (backend, real_spec);
  g_mount_spec_unref (real_spec);

  self->authority = polkit_authority_get_sync (NULL, &error);

  complete_job (job, error);
}

static void
g_vfs_backend_admin_class_init (GVfsBackendAdminClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

  object_class->finalize = do_finalize;

  backend_class->mount = do_mount;
  backend_class->open_for_read = do_open_for_read;
  backend_class->query_info = do_query_info;
  backend_class->query_info_on_read = do_query_info_on_read;
  backend_class->query_info_on_write = do_query_info_on_write;
  backend_class->read = do_read;
  backend_class->create = do_create;
  backend_class->append_to = do_append_to;
  backend_class->replace = do_replace;
  backend_class->write = do_write;
  backend_class->close_read = do_close_read;
  backend_class->close_write = do_close_write;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->seek_on_write = do_seek_on_write;
  backend_class->enumerate = do_enumerate;
  backend_class->truncate = do_truncate;
  backend_class->make_directory = do_make_directory;
  backend_class->make_symlink = do_make_symlink;
  backend_class->query_fs_info = do_query_fs_info;
  backend_class->create_dir_monitor = do_create_dir_monitor;
  backend_class->create_file_monitor = do_create_file_monitor;
  backend_class->set_display_name = do_set_display_name;
  backend_class->set_attribute = do_set_attribute;
  backend_class->delete = do_delete;
  backend_class->move = do_move;
  backend_class->copy = do_copy;
  backend_class->pull = do_pull;
  backend_class->push = do_push;
  backend_class->query_settable_attributes = do_query_settable_attributes;
  backend_class->query_writable_namespaces = do_query_writable_namespaces;
}

static void
g_vfs_backend_admin_init (GVfsBackendAdmin *self)
{
  GIcon *icon;
  GVfsBackend *backend = G_VFS_BACKEND (self);
  const gchar *content_type = "inode/directory";

  g_mutex_init (&self->polkit_mutex);
  g_vfs_backend_set_user_visible (backend, FALSE);

  icon = g_content_type_get_icon (content_type);
  g_vfs_backend_set_icon (backend, icon);
  g_object_unref (icon);

  icon = g_content_type_get_symbolic_icon (content_type);
  g_vfs_backend_set_symbolic_icon (backend, icon);
  g_object_unref (icon);
}

#define REQUIRED_CAPS (CAP_TO_MASK(CAP_FOWNER) | \
                       CAP_TO_MASK(CAP_DAC_OVERRIDE) | \
                       CAP_TO_MASK(CAP_DAC_READ_SEARCH) | \
                       CAP_TO_MASK(CAP_CHOWN))

static void
acquire_caps (uid_t uid)
{
  struct __user_cap_header_struct hdr;
  struct __user_cap_data_struct data;

  /* Set euid to user to make dbus work */
  if (seteuid (uid) < 0)
    g_error ("unable to drop privs");

  /* Set fsuid to still behave like root when working with files */
  setfsuid (0);
  if (setfsuid (-1) != 0)
   g_error ("setfsuid failed");

  memset (&hdr, 0, sizeof(hdr));
  hdr.version = _LINUX_CAPABILITY_VERSION;

  /* Drop all non-require capabilities */
  data.effective = REQUIRED_CAPS;
  data.permitted = REQUIRED_CAPS;
  data.inheritable = 0;
  if (capset (&hdr, &data) < 0)
    g_error ("capset failed");
}

static char *session_address = NULL;
static char *runtime_dir = NULL;
static GOptionEntry entries[] = {
  { "address", 0, 0, G_OPTION_ARG_STRING, &session_address, "DBus session address", NULL },
  { "dir", 0, 0, G_OPTION_ARG_STRING, &runtime_dir, "Runtime dir", NULL },
  { NULL }
};

void
g_vfs_backend_admin_pre_setup (int *argc,
                               char **argv[])
{
  const char *pkexec_uid;
  uid_t uid;
  GError *error = NULL;
  GOptionContext *context;

  context = g_option_context_new (NULL);
  g_option_context_set_ignore_unknown_options (context, TRUE);
  g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
  g_option_context_parse (context, argc, argv, &error);
  g_option_context_free (context);
  if (error != NULL)
    {
      g_printerr ("Can't parse arguments: %s", error->message);
      g_error_free (error);
      exit (1);
    }

  pkexec_uid = g_getenv ("PKEXEC_UID");
  if (pkexec_uid == NULL)
    {
      g_printerr ("gvfsd-admin must be executed under pkexec\n");
      exit (1);
    }

  errno = 0;
  uid = strtol (pkexec_uid, NULL, 10);
  if (errno != 0)
    g_error ("Unable to convert PKEXEC_UID string to uid_t");

  acquire_caps (uid);
  g_setenv ("DBUS_SESSION_BUS_ADDRESS", session_address, TRUE);

  if (runtime_dir)
    g_setenv ("XDG_RUNTIME_DIR", runtime_dir, TRUE);
}
