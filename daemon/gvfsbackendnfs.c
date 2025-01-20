/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2014 Ross Lagerwall
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
 * Public License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <poll.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "gvfsbackendnfs.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobdelete.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobtruncate.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobqueryinforead.h"
#include "gvfsjobqueryinfowrite.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobsetattribute.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobmove.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsdaemonutils.h"
#include "gvfsutils.h"

#include <nfsc/libnfs.h>
#include <nfsc/libnfs-raw-nfs.h>
#include <nfsc/libnfs-raw-mount.h>

struct _GVfsBackendNfs
{
  GVfsBackend parent_instance;

  struct nfs_context *ctx;
  GSource *source;
  mode_t umask;               /* cached umask of process */
};

typedef struct
{
  GSource source;
  struct nfs_context *ctx;
  GVfsBackendNfs *backend;
  int fd;                     /* fd registered for IO */
  gpointer tag;               /* tag for fd attached to this source */
  int events;                 /* IO events we're interested in */
} NfsSource;

G_DEFINE_TYPE (GVfsBackendNfs, g_vfs_backend_nfs, G_VFS_TYPE_BACKEND)

static void
g_vfs_backend_nfs_init (GVfsBackendNfs *backend)
{
}

static void
g_vfs_backend_nfs_destroy_context (GVfsBackendNfs *backend)
{
  if (backend->ctx)
    {
      nfs_destroy_context (backend->ctx);
      backend->ctx = NULL;
    }

  if (backend->source)
    {
      g_source_destroy (backend->source);
      g_source_unref (backend->source);
      backend->source = NULL;
    }
}

static void
g_vfs_backend_nfs_finalize (GObject *object)
{
  GVfsBackendNfs *backend = G_VFS_BACKEND_NFS (object);

  g_vfs_backend_nfs_destroy_context (backend);

  if (G_OBJECT_CLASS (g_vfs_backend_nfs_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_nfs_parent_class)->finalize) (object);
}

static gboolean
nfs_source_prepare (GSource *source, gint *timeout)
{
  NfsSource *nfs_source = (NfsSource *) source;
  int events, fd;

  *timeout = -1;

  fd = nfs_get_fd (nfs_source->ctx);
  events = nfs_which_events (nfs_source->ctx);

  if (fd < 0)
    {
      g_vfs_backend_force_unmount (G_VFS_BACKEND (nfs_source->backend));
      g_vfs_backend_nfs_destroy_context (nfs_source->backend);
    }
  else if (fd != nfs_source->fd)
    {
      g_source_remove_unix_fd (source, nfs_source->tag);
      nfs_source->fd = fd;
      nfs_source->events = events;
      nfs_source->tag = g_source_add_unix_fd (source, nfs_source->fd, events);
    }
  else if (events != nfs_source->events)
    {
      nfs_source->events = events;
      g_source_modify_unix_fd (source, nfs_source->tag, events);
    }

  return FALSE;
}

static gboolean
nfs_source_dispatch (GSource *source, GSourceFunc callback, gpointer user_data)
{
  NfsSource *nfs_source = (NfsSource *) source;
  int err;

  err = nfs_service (nfs_source->ctx,
                     g_source_query_unix_fd (source, nfs_source->tag));
  if (err)
    {
      g_warning ("nfs_service error: %d, %s\n",
                 err, nfs_get_error (nfs_source->ctx));
      g_vfs_backend_force_unmount (G_VFS_BACKEND (nfs_source->backend));
      g_vfs_backend_nfs_destroy_context (nfs_source->backend);
    }

  return G_SOURCE_CONTINUE;
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);
  GMountSpec *nfs_mount_spec;
  GSource *source;
  NfsSource *nfs_source;
  struct exportnode *export_list, *ptr;
  const char *host, *debug;
  g_autofree gchar *libnfs_host = NULL;
  char *basename, *display_name, *export = NULL;
  int err, debug_val;
  size_t pathlen = strlen (mount_spec->mount_prefix);
  size_t exportlen = SIZE_MAX;
  static GSourceFuncs nfs_source_callbacks = {
    nfs_source_prepare,
    NULL,
    nfs_source_dispatch,
    NULL
  };

  host = g_mount_spec_get (mount_spec, "host");
  if (!host)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("No hostname specified"));
      return;
    }

  /* The libnfs library doesn't use brackets for IPv6 addresses. */
  if (gvfs_is_ipv6 (host))
    libnfs_host = g_strndup (host + 1, strlen (host) - 2);
  else
    libnfs_host = g_strdup (host);
  export_list = mount_getexports (libnfs_host);

  /* Find the shortest matching mount. E.g. if the given mount_prefix is
   * /some/long/path and there exist two mounts, /some and /some/long, match
   * against /some. */
  for (ptr = export_list; ptr; ptr = ptr->ex_next)
    {
      /* First check that the NFS mount point is a prefix of the mount_prefix. */
      if (g_str_has_prefix (mount_spec->mount_prefix, ptr->ex_dir))
        {
          size_t this_exportlen = strlen (ptr->ex_dir);

          /* Check if the mount_prefix is longer than the NFS mount point.
           * E.g. mount_prefix: /mnt/file, mount point: /mnt */
          if (pathlen > this_exportlen)
            {
              /* Check if the mount_prefix has a slash at the correct point.
               * E.g. if the mount point is /mnt, then it's a match if the
               * mount_prefix is /mnt/a but not a match if the mount_prefix is
               * /mnta.  Choose it if it is the shortest found so far. */
              char *s = mount_spec->mount_prefix + this_exportlen;
              if (*s == '/' && this_exportlen < exportlen)
                {
                  export = ptr->ex_dir;
                  exportlen = this_exportlen;
                }
            }
          /* The mount_prefix and NFS mount point are identical.  Choose it if
           * it is the shortest found so far. */
          else if (this_exportlen < exportlen)
            {
              export = ptr->ex_dir;
              exportlen = this_exportlen;
            }
        }
    }

  if (!export)
    {
      mount_free_export_list (export_list);
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                _("Mount point does not exist"));
      return;
    }

  export = strdup (export);
  mount_free_export_list (export_list);

  op_backend->ctx = nfs_init_context ();

  debug = g_getenv ("GVFS_NFS_DEBUG");
  if (debug)
    debug_val = atoi (debug);
  else
    debug_val = 0;

  nfs_set_debug (op_backend->ctx, debug_val);

  err = nfs_mount (op_backend->ctx, libnfs_host, export);
  if (err)
    {
      if (err == -EACCES)
        {
          g_vfs_job_failed_literal (G_VFS_JOB (job),
                                    G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                    _("Permission denied: Perhaps this host is disallowed or a privileged port is needed"));
        }
      else
        {
          g_vfs_job_failed_from_errno (G_VFS_JOB (job), -err);
        }
      g_free (export);
      return;
    }

  source = g_source_new (&nfs_source_callbacks, sizeof (NfsSource));
  nfs_source = (NfsSource *) source;
  nfs_source->ctx = op_backend->ctx;
  nfs_source->backend = op_backend;
  nfs_source->events = nfs_which_events (op_backend->ctx);
  nfs_source->fd = nfs_get_fd (op_backend->ctx);
  nfs_source->tag = g_source_add_unix_fd (source,
                                          nfs_source->fd,
                                          nfs_source->events);
  g_source_attach (source, NULL);
  op_backend->source = source;

  basename = g_path_get_basename (export);
  /* Translators: This is "<mount point> on <host>" and is used as name for an NFS mount */
  display_name = g_strdup_printf (_("%s on %s"), basename, host);
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (basename);
  g_free (display_name);

  g_vfs_backend_set_icon_name (G_VFS_BACKEND (backend), "folder-remote");
  g_vfs_backend_set_symbolic_icon_name (G_VFS_BACKEND (backend), "folder-remote-symbolic");

  nfs_mount_spec = g_mount_spec_new ("nfs");
  g_mount_spec_set (nfs_mount_spec, "host", host);
  g_mount_spec_set_mount_prefix (nfs_mount_spec, export);
  g_vfs_backend_set_mount_spec (backend, nfs_mount_spec);
  g_mount_spec_unref (nfs_mount_spec);
  g_free (export);

  /* cache the process's umask for later */
  op_backend->umask = umask (0);
  umask (op_backend->umask);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
null_cb (int err, struct nfs_context *ctx, void *data, void *private_data)
{
}

static void
generic_cb (int err, struct nfs_context *ctx, void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);

  if (err == 0)
    g_vfs_job_succeeded (job);
  else
    g_vfs_job_failed_from_errno (job, -err);
}

static char *
create_etag (uint64_t mtime, uint32_t nsec)
{
  return g_strdup_printf ("%lu:%lu",
                          (long unsigned int)mtime,
                          (long unsigned int)nsec);
}

static void
open_for_read_fstat_cb (int err,
                        struct nfs_context *ctx,
                        void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);

  if (err == 0)
    {
      GVfsJobOpenForRead *op_job = G_VFS_JOB_OPEN_FOR_READ (job);
      struct stat *st = data;

      if (S_ISDIR (st->st_mode))
        {
          struct nfsfh *fh = op_job->backend_handle;

          nfs_close_async (ctx, fh, null_cb, NULL);
          g_vfs_job_failed_literal (job,
                                    G_IO_ERROR,
                                    G_IO_ERROR_IS_DIRECTORY,
                                    _("Can’t open directory"));
          return;
        }
    }
  g_vfs_job_succeeded (job);
}

static void
open_for_read_cb (int err,
                  struct nfs_context *ctx,
                  void *data, void *private_data)
{
  if (err == 0)
    {
      GVfsJobOpenForRead *op_job = G_VFS_JOB_OPEN_FOR_READ (private_data);

      g_vfs_job_open_for_read_set_handle (op_job, data);
      g_vfs_job_open_for_read_set_can_seek (op_job, TRUE);

      nfs_fstat_async (ctx, data, open_for_read_fstat_cb, private_data);
    }
  else
    {
      g_vfs_job_failed_from_errno (G_VFS_JOB (private_data), -err);
    }
}

static gboolean
try_open_for_read (GVfsBackend *backend,
                   GVfsJobOpenForRead *job,
                   const char *filename)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);

  nfs_open_async (op_backend->ctx, filename, O_RDONLY, open_for_read_cb, job);
  return TRUE;
}

static void
read_cb (int err, struct nfs_context *ctx, void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);

  if (err >= 0)
    {
      GVfsJobRead *op_job = G_VFS_JOB_READ (job);

#ifndef LIBNFS_API_V2
      memcpy (op_job->buffer, data, err);
#endif
      g_vfs_job_read_set_size (op_job, err);
      g_vfs_job_succeeded (job);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static gboolean
try_read (GVfsBackend *backend,
          GVfsJobRead *job,
          GVfsBackendHandle _handle,
          char *buffer,
          gsize bytes_requested)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);
  struct nfsfh *fh = _handle;

#ifdef LIBNFS_API_V2
  nfs_read_async (op_backend->ctx, fh, buffer, bytes_requested, read_cb, job);
#else
  nfs_read_async (op_backend->ctx, fh, bytes_requested, read_cb, job);
#endif
  return TRUE;
}

static const char *
set_type_from_mode (GFileInfo *info, uint64_t mode)
{
  GFileType type = G_FILE_TYPE_UNKNOWN;
  const char *mimetype = NULL;

  if (S_ISREG (mode))
    type = G_FILE_TYPE_REGULAR;
  else if (S_ISDIR (mode))
    {
      type = G_FILE_TYPE_DIRECTORY;
      mimetype = "inode/directory";
    }
  else if (S_ISFIFO (mode))
    {
      type = G_FILE_TYPE_SPECIAL;
      mimetype = "inode/fifo";
    }
  else if (S_ISSOCK (mode))
    {
      type = G_FILE_TYPE_SPECIAL;
      mimetype = "inode/socket";
    }
  else if (S_ISCHR (mode))
    {
      type = G_FILE_TYPE_SPECIAL;
      mimetype = "inode/chardevice";
    }
  else if (S_ISBLK (mode))
    {
      type = G_FILE_TYPE_SPECIAL;
      mimetype = "inode/blockdevice";
    }
  else if (S_ISLNK (mode))
    {
      type = G_FILE_TYPE_SYMBOLIC_LINK;
      g_file_info_set_is_symlink (info, TRUE);
      mimetype = "inode/symlink";
    }
  g_file_info_set_file_type (info, type);

  return mimetype;
}

static void
set_name_info (GFileInfo *info,
               const char *mimetype,
               const char *basename,
               GFileAttributeMatcher *matcher)
{
  char *free_mimetype = NULL;
  gboolean uncertain_content_type = FALSE;

  g_file_info_set_name (info, basename);
  if (basename[0] == '.')
    g_file_info_set_is_hidden (info, TRUE);
  if (basename[strlen (basename) -1] == '~')
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_BACKUP, TRUE);

  if (g_file_attribute_matcher_matches (matcher,
                                        G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME) ||
      g_file_attribute_matcher_matches (matcher,
                                        G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME))
    {
      char *edit_name = gvfs_file_info_populate_names_as_local (info, basename);
      g_free (edit_name);
    }

  if (mimetype == NULL)
    {
      if (basename)
        {
          free_mimetype = g_content_type_guess (basename, NULL, 0, &uncertain_content_type);
          mimetype = free_mimetype;
        }
      else
        mimetype = "application/octet-stream";
    }

  if (!uncertain_content_type)
    g_file_info_set_content_type (info, mimetype);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, mimetype);

  if (g_file_attribute_matcher_matches (matcher,
                                        G_FILE_ATTRIBUTE_STANDARD_ICON) ||
      g_file_attribute_matcher_matches (matcher,
                                        G_FILE_ATTRIBUTE_STANDARD_SYMBOLIC_ICON))
    {
      GIcon *icon = NULL;
      GIcon *symbolic_icon = NULL;

      icon = g_content_type_get_icon (mimetype);
      symbolic_icon = g_content_type_get_symbolic_icon (mimetype);

      if (icon == NULL)
        icon = g_themed_icon_new ("text-x-generic");
      if (symbolic_icon == NULL)
        symbolic_icon = g_themed_icon_new ("text-x-generic-symbolic");

      g_file_info_set_icon (info, icon);
      g_file_info_set_symbolic_icon (info, symbolic_icon);
      g_object_unref (icon);
      g_object_unref (symbolic_icon);
    }

  if (free_mimetype)
    g_free (free_mimetype);
}

static void
set_info_from_stat (GFileInfo *info, struct nfs_stat_64 *st, GFileAttributeMatcher *matcher)
{
  g_file_info_set_size (info, st->nfs_size);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE, st->nfs_used);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE, st->nfs_mode);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID, st->nfs_uid);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID, st->nfs_gid);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_NLINK, st->nfs_nlink);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_DEVICE, st->nfs_dev);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_RDEV, st->nfs_rdev);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_INODE, st->nfs_ino);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_BLOCK_SIZE, st->nfs_blksize);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_BLOCKS, st->nfs_blocks);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS, st->nfs_atime);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC, st->nfs_atime_nsec / 1000);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, st->nfs_mtime);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, st->nfs_mtime_nsec / 1000);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CHANGED, st->nfs_ctime);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC, st->nfs_ctime_nsec / 1000);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
}

static void
query_info_on_read_cb (int err,
                       struct nfs_context *ctx,
                       void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);

  if (err == 0)
    {
      GVfsJobQueryInfoRead *op_job = G_VFS_JOB_QUERY_INFO_READ (job);
      struct nfs_stat_64 *st = data;

      set_info_from_stat (op_job->file_info, st, op_job->attribute_matcher);
      set_type_from_mode (op_job->file_info, st->nfs_mode);

      g_vfs_job_succeeded (job);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static gboolean
try_query_info_on_read (GVfsBackend *backend,
                        GVfsJobQueryInfoRead *job,
                        GVfsBackendHandle _handle,
                        GFileInfo *info,
                        GFileAttributeMatcher *attribute_matcher)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);
  struct nfsfh *nfsfh = _handle;

  nfs_fstat64_async (op_backend->ctx, nfsfh, query_info_on_read_cb, job);
  return TRUE;
}

static void
seek_on_read_cb (int err,
                 struct nfs_context *ctx,
                 void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);

  if (err >= 0)
    {
      GVfsJobSeekRead *op_job = G_VFS_JOB_SEEK_READ (job);
      uint64_t *pos = data;

      g_vfs_job_seek_read_set_offset (op_job, *pos);
      g_vfs_job_succeeded (job);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static gboolean
try_seek_on_read (GVfsBackend *backend,
                  GVfsJobSeekRead *job,
                  GVfsBackendHandle _handle,
                  goffset offset,
                  GSeekType type)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);
  struct nfsfh *fh = _handle;

  nfs_lseek_async (op_backend->ctx,
                   fh, offset, gvfs_seek_type_to_lseek (type),
                   seek_on_read_cb, job);
  return TRUE;
}

static gboolean
try_close_read (GVfsBackend *backend,
                GVfsJobCloseRead *job,
                GVfsBackendHandle _handle)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);
  struct nfsfh *fh = _handle;

  nfs_close_async (op_backend->ctx, fh, generic_cb, job);
  return TRUE;
}

static gboolean
try_make_directory (GVfsBackend *backend,
                    GVfsJobMakeDirectory *job,
                    const char *filename)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);

  nfs_mkdir_async (op_backend->ctx, filename, generic_cb, job);
  return TRUE;
}

static void
unlink_cb (int err, struct nfs_context *ctx, void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);
  GVfsJobDelete *op_job = G_VFS_JOB_DELETE (job);

  if (err == 0)
    g_vfs_job_succeeded (job);
  else if (err == -EPERM || err == -EISDIR)
    nfs_rmdir_async (ctx, op_job->filename, generic_cb, private_data);
  else
    g_vfs_job_failed_from_errno (job, -err);
}

static gboolean
try_delete (GVfsBackend *backend,
            GVfsJobDelete *job,
            const char *filename)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);

  nfs_unlink_async (op_backend->ctx, filename, unlink_cb, job);
  return TRUE;
}

static gboolean
try_make_symlink (GVfsBackend *backend,
                  GVfsJobMakeSymlink *job,
                  const char *filename,
                  const char *symlink_value)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);

  nfs_symlink_async (op_backend->ctx,
                     symlink_value, filename,
                     generic_cb, job);
  return TRUE;
}

typedef struct {
  struct nfsfh *fh;
  GVfsJob *job;
  char *filename;
  char *tempname;
  char *backup_filename;
  uint64_t uid;
  uint64_t gid;
  uint64_t nlink;
  uint64_t mode;
  gboolean is_symlink;
} WriteHandle;

static void
write_handle_free (WriteHandle *handle)
{
  if (handle->job)
    g_object_unref (handle->job);
  if (handle->filename)
    g_free (handle->filename);
  if (handle->tempname)
    g_free (handle->tempname);
  if (handle->backup_filename)
    g_free (handle->backup_filename);
  g_slice_free (WriteHandle, handle);
}

static void
open_for_write_create_cb (int err,
                          struct nfs_context *ctx,
                          void *data,
                          void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);
  if (err == 0)
    {
      GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
      WriteHandle *handle = g_slice_new0 (WriteHandle);

      handle->fh = data;
      g_vfs_job_open_for_write_set_handle (op_job, handle);
      g_vfs_job_open_for_write_set_can_seek (op_job, TRUE);
      g_vfs_job_open_for_write_set_can_truncate (op_job, TRUE);
      g_vfs_job_succeeded (job);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static void
open_for_write_create (GVfsBackend *backend,
                       GVfsJobOpenForWrite *job,
                       const char *filename,
                       GFileCreateFlags flags,
                       int open_flags)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);

#ifdef LIBNFS_API_V2
  nfs_open2_async (op_backend->ctx,
                   filename,
                   O_CREAT | open_flags,
                   (flags & G_FILE_CREATE_PRIVATE ? 0600 : 0666) & ~op_backend->umask,
                   open_for_write_create_cb,
                   job);
#else
  nfs_create_async (op_backend->ctx,
                    filename,
                    open_flags,
                    (flags & G_FILE_CREATE_PRIVATE ? 0600 : 0666) & ~op_backend->umask,
                    open_for_write_create_cb,
                    job);
#endif
}

static void
open_for_write_stat_cb (int err,
                        struct nfs_context *ctx,
                        void *data,
                        void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);
  GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  struct nfs_stat_64 *st = data;
  int open_flags = 0;

  if (err == 0)
    {
      if (S_ISDIR (st->nfs_mode))
        {
          g_vfs_job_failed_literal (job,
                                    G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                    _("Target file is a directory"));
          return;
        }
    }
  else if (err != -ENOENT)
    {
      g_vfs_job_failed_from_errno (job, -err);
      return;
    }

  if (op_job->mode == OPEN_FOR_WRITE_APPEND)
    {
      open_flags = O_APPEND;
      g_vfs_job_open_for_write_set_initial_offset (op_job, st->nfs_size);
    }

  open_for_write_create (op_job->backend,
                         op_job,
                         op_job->filename,
                         op_job->flags,
                         open_flags);
}

static void
open_for_write (GVfsBackend *backend,
                GVfsJobOpenForWrite *job,
                const char *filename)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);

  /* Check for existing directory because libnfs doesn't fail in this case. */
  nfs_stat64_async (op_backend->ctx, filename, open_for_write_stat_cb, job);
}

static gboolean
try_append_to (GVfsBackend *backend,
               GVfsJobOpenForWrite *job,
               const char *filename,
               GFileCreateFlags flags)
{
  open_for_write (backend, job, filename);

  return TRUE;
}

static gboolean
try_edit (GVfsBackend *backend,
          GVfsJobOpenForWrite *job,
          const char *filename,
          GFileCreateFlags flags)
{
  open_for_write (backend, job, filename);

  return TRUE;
}

/* The following types and functions implement an asynchronous copy which calls
 * a callback function with a boolean success or failure.  This is used in some
 * cases for backup files when replacing. */
#define COPY_BLKSIZE (64 * 1024)

typedef void (*CopyFileCallback) (gboolean success, void *private_data);

typedef struct
{
  struct nfsfh *srcfh;
  struct nfsfh *destfh;
  char *dest;
  int mode;
  CopyFileCallback cb;
  void *private_data;
#ifdef LIBNFS_API_V2
  char buffer[COPY_BLKSIZE];
#endif
} CopyHandle;

static void
copy_handle_complete (struct nfs_context *ctx,
                      CopyHandle *handle,
                      gboolean result)
{
  if (handle->srcfh)
    nfs_close_async (ctx, handle->srcfh, null_cb, NULL);
  if (handle->destfh)
    nfs_close_async (ctx, handle->destfh, null_cb, NULL);
  handle->cb (result, handle->private_data);
  g_slice_free (CopyHandle, handle);
}

static void
copy_read_cb (int err, struct nfs_context *ctx, void *data, void *private_data);

static void
copy_write_cb (int err,
               struct nfs_context *ctx,
               void *data,
               void *private_data)
{
  CopyHandle *handle = private_data;

  if (err > 0)
#ifdef LIBNFS_API_V2
    nfs_read_async (ctx, handle->srcfh, handle->buffer, COPY_BLKSIZE, copy_read_cb, handle);
#else
    nfs_read_async (ctx, handle->srcfh, COPY_BLKSIZE, copy_read_cb, handle);
#endif
  else
    copy_handle_complete (ctx, handle, FALSE);
}

static void
copy_read_cb (int err, struct nfs_context *ctx, void *data, void *private_data)
{
  CopyHandle *handle = private_data;

  if (err == 0)
    copy_handle_complete (ctx, handle, TRUE);
  else if (err > 0)
#ifdef LIBNFS_API_V2
    nfs_write_async (ctx, handle->destfh, handle->buffer, err, copy_write_cb, handle);
#else
    nfs_write_async (ctx, handle->destfh, err, data, copy_write_cb, handle);
#endif
  else
    copy_handle_complete (ctx, handle, FALSE);
}

static void
copy_open_dest_cb (int err,
                   struct nfs_context *ctx,
                   void *data, void *private_data)
{
  CopyHandle *handle = private_data;

  if (err == 0)
    {
      handle->destfh = data;

#ifdef LIBNFS_API_V2
      nfs_read_async (ctx, handle->srcfh, handle->buffer, COPY_BLKSIZE, copy_read_cb, handle);
#else
      nfs_read_async (ctx, handle->srcfh, COPY_BLKSIZE, copy_read_cb, handle);
#endif
    }
  else
    {
      copy_handle_complete (ctx, handle, FALSE);
    }
}

static void
copy_open_source_cb (int err,
                     struct nfs_context *ctx,
                     void *data, void *private_data)
{
  CopyHandle *handle = private_data;

  if (err == 0)
    {
      handle->srcfh = data;
#ifdef LIBNFS_API_V2
      nfs_open2_async (ctx,
                       handle->dest, O_CREAT | O_TRUNC, handle->mode & 0777,
                       copy_open_dest_cb, handle);
#else
      nfs_create_async (ctx,
                        handle->dest, O_TRUNC, handle->mode & 0777,
                        copy_open_dest_cb, handle);
#endif
      g_free (handle->dest);
    }
  else
    {
      g_free (handle->dest);
      copy_handle_complete (ctx, handle, FALSE);
    }
}

static void
copy_file (struct nfs_context *ctx,
           const char *src, const char *dest, int mode,
           CopyFileCallback cb, void *private_data)
{
  CopyHandle *handle;

  handle = g_slice_new0 (CopyHandle);
  handle->dest = g_strdup (dest);
  handle->mode = mode;
  handle->cb = cb;
  handle->private_data = private_data;

  nfs_open_async (ctx, src, O_RDONLY, copy_open_source_cb, handle);
}

/* The replace code that follows is relatively straightforward but difficult to
 * read due to its asynchronous nature.  It closely follows the replace
 * implementation for local files in GIO, but in an async fashion.
 * Firstly, the file is opened with O_EXCL.  If this fails, the file exists.
 * Some checks on the existing file are made.  If possible, a temporary file is
 * opened and renamed over the existing file on close.  If a backup is needed,
 * the existing file is simply renamed to the backup.
 * If it is not possible to create a temporary file (e.g. due to permissions),
 * then the file is truncated and writing takes place directly into the file.
 * However, if a backup needs to be made, the existing file is first copied
 * into the backup file. */
static void
replace_trunc_cb (int err,
                  struct nfs_context *ctx,
                  void *data, void *private_data)
{
  WriteHandle *handle = private_data;
  GVfsJob *job = handle->job;

  if (err == 0)
    {
      GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);

      handle->fh = data;
      g_vfs_job_open_for_write_set_handle (op_job, handle);
      g_vfs_job_open_for_write_set_can_seek (op_job, TRUE);
      g_vfs_job_open_for_write_set_can_truncate (op_job, TRUE);
      g_vfs_job_succeeded (job);

      g_object_unref (handle->job);
      handle->job = NULL;
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
      write_handle_free (handle);
    }
}

static void
replace_backup_chown_cb (int err,
                         struct nfs_context *ctx,
                         void *data, void *private_data)
{
  WriteHandle *handle = private_data;
  GVfsJob *job = handle->job;

  g_free (handle->backup_filename);
  handle->backup_filename = NULL;

  if (err == 0 || err == -EPERM)
    {
      GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
      GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (op_job->backend);

#ifdef LIBNFS_API_V2
      nfs_open2_async (op_backend->ctx,
                       op_job->filename,
                       O_CREAT | O_TRUNC,
                       (op_job->flags & G_FILE_CREATE_PRIVATE ? 0600 : 0666) & ~op_backend->umask,
                       replace_trunc_cb, handle);
#else
      nfs_create_async (op_backend->ctx,
                        op_job->filename,
                        O_TRUNC,
                        (op_job->flags & G_FILE_CREATE_PRIVATE ? 0600 : 0666) & ~op_backend->umask,
                        replace_trunc_cb, handle);
#endif
    }
  else
    {
      g_vfs_job_failed_literal (job,
                                G_IO_ERROR, G_IO_ERROR_CANT_CREATE_BACKUP,
                                _("Backup file creation failed"));
      write_handle_free (handle);
    }
}

static void
replace_backup_cb (gboolean success, void *private_data)
{
  WriteHandle *handle = private_data;
  GVfsJob *job = handle->job;

  if (success)
    {
      GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
      GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (op_job->backend);
      nfs_chown_async (op_backend->ctx,
                       handle->backup_filename, handle->uid, handle->gid,
                       replace_backup_chown_cb, handle);
    }
  else
    {
      g_vfs_job_failed_literal (job,
                                G_IO_ERROR, G_IO_ERROR_CANT_CREATE_BACKUP,
                                _("Backup file creation failed"));
      write_handle_free (handle);
    }
}

static void
replace_rm_backup_cb (int err,
                      struct nfs_context *ctx,
                      void *data, void *private_data)
{
  WriteHandle *handle = private_data;
  GVfsJob *job = handle->job;

  if (err == 0 || err == -ENOENT || err == -EACCES)
    {
      GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);

      copy_file (ctx,
                 op_job->filename, handle->backup_filename,
                 handle->mode & 0777,
                 replace_backup_cb, handle);
    }
  else
    {
      g_vfs_job_failed_literal (job,
                                G_IO_ERROR, G_IO_ERROR_CANT_CREATE_BACKUP,
                                _("Backup file creation failed"));
      write_handle_free (handle);
    }
}

static void
replace_truncate (struct nfs_context *ctx, WriteHandle *handle)
{
  GVfsJob *job = handle->job;
  GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (op_job->backend);

  g_free (handle->filename);
  g_free (handle->tempname);
  handle->filename = handle->tempname = NULL;

  if (op_job->make_backup)
    {
      handle->backup_filename = g_strconcat (op_job->filename, "~", NULL);
      nfs_unlink_async (ctx,
                        handle->backup_filename,
                        replace_rm_backup_cb, handle);
    }
  else
    {
#ifdef LIBNFS_API_V2
      nfs_open2_async (ctx,
                       op_job->filename,
                       O_CREAT | O_TRUNC,
                       (op_job->flags & G_FILE_CREATE_PRIVATE ? 0600 : 0666) & ~op_backend->umask,
                       replace_trunc_cb, handle);
#else
      nfs_create_async (ctx,
                        op_job->filename,
                        O_TRUNC,
                        (op_job->flags & G_FILE_CREATE_PRIVATE ? 0600 : 0666) & ~op_backend->umask,
                        replace_trunc_cb, handle);
#endif
    }
}

static void
replace_temp_chmod_cb (int err,
                       struct nfs_context *ctx,
                       void *data, void *private_data)
{
  WriteHandle *handle = private_data;
  GVfsJob *job = handle->job;

  if (err == 0)
    {
      GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);

      g_vfs_job_open_for_write_set_handle (op_job, handle);
      g_vfs_job_open_for_write_set_can_seek (op_job, TRUE);
      g_vfs_job_open_for_write_set_can_truncate (op_job, TRUE);
      g_vfs_job_succeeded (job);

      g_object_unref (handle->job);
      handle->job = NULL;
    }
  else
    {
      nfs_close_async (ctx, handle->fh, null_cb, NULL);
      g_vfs_job_failed_literal (job,
                                G_IO_ERROR, G_IO_ERROR_FAILED,
                                _("Unable to create temporary file"));
      write_handle_free (handle);
    }
}

static void
replace_temp_chown_cb (int err,
                       struct nfs_context *ctx,
                       void *data, void *private_data)
{
  WriteHandle *handle = private_data;

  if (err == 0)
    {
      nfs_fchmod_async (ctx,
                        handle->fh, handle->mode & 0777,
                        replace_temp_chmod_cb, handle);
    }
  else
    {
      nfs_close_async (ctx, handle->fh, null_cb, NULL);
      g_vfs_job_failed_literal (handle->job,
                                G_IO_ERROR, G_IO_ERROR_FAILED,
                                _("Unable to create temporary file"));
      write_handle_free (handle);
    }
}

static void
replace_temp_cb (int err,
                 struct nfs_context *ctx,
                 void *data, void *private_data)
{
  WriteHandle *handle = private_data;
  GVfsJob *job = handle->job;
  GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);

  if (err == 0)
    {
      handle->fh = data;

      if (op_job->make_backup)
        handle->backup_filename = g_strconcat (op_job->filename, "~", NULL);

      if (op_job->flags & G_FILE_CREATE_REPLACE_DESTINATION)
        {
          g_vfs_job_open_for_write_set_handle (op_job, handle);
          g_vfs_job_open_for_write_set_can_seek (op_job, TRUE);
          g_vfs_job_open_for_write_set_can_truncate (op_job, TRUE);
          g_vfs_job_succeeded (job);

          g_object_unref (handle->job);
          handle->job = NULL;
        }
      else
        {
          nfs_fchown_async (ctx,
                            handle->fh,
                            handle->uid, handle->gid,
                            replace_temp_chown_cb, handle);
        }
    }
  else if ((err == -EACCES || err == -EEXIST) &&
           !(op_job->flags & G_FILE_CREATE_REPLACE_DESTINATION))
    {
      replace_truncate (ctx, handle);
    }
  else if (err == -EEXIST)
    {
      g_vfs_job_failed_literal (job,
                                G_IO_ERROR, G_IO_ERROR_FAILED,
                                _("Unable to create temporary file"));
      write_handle_free (handle);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
      write_handle_free (handle);
    }
}

static void
replace_stat_cb (int err,
                 struct nfs_context *ctx,
                 void *data, void *private_data)
{
  WriteHandle *handle = private_data;
  GVfsJob *job = handle->job;

  if (err == 0)
    {
      GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
      GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (op_job->backend);
      struct nfs_stat_64 *st = data;

      /* Fail if we're not replacing the destination and the destination is a
       * directory, or if we are replacing the destination and the destination
       * is a directory */
      if ((!(op_job->flags & G_FILE_CREATE_REPLACE_DESTINATION) ||
           !handle->is_symlink) &&
          S_ISDIR (st->nfs_mode))
        {
          g_vfs_job_failed_literal (job,
                                    G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                    _("Target file is a directory"));
          write_handle_free (handle);
        }
      /* Fail if we're not replacing the destination and the destination is not
       * a regular file. */
      else if (!(op_job->flags & G_FILE_CREATE_REPLACE_DESTINATION) &&
               !S_ISREG (st->nfs_mode))
        {
          g_vfs_job_failed_literal (job,
                                    G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                                    _("Target file is not a regular file"));
          write_handle_free (handle);
        }
      else
        {
          if (op_job->etag)
            {
              char *etag;

              etag = create_etag (st->nfs_mtime, st->nfs_mtime_nsec);
              if (strcmp (etag, op_job->etag))
                {
                  g_free (etag);
                  g_vfs_job_failed_literal (job,
                                            G_IO_ERROR, G_IO_ERROR_WRONG_ETAG,
                                            _("The file was externally modified"));
                  write_handle_free (handle);
                  return;
                }
              g_free (etag);
            }

          handle->mode = st->nfs_mode;
          handle->uid = st->nfs_uid;
          handle->gid = st->nfs_gid;
          handle->nlink = st->nfs_nlink;

          /* Write to a temporary file then do an atomic rename if either:
           * - G_FILE_CREATE_REPLACE_DESTINATION is specified
           * - the destination is not a symlink, and it does not have multiple
           *   hard links.
           */
          if ((op_job->flags & G_FILE_CREATE_REPLACE_DESTINATION) ||
              (!handle->is_symlink && handle->nlink <= 1))
            {
              char *dirname;
              char basename[] = ".giosaveXXXXXX";

              handle->filename = g_strdup (op_job->filename);

              dirname = g_path_get_dirname (op_job->filename);
              gvfs_randomize_string (basename + 8, 6);
              handle->tempname = g_build_filename (dirname, basename, NULL);
              g_free (dirname);

#ifdef LIBNFS_API_V2
              nfs_open2_async (ctx,
                               handle->tempname,
                               O_CREAT | O_EXCL,
                               (op_job->flags & G_FILE_CREATE_PRIVATE ? 0600 : 0666) & ~op_backend->umask,
                               replace_temp_cb, handle);
#else
              nfs_create_async (ctx,
                                handle->tempname,
                                O_EXCL,
                                (op_job->flags & G_FILE_CREATE_PRIVATE ? 0600 : 0666) & ~op_backend->umask,
                                replace_temp_cb, handle);
#endif
            }
          else
            {
              replace_truncate (ctx, handle);
            }
        }
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
      write_handle_free (handle);
    }
}

static void
replace_lstat_cb (int err,
                  struct nfs_context *ctx,
                  void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);

  if (err == 0)
    {
      WriteHandle *handle;
      GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);
      struct nfs_stat_64 *st = data;

      handle = g_slice_new0 (WriteHandle);
      handle->is_symlink = S_ISLNK (st->nfs_mode);
      handle->job = g_object_ref (job);

      /* If the filename is a link, call stat to get the real info.
       * Otherwise, lstat is the same as stat, so just chain straight to the
       * stat callback. */
      if (handle->is_symlink)
        nfs_stat64_async (ctx, op_job->filename, replace_stat_cb, handle);
      else
        replace_stat_cb (err, ctx, data, handle);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static void
replace_create_cb (int err,
                   struct nfs_context *ctx,
                   void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);
  GVfsJobOpenForWrite *op_job = G_VFS_JOB_OPEN_FOR_WRITE (job);

  if (err == 0)
    {
      WriteHandle *handle = g_slice_new0 (WriteHandle);

      handle->fh = data;
      g_vfs_job_open_for_write_set_handle (op_job, handle);
      g_vfs_job_open_for_write_set_can_seek (op_job, TRUE);
      g_vfs_job_open_for_write_set_can_truncate (op_job, TRUE);
      g_vfs_job_succeeded (job);
    }
  else if (err == -EEXIST)
    {
      nfs_lstat64_async (ctx, op_job->filename, replace_lstat_cb, job);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static gboolean
try_replace (GVfsBackend *backend,
             GVfsJobOpenForWrite *job,
             const char *filename,
             const char *etag,
             gboolean make_backup,
             GFileCreateFlags flags)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);

#ifdef LIBNFS_API_V2
  nfs_open2_async (op_backend->ctx,
                   filename,
                   O_CREAT | O_EXCL,
                   (flags & G_FILE_CREATE_PRIVATE ? 0600 : 0666) & ~op_backend->umask,
                   replace_create_cb, job);
#else
  nfs_create_async (op_backend->ctx,
                    filename,
                    O_EXCL,
                    (flags & G_FILE_CREATE_PRIVATE ? 0600 : 0666) & ~op_backend->umask,
                    replace_create_cb, job);
#endif
  return TRUE;
}

static gboolean
try_create (GVfsBackend *backend,
            GVfsJobOpenForWrite *job,
            const char *filename,
            GFileCreateFlags flags)
{
  open_for_write_create (backend, job, filename, flags, O_EXCL);

  return TRUE;
}

static void
write_cb (int err, struct nfs_context *ctx, void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);

  if (err >= 0)
    {
      g_vfs_job_write_set_written_size (G_VFS_JOB_WRITE (job), err);
      g_vfs_job_succeeded (job);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static gboolean
try_write (GVfsBackend *backend,
           GVfsJobWrite *job,
           GVfsBackendHandle _handle,
           char *buffer,
           gsize buffer_size)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);
  WriteHandle *handle = _handle;
  struct nfsfh *fh = handle->fh;

#ifdef LIBNFS_API_V2
  nfs_write_async (op_backend->ctx, fh, buffer, buffer_size, write_cb, job);
#else
  nfs_write_async (op_backend->ctx, fh, buffer_size, buffer, write_cb, job);
#endif
  return TRUE;
}

static void
close_backup_cb (int err,
                 struct nfs_context *ctx,
                 void *data, void *private_data)
{
  WriteHandle *handle = private_data;
  GVfsJob *job = handle->job;

  if (err == 0)
    {
      nfs_rename_async (ctx,
                        handle->tempname, handle->filename,
                        generic_cb, job);
    }
  else
    {
      g_vfs_job_failed_literal (job,
                                G_IO_ERROR, G_IO_ERROR_CANT_CREATE_BACKUP,
                                _("Backup file creation failed"));
    }
  write_handle_free (handle);
}

static void
close_write_cb (int err,
                struct nfs_context *ctx,
                void *data, void *private_data)
{
  WriteHandle *handle = private_data;
  GVfsJob *job = handle->job;

  if (err == 0)
    {
      if (handle->backup_filename)
        {
          nfs_rename_async (ctx,
                            handle->filename, handle->backup_filename,
                            close_backup_cb, handle);
        }
      else
        {
          nfs_rename_async (ctx,
                            handle->tempname, handle->filename,
                            generic_cb, job);
          write_handle_free (handle);
        }
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
      write_handle_free (handle);
    }
}

static void
close_stat_cb (int err,
               struct nfs_context *ctx,
               void *data, void *private_data)
{
  WriteHandle *handle = private_data;
  GVfsJob *job = handle->job;

  if (err == 0)
    {
      GVfsJobCloseWrite *op_job = G_VFS_JOB_CLOSE_WRITE (job);
      char *etag;
      struct nfs_stat_64 *st = data;

      etag = create_etag (st->nfs_mtime, st->nfs_mtime_nsec);
      g_vfs_job_close_write_set_etag (op_job, etag);
      g_free (etag);
    }

  if (handle->tempname)
    {
      nfs_close_async (ctx, handle->fh, close_write_cb, handle);
    }
  else
    {
      nfs_close_async (ctx, handle->fh, generic_cb, job);
      write_handle_free (handle);
    }
}

static gboolean
try_close_write (GVfsBackend *backend,
                 GVfsJobCloseWrite *job,
                 GVfsBackendHandle _handle)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);
  WriteHandle *handle = _handle;

  handle->job = G_VFS_JOB (g_object_ref (job));
  nfs_fstat64_async (op_backend->ctx, handle->fh, close_stat_cb, handle);

  return TRUE;
}

static void
query_info_on_write_cb (int err,
                        struct nfs_context *ctx,
                        void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);

  if (err == 0)
    {
      GVfsJobQueryInfoWrite *op_job = G_VFS_JOB_QUERY_INFO_WRITE (job);
      struct nfs_stat_64 *st = data;

      set_info_from_stat (op_job->file_info, data, op_job->attribute_matcher);
      set_type_from_mode (op_job->file_info, st->nfs_mode);

      g_vfs_job_succeeded (job);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static gboolean
try_query_info_on_write (GVfsBackend *backend,
                        GVfsJobQueryInfoWrite *job,
                        GVfsBackendHandle _handle,
                        GFileInfo *info,
                        GFileAttributeMatcher *attribute_matcher)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);
  WriteHandle *handle = _handle;
  struct nfsfh *fh = handle->fh;

  nfs_fstat64_async (op_backend->ctx, fh, query_info_on_write_cb, job);
  return TRUE;
}

static void
seek_on_write_cb (int err,
                  struct nfs_context *ctx,
                  void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);

  if (err >= 0)
    {
      GVfsJobSeekWrite *op_job = G_VFS_JOB_SEEK_WRITE (job);
      uint64_t *pos = data;

      g_vfs_job_seek_write_set_offset (op_job, *pos);
      g_vfs_job_succeeded (job);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static gboolean
try_seek_on_write (GVfsBackend *backend,
                   GVfsJobSeekWrite *job,
                   GVfsBackendHandle _handle,
                   goffset offset,
                   GSeekType type)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);
  WriteHandle *handle = _handle;
  struct nfsfh *fh = handle->fh;

  nfs_lseek_async (op_backend->ctx,
                   fh, offset, gvfs_seek_type_to_lseek (type),
                   seek_on_write_cb, job);
  return TRUE;
}

static gboolean
try_truncate (GVfsBackend *backend,
              GVfsJobTruncate *job,
              GVfsBackendHandle _handle,
              goffset size)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);
  WriteHandle *handle = _handle;
  struct nfsfh *fh = handle->fh;

  nfs_ftruncate_async (op_backend->ctx, fh, size, generic_cb, job);
  return TRUE;
}

static void
query_fs_cb (int err, struct nfs_context *ctx, void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);

  if (err == 0)
    {
      GVfsJobQueryFsInfo *op_job = G_VFS_JOB_QUERY_FS_INFO (job);
      struct statvfs *stat = data;

      /* If free and available are both 0, treat it like the size information
       * is missing.
       */
      if (stat->f_bfree || stat->f_bavail)
        {
          g_file_info_set_attribute_uint64 (op_job->file_info,
                                            G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
                                            stat->f_frsize * stat->f_bavail);
          g_file_info_set_attribute_uint64 (op_job->file_info,
                                            G_FILE_ATTRIBUTE_FILESYSTEM_SIZE,
                                            stat->f_frsize * stat->f_blocks);
          g_file_info_set_attribute_uint64 (op_job->file_info,
                                            G_FILE_ATTRIBUTE_FILESYSTEM_USED,
                                            stat->f_frsize * (stat->f_blocks - stat->f_bfree));
        }
      g_file_info_set_attribute_boolean (op_job->file_info,
                                         G_FILE_ATTRIBUTE_FILESYSTEM_READONLY,
                                         stat->f_flag & ST_RDONLY);

      g_vfs_job_succeeded (job);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static gboolean
try_query_fs_info (GVfsBackend *backend,
                   GVfsJobQueryFsInfo *job,
                   const char *filename,
                   GFileInfo *info,
                   GFileAttributeMatcher *matcher)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);

  g_file_info_set_attribute_string (info,
                                    G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "nfs");
  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, TRUE);
  g_file_info_set_attribute_uint32 (info,
                                    G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW,
                                    G_FILESYSTEM_PREVIEW_TYPE_IF_ALWAYS);

  if (g_file_attribute_matcher_matches (matcher,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) ||
      g_file_attribute_matcher_matches (matcher,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_FREE) ||
      g_file_attribute_matcher_matches (matcher,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_USED) ||
      g_file_attribute_matcher_matches (matcher,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_READONLY))
    {
      nfs_statvfs_async (op_backend->ctx, filename, query_fs_cb, job);
    }
  else
    {
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }

  return TRUE;
}

typedef struct
{
  GSList *readlink_list;
  GSList *symlink_list;
  GSList *access_list;
  gboolean requires_access;
  int access_parent;
  GVfsJobEnumerate *op_job;
} EnumerateHandle;

static void enumerate_continue (EnumerateHandle *handle,
                                struct nfs_context *ctx);

static void
enumerate_access_cb (int err,
                     struct nfs_context *ctx,
                     void *data, void *private_data)
{
  EnumerateHandle *handle = private_data;
  GFileInfo *info = handle->access_list->data;

  if (err >= 0)
    {
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, err & R_OK);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, err & W_OK);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, err & X_OK);
    }

  g_vfs_job_enumerate_add_info (handle->op_job, info);
  g_object_unref (info);

  handle->access_list = g_slist_delete_link (handle->access_list,
                                             handle->access_list);

  enumerate_continue (handle, ctx);
}

static void
enumerate_stat_cb (int err,
                   struct nfs_context *ctx,
                   void *data, void *private_data)
{
  EnumerateHandle *handle = private_data;
  GFileInfo *info = handle->symlink_list->data;

  if (err == 0)
    {
      struct nfs_stat_64 *st = data;
      const char *mimetype;
      GFileInfo *new_info;

      new_info = g_file_info_new ();
      set_info_from_stat (new_info, st, handle->op_job->attribute_matcher);
      mimetype = set_type_from_mode (new_info, st->nfs_mode);
      set_name_info (new_info,
                     mimetype,
                     g_file_info_get_name (info),
                     handle->op_job->attribute_matcher);
      g_file_info_set_is_symlink (new_info, TRUE);

      if (g_file_attribute_matcher_matches (handle->op_job->attribute_matcher,
                                            G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET))
        g_file_info_set_symlink_target (new_info,
                                        g_file_info_get_symlink_target (info));

      if ((g_file_attribute_matcher_matches (handle->op_job->attribute_matcher,
                                             G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE) ||
           g_file_attribute_matcher_matches (handle->op_job->attribute_matcher,
                                             G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME)) &&
          handle->access_parent >= 0)
        {
          g_file_info_set_attribute_boolean (new_info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, handle->access_parent & W_OK);
          g_file_info_set_attribute_boolean (new_info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, handle->access_parent & W_OK);
        }

      g_object_unref (info);
      info = new_info;
    }

  if (handle->requires_access)
    handle->access_list = g_slist_prepend (handle->access_list, info);
  else
    {
      g_vfs_job_enumerate_add_info (handle->op_job, info);
      g_object_unref (info);
    }

  handle->symlink_list = g_slist_delete_link (handle->symlink_list,
                                              handle->symlink_list);

  enumerate_continue (handle, ctx);
}

static void
enumerate_readlink_cb (int err,
                       struct nfs_context *ctx,
                       void *data, void *private_data)
{
  EnumerateHandle *handle = private_data;
  GFileInfo *info = handle->readlink_list->data;

  if (err == 0)
    g_file_info_set_symlink_target (info, data);

  if (!(handle->op_job->flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS))
    handle->symlink_list = g_slist_prepend (handle->symlink_list, info);
  else if (handle->requires_access)
    handle->access_list = g_slist_prepend (handle->access_list, info);
  else
    {
      g_vfs_job_enumerate_add_info (handle->op_job, info);
      g_object_unref (info);
    }

  handle->readlink_list = g_slist_delete_link (handle->readlink_list,
                                              handle->readlink_list);

  enumerate_continue (handle, ctx);
}

static void
enumerate_continue (EnumerateHandle *handle, struct nfs_context *ctx)
{
  if (handle->readlink_list || handle->symlink_list || handle->access_list)
    {
      char *path;

      if (handle->readlink_list)
        {
          const char *filename = g_file_info_get_name (handle->readlink_list->data);
          path = g_build_filename (handle->op_job->filename, filename, NULL);
          nfs_readlink_async (ctx, path, enumerate_readlink_cb, handle);
        }
      else if (handle->symlink_list)
        {
          const char *filename = g_file_info_get_name (handle->symlink_list->data);
          path = g_build_filename (handle->op_job->filename, filename, NULL);
          nfs_stat64_async (ctx, path, enumerate_stat_cb, handle);
        }
      else if (handle->access_list)
        {
          const char *filename = g_file_info_get_name (handle->access_list->data);
          path = g_build_filename (handle->op_job->filename, filename, NULL);
          nfs_access2_async (ctx, path, enumerate_access_cb, handle);
        }

      g_free (path);
    }
  else
    {
      GVfsJobEnumerate *op_job = handle->op_job;
      g_slice_free (EnumerateHandle, handle);
      g_vfs_job_enumerate_done (op_job);
    }
}

static void
enumerate_cb (int err, struct nfs_context *ctx, void *data, void *private_data)
{
  EnumerateHandle *handle = private_data;
  GVfsJob *job = G_VFS_JOB (handle->op_job);

  if (err == 0)
    {
      GVfsJobEnumerate *op_job = handle->op_job;
      struct nfsdir *dir = data;
      struct nfsdirent *d;

      g_vfs_job_succeeded (job);

      handle->requires_access =
          g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                            G_FILE_ATTRIBUTE_ACCESS_CAN_READ) ||
          g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                            G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE) ||
          g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                            G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE);

      while ((d = nfs_readdir (ctx, dir)))
        {
          GFileInfo *info;
          GFileType type = G_FILE_TYPE_UNKNOWN;
          char *etag, *mimetype = NULL;

          if (!strcmp (d->name, ".") || !strcmp (d->name, ".."))
            continue;

          info = g_file_info_new ();
          g_file_info_set_size (info, d->size);
          g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID, d->uid);
          g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID, d->gid);
          g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE, d->mode);
          g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_INODE, d->inode);
          g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_NLINK, d->nlink);
          g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_DEVICE, d->dev);
          g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_RDEV, d->rdev);
          g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS, d->atime.tv_sec);
          g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC, d->atime.tv_usec);
          g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, d->mtime.tv_sec);
          g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, d->mtime.tv_usec);
          g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CHANGED, d->ctime.tv_sec);
          g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC, d->ctime.tv_usec);
          g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_BLOCK_SIZE, d->blksize);
          g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_BLOCKS, d->blocks);
          g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE, d->used);
          g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);

          etag = create_etag (d->mtime.tv_sec, d->mtime_nsec);
          g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE, etag);
          g_free (etag);

          switch (d->type)
            {
            case NF3REG:
              type = G_FILE_TYPE_REGULAR;
              break;
            case NF3DIR:
              type = G_FILE_TYPE_DIRECTORY;
              mimetype = "inode/directory";
              break;
            case NF3BLK:
              type = G_FILE_TYPE_SPECIAL;
              mimetype = "inode/blockdevice";
              break;
            case NF3CHR:
              type = G_FILE_TYPE_SPECIAL;
              mimetype = "inode/chardevice";
              break;
            case NF3SOCK:
              type = G_FILE_TYPE_SPECIAL;
              mimetype = "inode/socket";
              break;
            case NF3FIFO:
              type = G_FILE_TYPE_SPECIAL;
              mimetype = "inode/fifo";
              break;
            case NF3LNK:
              type = G_FILE_TYPE_SYMBOLIC_LINK;
              mimetype = "inode/symlink";
              g_file_info_set_is_symlink (info, TRUE);
              break;
            }

          g_file_info_set_file_type (info, type);
          set_name_info (info,
                         mimetype,
                         d->name,
                         op_job->attribute_matcher);

          if ((g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                                 G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE) ||
               g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                                 G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME)) &&
              handle->access_parent >= 0)
            {
              g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, handle->access_parent & W_OK);
              g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, handle->access_parent & W_OK);
            }

          if (d->type == NF3LNK &&
              g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                                G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET))
            {
              handle->readlink_list = g_slist_prepend (handle->readlink_list,
                                                       info);
              continue;
            }

          if (d->type == NF3LNK &&
              !(op_job->flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS))
            {
              handle->symlink_list = g_slist_prepend (handle->symlink_list,
                                                      info);
              continue;
            }

          if (handle->requires_access)
            {
              handle->access_list = g_slist_prepend (handle->access_list,
                                                     info);
              continue;
            }

          g_vfs_job_enumerate_add_info (op_job, info);
          g_object_unref (info);
        }

      nfs_closedir (ctx, dir);
      enumerate_continue (handle, ctx);
    }
  else
    {
      g_slice_free (EnumerateHandle, handle);
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static void
enumerate_access_parent_cb (int err,
                            struct nfs_context *ctx,
                            void *data, void *private_data)
{
  EnumerateHandle *handle = private_data;
  GVfsJobEnumerate *op_job = handle->op_job;

  handle->access_parent = err;

  nfs_opendir_async (ctx, op_job->filename, enumerate_cb, handle);
}

static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);
  EnumerateHandle *handle;

  handle = g_slice_new0 (EnumerateHandle);
  handle->op_job = job;
  handle->access_parent = -1;

  if (g_file_attribute_matcher_matches (attribute_matcher,
                                        G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE) ||
      g_file_attribute_matcher_matches (attribute_matcher,
                                        G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME))
    {
      nfs_access2_async (op_backend->ctx,
                         filename,
                         enumerate_access_parent_cb, handle);
    }
  else
    {
      nfs_opendir_async (op_backend->ctx, filename, enumerate_cb, handle);
    }

  return TRUE;
}

static void stat_access_parent_cb (int err,
                                   struct nfs_context *ctx,
                                   void *data, void *private_data);
static void stat_readlink_cb (int err,
                              struct nfs_context *ctx,
                              void *data, void *private_data);

static void
stat_access_cb (int err,
                struct nfs_context *ctx,
                void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);
  GVfsJobQueryInfo *op_job = G_VFS_JOB_QUERY_INFO (job);
  GFileInfo *info = op_job->file_info;

  if (err >= 0)
    {
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, err & R_OK);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, err & W_OK);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, err & X_OK);
    }

  if (g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                        G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME) ||
      g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                        G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE))
    {
      char *dirname = g_path_get_dirname (op_job->filename);
      nfs_access2_async (ctx, dirname, stat_access_parent_cb, job);
      g_free (dirname);
    }
  else if (g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                             G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET))
    {
      nfs_readlink_async (ctx, op_job->filename, stat_readlink_cb, job);
    }
  else
    {
      g_vfs_job_succeeded (job);
    }
}

static void
stat_access_parent_cb (int err,
                       struct nfs_context *ctx,
                       void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);
  GVfsJobQueryInfo *op_job = G_VFS_JOB_QUERY_INFO (job);
  GFileInfo *info = op_job->file_info;

  if (err >= 0)
    {
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, err & W_OK);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, err & W_OK);
    }

  if (g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                        G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET))
    nfs_readlink_async (ctx, op_job->filename, stat_readlink_cb, job);
  else
    g_vfs_job_succeeded (job);
}

static void
stat_readlink_cb (int err,
                  struct nfs_context *ctx,
                  void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);
  GVfsJobQueryInfo *op_job = G_VFS_JOB_QUERY_INFO (job);
  GFileInfo *info = op_job->file_info;

  if (err == 0)
    g_file_info_set_symlink_target (info, data);

  g_vfs_job_succeeded (job);
}

static void
stat_cb (int err, struct nfs_context *ctx, void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);

  if (err == 0)
    {
      GVfsJobQueryInfo *op_job = G_VFS_JOB_QUERY_INFO (job);
      GFileInfo *info = op_job->file_info;
      struct nfs_stat_64 *st = data;
      const char *mimetype;
      char *basename, *etag;

      set_info_from_stat (info, st, op_job->attribute_matcher);

      etag = create_etag (st->nfs_mtime, st->nfs_mtime_nsec);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE, etag);
      g_free (etag);

      mimetype = set_type_from_mode (info, st->nfs_mode);

      if (!strcmp (op_job->filename, "/"))
        {
          GMountSpec *mount_spec = g_vfs_backend_get_mount_spec (op_job->backend);
          basename = g_path_get_basename (mount_spec->mount_prefix);
        }
      else
        {
          basename = g_path_get_basename (op_job->filename);
        }
      set_name_info (info,
                     mimetype,
                     basename,
                     op_job->attribute_matcher);
      g_free (basename);

      if (g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                            G_FILE_ATTRIBUTE_ACCESS_CAN_READ) ||
          g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                            G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE) ||
          g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                            G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE))
        {
          nfs_access2_async (ctx, op_job->filename, stat_access_cb, job);
        }
      else if (g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                                 G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME) ||
               g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                                 G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE))
        {
          char *dirname = g_path_get_dirname (op_job->filename);
          nfs_access2_async (ctx, dirname, stat_access_parent_cb, job);
          g_free (dirname);
        }
      else if (g_file_attribute_matcher_matches (op_job->attribute_matcher,
                                                 G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET))
        {
          nfs_readlink_async (ctx, op_job->filename, stat_readlink_cb, job);
        }
      else
        {
          g_vfs_job_succeeded (job);
        }
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static void
stat_is_symlink_cb (int err,
                    struct nfs_context *ctx,
                    void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);

  if (err == 0)
    {
      GVfsJobQueryInfo *op_job = G_VFS_JOB_QUERY_INFO (job);
      struct nfs_stat_64 *st = data;

      /* In the case that symlinks are not followed, this is set by
       * set_type_from_mode in stat_cb(). */
      g_file_info_set_is_symlink (op_job->file_info, S_ISLNK (st->nfs_mode));

      /* If the filename is a link, call stat to get the real info.
       * Otherwise, lstat is the same as stat, so just chain straight to the
       * stat callback. */
      if (S_ISLNK (st->nfs_mode))
        nfs_stat64_async (ctx, op_job->filename, stat_cb, job);
      else
        stat_cb (err, ctx, data, private_data);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static gboolean
try_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);

  if (flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS)
    nfs_lstat64_async (op_backend->ctx, filename, stat_cb, job);
  else
    nfs_lstat64_async (op_backend->ctx, filename, stat_is_symlink_cb, job);

  return TRUE;
}

static gboolean
try_set_display_name (GVfsBackend *backend,
                      GVfsJobSetDisplayName *job,
                      const char *filename,
                      const char *display_name)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);
  char *dirname, *basename, *new_name;

  dirname = g_path_get_dirname (filename);
  basename = g_filename_from_utf8 (display_name, -1, NULL, NULL, NULL);
  if (basename == NULL)
    basename = g_strdup (display_name);
  new_name = g_build_filename (dirname, basename, NULL);
  g_free (dirname);
  g_free (basename);

  g_vfs_job_set_display_name_set_new_path (job, new_name);

  nfs_rename_async (op_backend->ctx, filename, new_name, generic_cb, job);
  g_free (new_name);

  return TRUE;
}

static gboolean
try_query_settable_attributes (GVfsBackend *backend,
                               GVfsJobQueryAttributes *job,
                               const char *filename)
{
  GFileAttributeInfoList *list;

  list = g_file_attribute_info_list_new ();

  g_file_attribute_info_list_add (list,
                                  G_FILE_ATTRIBUTE_TIME_ACCESS,
                                  G_FILE_ATTRIBUTE_TYPE_UINT64,
                                  G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
  g_file_attribute_info_list_add (list,
                                  G_FILE_ATTRIBUTE_TIME_ACCESS_USEC,
                                  G_FILE_ATTRIBUTE_TYPE_UINT32,
                                  G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
  g_file_attribute_info_list_add (list,
                                  G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                  G_FILE_ATTRIBUTE_TYPE_UINT64,
                                  G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                  G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
  g_file_attribute_info_list_add (list,
                                  G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
                                  G_FILE_ATTRIBUTE_TYPE_UINT32,
                                  G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                  G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
  g_file_attribute_info_list_add (list,
                                  G_FILE_ATTRIBUTE_UNIX_UID,
                                  G_FILE_ATTRIBUTE_TYPE_UINT32,
                                  G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
  g_file_attribute_info_list_add (list,
                                  G_FILE_ATTRIBUTE_UNIX_GID,
                                  G_FILE_ATTRIBUTE_TYPE_UINT32,
                                  G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
  g_file_attribute_info_list_add (list,
                                  G_FILE_ATTRIBUTE_UNIX_MODE,
                                  G_FILE_ATTRIBUTE_TYPE_UINT32,
                                  G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                  G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);

  g_vfs_job_query_attributes_set_list (job, list);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_file_attribute_info_list_unref (list);

  return TRUE;
}

static void
set_mod_cb (int err, struct nfs_context *ctx, void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);

  if (err == 0)
    {
      GVfsJobSetAttribute *op_job = G_VFS_JOB_SET_ATTRIBUTE (job);
      struct nfs_stat_64 *st = data;
      gpointer *value_p = _g_dbus_attribute_as_pointer (op_job->type,
                                                        &op_job->value);
      struct timeval tv[2];

      if (!strcmp (op_job->attribute, G_FILE_ATTRIBUTE_TIME_ACCESS))
        tv[0].tv_sec = *(guint64 *)value_p;
      else
        tv[0].tv_sec = st->nfs_atime;
      if (!strcmp (op_job->attribute, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC))
        tv[0].tv_usec = *(guint32 *)value_p;
      else
        tv[0].tv_usec = st->nfs_atime_nsec / 1000;

      if (!strcmp (op_job->attribute, G_FILE_ATTRIBUTE_TIME_MODIFIED))
        tv[1].tv_sec = *(guint64 *)value_p;
      else
        tv[1].tv_sec = st->nfs_mtime;
      if (!strcmp (op_job->attribute, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC))
        tv[1].tv_usec = *(guint32 *)value_p;
      else
        tv[1].tv_usec = st->nfs_mtime_nsec / 1000;

      if (op_job->flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS)
        nfs_lutimes_async (ctx, op_job->filename, tv, generic_cb, job);
      else
        nfs_utimes_async (ctx, op_job->filename, tv, generic_cb, job);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static gboolean
try_set_attribute (GVfsBackend *backend,
                   GVfsJobSetAttribute *job,
                   const char *filename,
                   const char *attribute,
                   GFileAttributeType type,
                   gpointer value_p,
                   GFileQueryInfoFlags flags)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);

  if (!strcmp (attribute, G_FILE_ATTRIBUTE_TIME_ACCESS) ||
      !strcmp (attribute, G_FILE_ATTRIBUTE_TIME_MODIFIED))
    {
      if (type != G_FILE_ATTRIBUTE_TYPE_UINT64)
        goto error;

      if (flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS)
        nfs_lstat64_async (op_backend->ctx, filename, set_mod_cb, job);
      else
        nfs_stat64_async (op_backend->ctx, filename, set_mod_cb, job);
    }
  else if (!strcmp (attribute, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC) ||
           !strcmp (attribute, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC))
    {
      if (type != G_FILE_ATTRIBUTE_TYPE_UINT32)
        goto error;

      if (flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS)
        nfs_lstat64_async (op_backend->ctx, filename, set_mod_cb, job);
      else
        nfs_stat64_async (op_backend->ctx, filename, set_mod_cb, job);
    }
  else if (!strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_UID))
    {
      if (type != G_FILE_ATTRIBUTE_TYPE_UINT32)
        goto error;

      if (flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS)
        nfs_lchown_async (op_backend->ctx,
                          filename,
                          *(guint32 *)value_p, -1,
                          generic_cb, job);
      else
        nfs_chown_async (op_backend->ctx,
                         filename,
                         *(guint32 *)value_p, -1,
                         generic_cb, job);
    }
  else if (!strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_GID))
    {
      if (type != G_FILE_ATTRIBUTE_TYPE_UINT32)
        goto error;

      if (flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS)
        nfs_lchown_async (op_backend->ctx,
                          filename,
                          -1, *(guint32 *)value_p,
                          generic_cb, job);
      else
        nfs_chown_async (op_backend->ctx,
                         filename,
                         -1, *(guint32 *)value_p,
                         generic_cb, job);
    }
  else if (!strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_MODE))
    {
      if (type != G_FILE_ATTRIBUTE_TYPE_UINT32)
        goto error;

      if (flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS)
        nfs_lchmod_async (op_backend->ctx,
                          filename,
                          *(guint32 *)value_p & 0777,
                          generic_cb, job);
      else
        nfs_chmod_async (op_backend->ctx,
                         filename,
                         *(guint32 *)value_p & 0777,
                         generic_cb, job);
    }
  else
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                _("Operation not supported"));
    }

  return TRUE;

error:
  g_vfs_job_failed_literal (G_VFS_JOB (job),
                            G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            _("Invalid attribute type"));
  return TRUE;
}

static gboolean
try_unmount (GVfsBackend *backend,
             GVfsJobUnmount *job,
             GMountUnmountFlags flags,
             GMountSource *mount_source)
{
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

typedef struct
{
  GVfsJob *job;
  gboolean source_is_dir;
  uint64_t file_size;
} MoveHandle;

static void
move_rename_cb (int err,
                struct nfs_context *ctx,
                void *data, void *private_data)
{
  MoveHandle *handle = private_data;
  GVfsJob *job = handle->job;
  uint64_t file_size = handle->file_size;

  g_slice_free (MoveHandle, handle);

  if (err == 0)
    {
      g_vfs_job_progress_callback (file_size, file_size, job);
      g_vfs_job_succeeded (job);
    }
  else if (err == -EXDEV)
    {
      g_vfs_job_failed_literal (job,
                                G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                _("Operation not supported"));
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static void
move_remove_cb (int err,
                struct nfs_context *ctx,
                void *data, void *private_data)
{
  MoveHandle *handle = private_data;
  GVfsJob *job = handle->job;

  if (err == 0)
    {
      GVfsJobMove *op_job = G_VFS_JOB_MOVE (job);

      nfs_rename_async (ctx,
                        op_job->source, op_job->destination,
                        move_rename_cb, handle);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
      g_slice_free (MoveHandle, handle);
    }
}

static void
move_stat_dest_cb (int err,
                   struct nfs_context *ctx,
                   void *data, void *private_data)
{
  MoveHandle *handle = private_data;
  GVfsJob *job = handle->job;
  GVfsJobMove *op_job = G_VFS_JOB_MOVE (job);

  if (err == 0)
    {
      struct nfs_stat_64 *st = data;

      if (op_job->flags & G_FILE_COPY_OVERWRITE)
        {
          if (S_ISDIR (st->nfs_mode))
            {
              if (handle->source_is_dir)
                {
                  g_vfs_job_failed_literal (job,
                                            G_IO_ERROR, G_IO_ERROR_WOULD_MERGE,
                                            _("Can’t move directory over directory"));
                }
              else
                {
                  g_vfs_job_failed_literal (job,
                                            G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                            _("File is directory"));
                }
              g_slice_free (MoveHandle, handle);
              return;
            }
        }
      else
        {
          g_vfs_job_failed (job,
                            G_IO_ERROR, G_IO_ERROR_EXISTS,
                            _("Target file already exists"));
          g_slice_free (MoveHandle, handle);
          return;
        }

      if (handle->source_is_dir && op_job->flags & G_FILE_COPY_OVERWRITE)
        {
          nfs_unlink_async (ctx, op_job->destination, move_remove_cb, handle);
          return;
        }
    }

  nfs_rename_async (ctx,
                    op_job->source, op_job->destination,
                    move_rename_cb, handle);
}

static void
move_stat_source_cb (int err,
                     struct nfs_context *ctx,
                     void *data, void *private_data)
{
  GVfsJob *job = G_VFS_JOB (private_data);

  if (err == 0)
    {
      GVfsJobMove *op_job = G_VFS_JOB_MOVE (job);
      struct nfs_stat_64 *st = data;
      MoveHandle *handle;

      handle = g_slice_new0 (MoveHandle);
      handle->job = job;
      handle->source_is_dir = S_ISDIR (st->nfs_mode);
      handle->file_size = st->nfs_size;

      nfs_lstat64_async (ctx, op_job->destination, move_stat_dest_cb, handle);
    }
  else
    {
      g_vfs_job_failed_from_errno (job, -err);
    }
}

static gboolean
try_move (GVfsBackend *backend,
          GVfsJobMove *job,
          const char *source,
          const char *destination,
          GFileCopyFlags flags,
          GFileProgressCallback progress_callback,
          gpointer progress_callback_data)
{
  GVfsBackendNfs *op_backend = G_VFS_BACKEND_NFS (backend);

  if (flags & G_FILE_COPY_BACKUP)
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                _("Operation not supported"));
      return TRUE;
    }

  nfs_lstat64_async (op_backend->ctx, source, move_stat_source_cb, job);

  return TRUE;
}

static void
g_vfs_backend_nfs_class_init (GVfsBackendNfsClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

  gobject_class->finalize = g_vfs_backend_nfs_finalize;

  backend_class->mount = do_mount;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_read = try_read;
  backend_class->try_query_info_on_read = try_query_info_on_read;
  backend_class->try_seek_on_read = try_seek_on_read;
  backend_class->try_close_read = try_close_read;
  backend_class->try_make_directory = try_make_directory;
  backend_class->try_delete = try_delete;
  backend_class->try_make_symlink = try_make_symlink;
  backend_class->try_create = try_create;
  backend_class->try_append_to = try_append_to;
  backend_class->try_edit = try_edit;
  backend_class->try_replace = try_replace;
  backend_class->try_write = try_write;
  backend_class->try_query_info_on_write = try_query_info_on_write;
  backend_class->try_seek_on_write = try_seek_on_write;
  backend_class->try_truncate = try_truncate;
  backend_class->try_close_write = try_close_write;
  backend_class->try_query_fs_info = try_query_fs_info;
  backend_class->try_enumerate = try_enumerate;
  backend_class->try_query_info = try_query_info;
  backend_class->try_set_display_name = try_set_display_name;
  backend_class->try_query_settable_attributes = try_query_settable_attributes;
  backend_class->try_set_attribute = try_set_attribute;
  backend_class->try_unmount = try_unmount;
  backend_class->try_move = try_move;
}
