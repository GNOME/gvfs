 /* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) Carl-Anton Ingmarsson 2011 <ca.ingmarsson@gmail.com>
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
 * Author: Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
 */

#include <config.h>

#include <stdlib.h>
#include <sys/stat.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#ifdef HAVE_GCRYPT
#include <gcrypt.h>
#endif

#include "gvfsjobmount.h"
#include "gvfsjobunmount.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobsetattribute.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobcloseread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobtruncate.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobdelete.h"
#include "gvfsjobmakedirectory.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobmove.h"
#include "gvfsjobcopy.h"
#include "gvfsutils.h"

#include "gvfsafpserver.h"
#include "gvfsafpvolume.h"

#include "gvfsbackendafp.h"

struct _GVfsBackendAfpClass
{
  GVfsBackendClass parent_class;
};

struct _GVfsBackendAfp
{
  GVfsBackend parent_instance;

  GNetworkAddress    *addr;
  char               *volume_name;
  char               *user;

  GVfsAfpServer      *server;
  GVfsAfpVolume      *volume;

  guint32             user_id;
  guint32             group_id;
};


G_DEFINE_TYPE (GVfsBackendAfp, g_vfs_backend_afp, G_VFS_TYPE_BACKEND);


/*
 * Utility functions
 */

static void
copy_file_info_into (GFileInfo *src, GFileInfo *dest)
{
  char **attrs;
  gint i;

  attrs = g_file_info_list_attributes (src, NULL);

  for (i = 0; attrs[i]; i++)
  {
    GFileAttributeType type;
    gpointer value;
    
    g_file_info_get_attribute_data (src, attrs[i], &type, &value, NULL);
    g_file_info_set_attribute (dest, attrs[i], type, value);
  }

  g_strfreev (attrs);
}

typedef struct
{
  GVfsBackendAfp *backend;

  gint16 fork_refnum;
  gint64 offset;

  /* For write only */
  gint64 size;
  GVfsJobOpenForWriteMode mode;

  /* For replace only */
  char *filename;
  char *tmp_filename;
  gboolean make_backup;
} AfpHandle;

static AfpHandle *
afp_handle_new (GVfsBackendAfp *backend, gint16 fork_refnum)
{
  AfpHandle *afp_handle;

  afp_handle = g_slice_new0 (AfpHandle);
  afp_handle->backend = backend;
  afp_handle->fork_refnum = fork_refnum;

  return afp_handle;
}

static void
afp_handle_free (AfpHandle *afp_handle)
{
  g_free (afp_handle->filename);
  g_free (afp_handle->tmp_filename);
  
  g_slice_free (AfpHandle, afp_handle);
}

/*
 * Backend code
 */

typedef struct
{
  GVfsJobCopy  *job;
  GAsyncResult *source_parms_res;
  GAsyncResult *dest_parms_res;
  goffset size;
} CopyData;

static void
copy_data_free (CopyData *copy_data)
{
  g_object_unref (copy_data->source_parms_res);
  g_object_unref (copy_data->dest_parms_res);

  g_slice_free (CopyData, copy_data);
}

static void
copy_copy_file_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  CopyData *copy_data = user_data;
  GVfsJobCopy *job = copy_data->job;
  goffset size;

  GError *err = NULL;

  size = copy_data->size;
  copy_data_free (copy_data);
  
  if (!g_vfs_afp_volume_copy_file_finish (volume, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_progress_callback (size, size, job);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
copy_delete_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  CopyData *copy_data = user_data;
  GVfsJobCopy *job = copy_data->job;
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);

  GError *err = NULL;
  
  if (!g_vfs_afp_volume_delete_finish (volume, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    copy_data_free (copy_data);
    return;
  }

  g_vfs_afp_volume_copy_file (afp_backend->volume, job->source, job->destination,
                              G_VFS_JOB (job)->cancellable, copy_copy_file_cb, copy_data);
}

static void
do_copy (CopyData *copy_data)
{
  GVfsJobCopy *job = copy_data->job;
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);
  
  GFileInfo *info;
  GError *err = NULL;

  gboolean source_is_dir;
  gboolean dest_exists;
  gboolean dest_is_dir;
  
  info = g_vfs_afp_volume_get_filedir_parms_finish (afp_backend->volume, copy_data->source_parms_res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    goto error;
  }
  copy_data->size = g_file_info_get_size (info);

  /* If the source is a directory, don't fail with WOULD_RECURSE immediately,
   * as that is less useful to the app. Better check for errors on the
   * target instead.
   */
  source_is_dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY ? TRUE : FALSE;
  g_object_unref (info);

  info = g_vfs_afp_volume_get_filedir_parms_finish (afp_backend->volume, copy_data->dest_parms_res, &err);
  if (!info)
  {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_clear_error (&err);
      dest_exists = FALSE;
    }
    else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
      g_error_free (err);
      goto error;
    }
  }
  else
  {
    dest_exists = TRUE;
    dest_is_dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY ? TRUE : FALSE;
    g_object_unref (info);
  }

  /* Check target errors */
  if (dest_exists)
  {
    if ((job->flags & G_FILE_COPY_OVERWRITE))
    {
      /* Always fail on dirs, even with overwrite */
      if (dest_is_dir)
      {
        if (source_is_dir)
          g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_WOULD_MERGE,
                                    _("Can’t copy directory over directory"));
        else
          g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                    _("File is directory"));
        goto error;
      }
    }
    else
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS,
                        _("Target file already exists"));
      goto error;
    }
  }

  /* Now we fail if the source is a directory */
  if (source_is_dir)
  {
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
                      _("Can’t recursively copy directory"));
    goto error;
  }

  if (dest_exists)
  {
    g_vfs_afp_volume_delete (afp_backend->volume, job->destination,
                             G_VFS_JOB (job)->cancellable, copy_delete_cb, copy_data);
  }
  else
  {
    g_vfs_afp_volume_copy_file (afp_backend->volume, job->source, job->destination,
                                G_VFS_JOB (job)->cancellable, copy_copy_file_cb, copy_data);
  }
  return;
  
error:
  copy_data_free (copy_data);
  return;
}

static void
copy_get_dest_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  CopyData *copy_data = (CopyData *)user_data;
  
  copy_data->dest_parms_res = g_object_ref (res);
  if (copy_data->source_parms_res)
    do_copy (copy_data);
}

static void
copy_get_source_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  CopyData *copy_data = (CopyData *)user_data;

  copy_data->source_parms_res = g_object_ref (res);
  if (copy_data->dest_parms_res)
    do_copy (copy_data);
}

static gboolean
try_copy (GVfsBackend *backend,
          GVfsJobCopy *job,
          const char *source,
          const char *destination,
          GFileCopyFlags flags,
          GFileProgressCallback progress_callback,
          gpointer progress_callback_data)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  CopyData *copy_data;

  copy_data = g_slice_new0 (CopyData);
  copy_data->job = job;

  g_vfs_afp_volume_get_filedir_parms (afp_backend->volume, source,
                                      AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT |
                                        AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT,
                                      AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                                      G_VFS_JOB (job)->cancellable, copy_get_source_parms_cb,
                                      copy_data);

  g_vfs_afp_volume_get_filedir_parms (afp_backend->volume, destination,
                                      AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                                      AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                                      G_VFS_JOB (job)->cancellable, copy_get_dest_parms_cb,
                                      copy_data);

  return TRUE;
}

static void
move_move_and_rename_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobMove *job = G_VFS_JOB_MOVE (user_data);

  GError *err = NULL;
  
  if (!g_vfs_afp_volume_move_and_rename_finish (volume, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
move_delete_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobMove *job = G_VFS_JOB_MOVE (user_data);

  GError *err = NULL;
  
  if (!g_vfs_afp_volume_delete_finish (volume, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_afp_volume_move_and_rename (volume, job->source, job->destination,
                                    G_VFS_JOB (job)->cancellable, move_move_and_rename_cb,
                                    job);
}

typedef struct
{
  GVfsJobMove *job;
  GAsyncResult *source_parms_res;
  GAsyncResult *dest_parms_res;
} MoveData;

static void
free_move_data (MoveData *move_data)
{
  g_object_unref (move_data->source_parms_res);
  g_object_unref (move_data->dest_parms_res);

  g_slice_free (MoveData, move_data);
}

static void
do_move (MoveData *move_data)
{
  GVfsJobMove *job = move_data->job;
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);
  
  GFileInfo *info;
  GError *err = NULL;

  gboolean source_is_dir;
  gboolean dest_exists;
  gboolean dest_is_dir;
  
  info = g_vfs_afp_volume_get_filedir_parms_finish (afp_backend->volume,
                                                    move_data->source_parms_res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    goto done;
  }

  source_is_dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY ? TRUE : FALSE;
  g_object_unref (info);

  info = g_vfs_afp_volume_get_filedir_parms_finish (afp_backend->volume, 
                                                    move_data->dest_parms_res, &err);
  if (!info)
  {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_clear_error (&err);
      dest_exists = FALSE;
    }
    else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
      g_error_free (err);
      goto done;
    }
  }
  else
  {
    dest_exists = TRUE;
    dest_is_dir = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY ? TRUE : FALSE;
    g_object_unref (info);
  }

  if (dest_exists)
  {
    if ((job->flags & G_FILE_COPY_OVERWRITE))
    {
      /* Always fail on dirs, even with overwrite */
      if (dest_is_dir)
      {
        if (source_is_dir)
          g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_WOULD_MERGE,
                                    _("Can’t move directory over directory"));
        else
          g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                    _("File is directory"));
        goto done;
      }
    }
    else
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS,
                        _("Target file already exists"));
      goto done;
    }

    g_vfs_afp_volume_delete (afp_backend->volume, job->destination,
                             G_VFS_JOB (job)->cancellable, move_delete_cb, job);
  }
  else
    g_vfs_afp_volume_move_and_rename (afp_backend->volume, job->source, job->destination,
                                      G_VFS_JOB (job)->cancellable, move_move_and_rename_cb,
                                      job);

done:
  free_move_data (move_data);
  return;
}

static void
move_get_dest_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  MoveData *move_data = (MoveData *)user_data;
  
  move_data->dest_parms_res = g_object_ref (res);
  if (move_data->source_parms_res)
    do_move (move_data);
}

static void
move_get_source_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  MoveData *move_data = (MoveData *)user_data;

  move_data->source_parms_res = g_object_ref (res);
  if (move_data->dest_parms_res)
    do_move (move_data);
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
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  MoveData *move_data;
  
  move_data = g_slice_new0 (MoveData);
  move_data->job = job;
  
  g_vfs_afp_volume_get_filedir_parms (afp_backend->volume, source,
                                      AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                                      AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                                      G_VFS_JOB (job)->cancellable, move_get_source_parms_cb,
                                      move_data);

  g_vfs_afp_volume_get_filedir_parms (afp_backend->volume, destination,
                                      AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                                      AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT,
                                      G_VFS_JOB (job)->cancellable, move_get_dest_parms_cb,
                                      move_data);  

  return TRUE;
}

static void
rename_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobSetDisplayName *job = G_VFS_JOB_SET_DISPLAY_NAME (user_data);

  GError *err = NULL;
  char *dirname, *newpath;

  if (!g_vfs_afp_volume_rename_finish (volume, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  dirname = g_path_get_dirname (job->filename);
  newpath = g_build_filename (dirname, job->display_name, NULL);
  g_vfs_job_set_display_name_set_new_path (job, newpath);

  g_free (dirname);
  g_free (newpath);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}
  
static gboolean
try_set_display_name (GVfsBackend *backend,
                      GVfsJobSetDisplayName *job,
                      const char *filename,
                      const char *display_name)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  
  if (is_root (filename))
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                              _("Can’t rename volume"));
    return TRUE;
  }

  g_vfs_afp_volume_rename (afp_backend->volume, filename, display_name,
                           G_VFS_JOB (job)->cancellable, rename_cb, job);
  return TRUE;
}

static void
create_directory_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobMakeDirectory *job = G_VFS_JOB_MAKE_DIRECTORY (user_data);

  GError *err = NULL;

  if (!g_vfs_afp_volume_create_directory_finish (volume, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean 
try_make_directory (GVfsBackend *backend,
                    GVfsJobMakeDirectory *job,
                    const char *filename)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  g_vfs_afp_volume_create_directory (afp_backend->volume, job->filename,
                                     G_VFS_JOB (job)->cancellable,
                                     create_directory_cb, job);
  return TRUE;
}

static void
delete_delete_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobDelete *job = G_VFS_JOB_DELETE (user_data);

  GError *err = NULL;

  if (!g_vfs_afp_volume_delete_finish (volume, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean 
try_delete (GVfsBackend *backend,
            GVfsJobDelete *job,
            const char *filename)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  g_vfs_afp_volume_delete (afp_backend->volume, filename,
                           G_VFS_JOB (job)->cancellable, delete_delete_cb, job);
  
  return TRUE;
}

static void
write_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobWrite *job = G_VFS_JOB_WRITE (user_data);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  GError *err = NULL;
  gint64 last_written;
  gsize written_size;

  if (!g_vfs_afp_volume_write_to_fork_finish (volume, res, &last_written, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  written_size = last_written - afp_handle->offset;
  afp_handle->offset = last_written;
  afp_handle->size = MAX (last_written, afp_handle->size);
  
  g_vfs_job_write_set_written_size (job, written_size); 
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_write (GVfsBackend *backend,
           GVfsJobWrite *job,
           GVfsBackendHandle handle,
           char *buffer,
           gsize buffer_size)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;

  if (afp_handle->mode == OPEN_FOR_WRITE_APPEND)
    afp_handle->offset = afp_handle->size;

  g_vfs_afp_volume_write_to_fork (afp_backend->volume, afp_handle->fork_refnum,
                                  buffer, buffer_size, afp_handle->offset,
                                  G_VFS_JOB (job)->cancellable, write_cb, job);

  return TRUE;
}

static gboolean
try_seek_on_write (GVfsBackend *backend,
                   GVfsJobSeekWrite *job,
                   GVfsBackendHandle handle,
                   goffset    offset,
                   GSeekType  type)
{
  AfpHandle *afp_handle = (AfpHandle *)handle;

  switch (job->seek_type)
  {
    case G_SEEK_CUR:
      afp_handle->offset += job->requested_offset;
      break;
    case G_SEEK_SET:
      afp_handle->offset = job->requested_offset;
      break;
    case G_SEEK_END:
      afp_handle->offset = afp_handle->size + job->requested_offset;
      break;
  }

  if (afp_handle->offset < 0)
    afp_handle->offset = 0;

  g_vfs_job_seek_write_set_offset (job, afp_handle->offset);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static void
truncate_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobTruncate *job = G_VFS_JOB_TRUNCATE (user_data);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  GError *err = NULL;

  if (g_vfs_afp_volume_set_fork_size_finish (volume, res, &err))
  {
    afp_handle->size = job->size;
    g_vfs_job_succeeded (G_VFS_JOB (job));
  }
  else
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
  }
}

static gboolean
try_truncate (GVfsBackend *backend,
              GVfsJobTruncate *job,
              GVfsBackendHandle handle,
              goffset size)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;

  g_vfs_afp_volume_set_fork_size (afp_backend->volume, afp_handle->fork_refnum,
                                  size, G_VFS_JOB (job)->cancellable,
                                  truncate_cb, job);

  return TRUE;
}

static void
seek_on_read_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobSeekRead *job = G_VFS_JOB_SEEK_READ (user_data);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  GError *err = NULL;
  GFileInfo *info;
  gsize size;

  info = g_vfs_afp_volume_get_fork_parms_finish (volume, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }
  
  size = g_file_info_get_size (info);
  g_object_unref (info);

  afp_handle->offset = size + job->requested_offset;

  if (afp_handle->offset < 0)
    afp_handle->offset = 0;

  g_vfs_job_seek_read_set_offset (job, afp_handle->offset);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_seek_on_read (GVfsBackend *backend,
                  GVfsJobSeekRead *job,
                  GVfsBackendHandle handle,
                  goffset    offset,
                  GSeekType  type)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;

  switch (job->seek_type)
  {
    case G_SEEK_CUR:
      afp_handle->offset += job->requested_offset;
      break;
    case G_SEEK_SET:
      afp_handle->offset = job->requested_offset;
      break;
    case G_SEEK_END:
      g_vfs_afp_volume_get_fork_parms (afp_backend->volume, afp_handle->fork_refnum,
                                 AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT,
                                 G_VFS_JOB (job)->cancellable, seek_on_read_cb, job);
      return TRUE;
  }

  if (afp_handle->offset < 0)
    afp_handle->offset = 0;

  g_vfs_job_seek_read_set_offset (job, afp_handle->offset);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static void
read_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobRead *job = G_VFS_JOB_READ (user_data);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  GError *err = NULL;
  gsize bytes_read;

  if (!g_vfs_afp_volume_read_from_fork_finish (volume, res, &bytes_read, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  afp_handle->offset += bytes_read;
  g_vfs_job_read_set_size (job, bytes_read);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
}
  
static gboolean 
try_read (GVfsBackend *backend,
          GVfsJobRead *job,
          GVfsBackendHandle handle,
          char *buffer,
          gsize bytes_requested)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;

  g_vfs_afp_volume_read_from_fork (afp_backend->volume, afp_handle->fork_refnum,
                                   buffer, bytes_requested, afp_handle->offset,
                                   G_VFS_JOB (job)->cancellable, read_cb, job);
  return TRUE;
}

static void
close_replace_get_filedir_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobCloseWrite *job = G_VFS_JOB_CLOSE_WRITE (user_data);

  GFileInfo *info;
  GError *err = NULL;

  info = g_vfs_afp_volume_get_filedir_parms_finish (volume, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_close_write_set_etag (job, g_file_info_get_etag (info));
  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  g_object_unref (info);
}

static void
close_replace_delete_backup_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  AfpHandle *afp_handle = (AfpHandle *)user_data;

  char *backup_name;

  /* We ignore all errors and just try to rename the temporary file anyway */
  backup_name = g_strconcat (afp_handle->filename, "~", NULL);

  g_vfs_afp_volume_move_and_rename (volume, afp_handle->tmp_filename, backup_name,
                                    NULL, NULL, NULL);
  afp_handle_free (afp_handle);
}

static void
close_replace_close_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  AfpHandle *afp_handle = (AfpHandle *)user_data;

  if (afp_handle->make_backup)
  {
    char *backup_name = g_strconcat (afp_handle->filename, "~", NULL);
    
    /* Delete old backup */
    g_vfs_afp_volume_delete (volume, backup_name, NULL,
                             close_replace_delete_backup_cb, afp_handle);
    g_free (backup_name);
  }

  else
  {
    /* Delete temporary file */
    g_vfs_afp_volume_delete (volume, afp_handle->tmp_filename, NULL, NULL, NULL);
    
    afp_handle_free (afp_handle);
  }
}

static void
close_replace_exchange_files_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobCloseWrite *job = G_VFS_JOB_CLOSE_WRITE (user_data);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  GError *err = NULL;

  if (!g_vfs_afp_volume_exchange_files_finish (volume, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    afp_handle_free (afp_handle);
    return;
  }

  /* Close fork and remove/rename the temporary file even if the exchange failed */
  g_vfs_afp_volume_close_fork (volume, afp_handle->fork_refnum,
                               G_VFS_JOB (job)->cancellable,
                               close_replace_close_fork_cb, job->handle);
  
  /* Get ETAG */
  g_vfs_afp_volume_get_filedir_parms (volume, afp_handle->filename,
                                      AFP_FILE_BITMAP_MOD_DATE_BIT, 0,
                                      G_VFS_JOB (job)->cancellable,
                                      close_replace_get_filedir_parms_cb, job);
}

static void
close_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJob *job = G_VFS_JOB (user_data);

  GError *err = NULL;

  if (!g_vfs_afp_volume_close_fork_finish (volume, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
close_fork (GVfsAfpVolume  *volume,
            GVfsJob        *job,
            AfpHandle      *afp_handle)
{
  g_vfs_afp_volume_close_fork (volume, afp_handle->fork_refnum,
                               G_VFS_JOB (job)->cancellable,
                               close_fork_cb, job);
  afp_handle_free (afp_handle);
}

static void
close_write_get_fork_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobCloseWrite *job = G_VFS_JOB_CLOSE_WRITE (user_data);

  AfpHandle *afp_handle = (AfpHandle *)job->handle;

  GFileInfo *info;

  info = g_vfs_afp_volume_get_fork_parms_finish (volume, res, NULL);
  if (info)
    g_vfs_job_close_write_set_etag (job, g_file_info_get_etag (info));

  close_fork (volume, G_VFS_JOB (job), afp_handle);
}

static gboolean
try_close_write (GVfsBackend *backend,
                 GVfsJobCloseWrite *job,
                 GVfsBackendHandle handle)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;
  
  if (afp_handle->tmp_filename != NULL)
  {
    g_vfs_afp_volume_exchange_files (afp_backend->volume, afp_handle->filename,
                                     afp_handle->tmp_filename, 
                                     G_VFS_JOB (job)->cancellable,
                                     close_replace_exchange_files_cb, job);
  }
  else
  {
    /* Get ETAG */
    g_vfs_afp_volume_get_fork_parms (afp_backend->volume, afp_handle->fork_refnum,
                                     AFP_FILE_BITMAP_MOD_DATE_BIT,
                                     G_VFS_JOB (job)->cancellable,
                                     close_write_get_fork_parms_cb, job);
  }
  
  return TRUE;
}

static gboolean
try_close_read (GVfsBackend *backend,
                GVfsJobCloseRead *job,
                GVfsBackendHandle handle)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  AfpHandle *afp_handle = (AfpHandle *)handle;

  close_fork (afp_backend->volume, G_VFS_JOB (job), afp_handle);
  
  return TRUE;
}

static void
open_for_write_get_fork_parms_cb (GObject *source_object,
                                  GAsyncResult *res,
                                  gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobOpenForWrite *job = G_VFS_JOB_OPEN_FOR_WRITE (user_data);
  AfpHandle *afp_handle = (AfpHandle *)job->backend_handle;
  GFileInfo *info;
  GError *err = NULL;
  goffset size;

  info = g_vfs_afp_volume_get_fork_parms_finish (volume, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    afp_handle_free (afp_handle);
    return;
  }

  size = g_file_info_get_size (info);
  g_object_unref (info);

  afp_handle->offset = size;
  afp_handle->size = size;
  g_vfs_job_open_for_write_set_initial_offset (job, size);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
open_for_write_open_fork_cb (GObject *source_object,
                             GAsyncResult *res,
                             gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobOpenForWrite *job = G_VFS_JOB_OPEN_FOR_WRITE (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);

  gint16 fork_refnum;
  GError *err = NULL;
  AfpHandle *afp_handle;
  
  if (!g_vfs_afp_volume_open_fork_finish (volume, res, &fork_refnum, NULL, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  afp_handle = afp_handle_new (afp_backend, fork_refnum);
  afp_handle->mode = job->mode;
  
  g_vfs_job_open_for_write_set_handle (job, (GVfsBackendHandle) afp_handle);
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
  g_vfs_job_open_for_write_set_can_truncate (job, TRUE);

  if (job->mode == OPEN_FOR_WRITE_APPEND)
    {
      g_vfs_afp_volume_get_fork_parms (afp_backend->volume,
                                       afp_handle->fork_refnum,
                                       AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT,
                                       G_VFS_JOB (job)->cancellable,
                                       open_for_write_get_fork_parms_cb,
                                       job);
      return ;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
open_for_write_create_file_cb (GObject *source_object,
                               GAsyncResult *res,
                               gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobOpenForWrite *job = G_VFS_JOB_OPEN_FOR_WRITE (user_data);

  GError *err = NULL;

  if (!g_vfs_afp_volume_create_file_finish (volume, res, &err) &&
      (job->mode == OPEN_FOR_WRITE_CREATE ||
       !g_error_matches (err, G_IO_ERROR, G_IO_ERROR_EXISTS)))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_clear_error (&err);
  g_vfs_afp_volume_open_fork (volume,
                              job->filename,
                              AFP_ACCESS_MODE_WRITE_BIT,
                              0,
                              G_VFS_JOB (job)->cancellable,
                              open_for_write_open_fork_cb,
                              job);
}

static void
open_for_write (GVfsBackend *backend,
                GVfsJobOpenForWrite *job,
                const char *filename)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  g_vfs_afp_volume_create_file (afp_backend->volume,
                                filename,
                                FALSE,
                                G_VFS_JOB (job)->cancellable,
                                open_for_write_create_file_cb,
                                job);
}

static gboolean
try_create (GVfsBackend *backend,
            GVfsJobOpenForWrite *job,
            const char *filename,
            GFileCreateFlags flags)
{
  open_for_write (backend, job, filename);

  return TRUE;
}

static void
replace_set_fork_size_cb (GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobTruncate *job = G_VFS_JOB_TRUNCATE (user_data);
  AfpHandle *afp_handle = (AfpHandle *)job->handle;
  GError *err = NULL;

  if (!g_vfs_afp_volume_set_fork_size_finish (volume, res, &err))
  {
    g_vfs_afp_volume_close_fork (volume,
                                 afp_handle->fork_refnum,
                                 G_VFS_JOB (job)->cancellable,
                                 NULL,
                                 NULL);
    afp_handle_free (afp_handle);
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
replace_open_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobOpenForWrite *job = G_VFS_JOB_OPEN_FOR_WRITE (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);

  gint16 fork_refnum;
  GError *err = NULL;
  AfpHandle *afp_handle;
  char *tmp_filename;
  
  if (!g_vfs_afp_volume_open_fork_finish (volume, res, &fork_refnum, NULL, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  afp_handle = afp_handle_new (afp_backend, fork_refnum);
  afp_handle->mode = job->mode;

  g_vfs_job_open_for_write_set_handle (job, (GVfsBackendHandle) afp_handle);
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
  g_vfs_job_open_for_write_set_can_truncate (job, TRUE);
  g_vfs_job_open_for_write_set_initial_offset (job, 0);

  tmp_filename = g_object_get_data (G_OBJECT (job), "TempFilename");
  /* Replace using temporary file */
  if (tmp_filename)
  {
    afp_handle->filename = g_strdup (job->filename);
    afp_handle->tmp_filename = g_strdup (tmp_filename);
    afp_handle->make_backup = job->make_backup;
  }
  else
  {
    g_vfs_afp_volume_set_fork_size (volume,
                                    fork_refnum,
                                    0,
                                    G_VFS_JOB (job)->cancellable,
                                    replace_set_fork_size_cb,
                                    job);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void replace_create_tmp_file (GVfsAfpVolume *volume, GVfsJobOpenForWrite *job);

static void
replace_create_tmp_file_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobOpenForWrite *job = G_VFS_JOB_OPEN_FOR_WRITE (user_data);

  GError *err = NULL;
  char *tmp_filename;

  if (!g_vfs_afp_volume_create_file_finish (volume, res, &err))
  {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_EXISTS))
      replace_create_tmp_file (volume, job);

    /* We don't have the necessary permissions to create a temporary file
     * so we try to write directly to the file */
    else if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
    {
      /* FIXME: We don't support making backups when we can't use FPExchangeFiles */
      if (job->make_backup)
      {
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                                  G_IO_ERROR_CANT_CREATE_BACKUP,
                                  _("Backups not supported"));
      }
      else
      {
        g_object_set_data (G_OBJECT (job), "TempFilename", NULL);
        g_vfs_afp_volume_open_fork (volume, job->filename,
                                    AFP_ACCESS_MODE_WRITE_BIT, 0,
                                    G_VFS_JOB (job)->cancellable, replace_open_fork_cb, job);
      }
    }
                              
    else
    {
      g_vfs_job_failed (G_VFS_JOB (job), err->domain, err->code,
                        _("Unable to create temporary file (%s)"), err->message);
    }
    g_error_free (err);
    return;
  }

  tmp_filename = g_object_get_data (G_OBJECT (job), "TempFilename");
  g_vfs_afp_volume_open_fork (volume, tmp_filename,
                              AFP_ACCESS_MODE_WRITE_BIT, 0,
                              G_VFS_JOB (job)->cancellable, replace_open_fork_cb, job);
}

static void
replace_create_tmp_file (GVfsAfpVolume *volume, GVfsJobOpenForWrite *job)
{
  char basename[] = "~gvfXXXX.tmp";
  char *dir, *tmp_filename;

  gvfs_randomize_string (basename + 4, 4);
  dir = g_path_get_dirname (job->filename);

  tmp_filename = g_build_filename (dir, basename, NULL);
  g_free (dir);

  g_object_set_data_full (G_OBJECT (job), "TempFilename", tmp_filename, g_free);
  g_vfs_afp_volume_create_file (volume, tmp_filename, FALSE,
                                G_VFS_JOB (job)->cancellable,
                                replace_create_tmp_file_cb, job);
}

static void
replace_get_filedir_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobOpenForWrite *job = G_VFS_JOB_OPEN_FOR_WRITE (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);

  GError *err = NULL;
  GFileInfo *info;

  info = g_vfs_afp_volume_get_filedir_parms_finish (volume, res, &err);
  if (!info)
  {
    /* Create file if it doesn't exist */
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
      try_create (G_VFS_BACKEND (afp_backend), job, job->filename, job->flags);

    else
      g_vfs_job_failed_from_error (G_VFS_JOB (job), err);

    g_error_free (err);
    return;
  }

  if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                              _("File is directory"));
  }
  
  else if (job->etag && g_strcmp0 (g_file_info_get_etag (info), job->etag) != 0)
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), 
                              G_IO_ERROR, G_IO_ERROR_WRONG_ETAG,
                              _("The file was externally modified"));
  }
  else
  {
    if (g_vfs_afp_volume_get_attributes (volume) & AFP_VOLUME_ATTRIBUTES_BITMAP_NO_EXCHANGE_FILES)
    {
      /* FIXME: We don't support making backups when we can't use FPExchangeFiles */
      if (job->make_backup)
      {
        g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                                  G_IO_ERROR_CANT_CREATE_BACKUP,
                                  _("Backups not supported"));
      }
      else
      {
        g_vfs_afp_volume_open_fork (volume, job->filename,
                                    AFP_ACCESS_MODE_WRITE_BIT, 0,
                                    G_VFS_JOB (job)->cancellable, replace_open_fork_cb, job);
      }
    }
    else
      replace_create_tmp_file (volume, job);
  }

  g_object_unref (info);
}
  
static gboolean
try_replace (GVfsBackend *backend,
             GVfsJobOpenForWrite *job,
             const char *filename,
             const char *etag,
             gboolean make_backup,
             GFileCreateFlags flags)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  g_vfs_afp_volume_get_filedir_parms (afp_backend->volume, filename, 
                                      AFP_FILE_BITMAP_MOD_DATE_BIT, 0,
                                      G_VFS_JOB (job)->cancellable,
                                      replace_get_filedir_parms_cb, job);
  return TRUE;
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

static void
read_open_fork_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobOpenForRead *job = G_VFS_JOB_OPEN_FOR_READ (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);

  GError *err = NULL;
  gint16 fork_refnum;
  AfpHandle *afp_handle;
  
  if (!g_vfs_afp_volume_open_fork_finish (volume, res, &fork_refnum, NULL, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  afp_handle = afp_handle_new (afp_backend, fork_refnum);
  
  g_vfs_job_open_for_read_set_handle (job, (GVfsBackendHandle) afp_handle);
  g_vfs_job_open_for_read_set_can_seek (job, TRUE);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_open_for_read (GVfsBackend *backend,
                   GVfsJobOpenForRead *job,
                   const char *filename)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  g_vfs_afp_volume_open_fork (afp_backend->volume, filename,
                              AFP_ACCESS_MODE_READ_BIT, 0,
                              G_VFS_JOB (job)->cancellable, read_open_fork_cb, job);
  return TRUE;
}

static guint16
create_filedir_bitmap (GVfsBackendAfp *afp_backend, GFileAttributeMatcher *matcher)
{
  guint16 bitmap;

  bitmap = AFP_FILEDIR_BITMAP_ATTRIBUTE_BIT | AFP_FILEDIR_BITMAP_UTF8_NAME_BIT;

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_TIME_CREATED))
    bitmap |= AFP_FILEDIR_BITMAP_CREATE_DATE_BIT;
  
  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_TIME_MODIFIED) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_ETAG_VALUE))
    bitmap |= AFP_FILEDIR_BITMAP_MOD_DATE_BIT;

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_UNIX_MODE) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_UNIX_UID) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_UNIX_GID) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_ACCESS_CAN_READ) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE)||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_OWNER_USER) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_OWNER_USER_REAL) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_OWNER_GROUP))
      
  {
    if (g_vfs_afp_volume_get_attributes (afp_backend->volume) & AFP_VOLUME_ATTRIBUTES_BITMAP_SUPPORTS_UNIX_PRIVS)
      bitmap |= AFP_FILEDIR_BITMAP_UNIX_PRIVS_BIT;
  }
      
  return bitmap;
}

static guint16
create_file_bitmap (GVfsBackendAfp *afp_backend, GFileAttributeMatcher *matcher)
{
  guint16 file_bitmap;
  
  file_bitmap = create_filedir_bitmap (afp_backend, matcher);

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_STANDARD_SIZE))
    file_bitmap |= AFP_FILE_BITMAP_EXT_DATA_FORK_LEN_BIT;
  
  return file_bitmap;
}

static guint16
create_dir_bitmap (GVfsBackendAfp *afp_backend, GFileAttributeMatcher *matcher)
{
  guint16 dir_bitmap;
  
  dir_bitmap = create_filedir_bitmap (afp_backend, matcher);

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_AFP_CHILDREN_COUNT))
    dir_bitmap |= AFP_DIR_BITMAP_OFFSPRING_COUNT_BIT;
  
  return dir_bitmap;
}

static void
enumerate (GVfsBackendAfp *afp_backend,
           GVfsJobEnumerate *job,
           gint32 start_index);

static void
enumerate_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobEnumerate *job = G_VFS_JOB_ENUMERATE (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);

  GPtrArray *infos;
  GError *err = NULL;

  guint i;
  gint64 start_index;

  
  if (!g_vfs_afp_volume_enumerate_finish (volume, res, &infos, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  /* No more files */
  if (!infos)
  {
    g_vfs_job_succeeded (G_VFS_JOB (job));
    g_vfs_job_enumerate_done (job);
    return;
  }

  for (i = 0; i < infos->len; i++)
    g_vfs_job_enumerate_add_info (job, g_ptr_array_index (infos, i));
  
  start_index = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (job),
                                                    "start-index"));
  start_index += infos->len;
  g_ptr_array_unref (infos);

  enumerate (afp_backend, job, start_index);
}

static void
enumerate (GVfsBackendAfp *afp_backend,
           GVfsJobEnumerate *job,
           gint32 start_index)
{
  const char *filename = job->filename;
  GFileAttributeMatcher *matcher = job->attribute_matcher;
  
  guint16 file_bitmap, dir_bitmap;
  
  g_object_set_data (G_OBJECT (job), "start-index",
                     GINT_TO_POINTER (start_index));

  file_bitmap = create_file_bitmap (afp_backend, matcher);
  dir_bitmap = create_dir_bitmap (afp_backend, matcher);
  
  g_vfs_afp_volume_enumerate (afp_backend->volume, filename, start_index,
                              file_bitmap, dir_bitmap,
                              G_VFS_JOB (job)->cancellable, enumerate_cb, job);
}

static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *matcher,
               GFileQueryInfoFlags flags)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  enumerate (afp_backend, job, 1);
  
  return TRUE;
}

static gboolean
try_query_settable_attributes (GVfsBackend *backend,
                               GVfsJobQueryAttributes *job,
                               const char *filename)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  GFileAttributeInfoList *list;

  list = g_file_attribute_info_list_new ();

  if (g_vfs_afp_volume_get_attributes (afp_backend->volume) & AFP_VOLUME_ATTRIBUTES_BITMAP_SUPPORTS_UNIX_PRIVS)
  {
    g_file_attribute_info_list_add (list,
                                    G_FILE_ATTRIBUTE_UNIX_MODE,
                                    G_FILE_ATTRIBUTE_TYPE_UINT32,
                                    G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                    G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
    g_file_attribute_info_list_add (list,
                                    G_FILE_ATTRIBUTE_UNIX_UID,
                                    G_FILE_ATTRIBUTE_TYPE_UINT32,
                                    G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                    G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
    g_file_attribute_info_list_add (list,
                                    G_FILE_ATTRIBUTE_UNIX_GID,
                                    G_FILE_ATTRIBUTE_TYPE_UINT32,
                                    G_FILE_ATTRIBUTE_INFO_COPY_WITH_FILE |
                                    G_FILE_ATTRIBUTE_INFO_COPY_WHEN_MOVED);
  }
    
  g_vfs_job_query_attributes_set_list (job, list);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_file_attribute_info_list_unref (list);
  
  return TRUE;
}

static void
set_attribute_set_unix_privs_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobSetAttribute *job = G_VFS_JOB_SET_ATTRIBUTE (user_data);

  GError *err = NULL;

  if (!g_vfs_afp_volume_set_unix_privs_finish (volume, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
set_attribute_get_filedir_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobSetAttribute *job = G_VFS_JOB_SET_ATTRIBUTE (user_data);

  GFileInfo *info;
  GError *err = NULL;

  guint32 uid, gid, permissions, ua_permissions;

  info = g_vfs_afp_volume_get_filedir_parms_finish (volume, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  uid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID);
  gid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID);
  permissions = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE);
  ua_permissions = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_AFP_UA_PERMISSIONS);

  g_object_unref (info);

  
  if (strcmp (job->attribute, G_FILE_ATTRIBUTE_UNIX_UID) == 0)
    uid = job->value.uint32;
  else if (strcmp (job->attribute, G_FILE_ATTRIBUTE_UNIX_GID) == 0)
    gid = job->value.uint32;
  else if (strcmp (job->attribute, G_FILE_ATTRIBUTE_UNIX_MODE) == 0)
    permissions = job->value.uint32;

  g_vfs_afp_volume_set_unix_privs (volume, job->filename,
                                   uid, gid, permissions, ua_permissions,
                                   G_VFS_JOB (job)->cancellable,
                                   set_attribute_set_unix_privs_cb, job);
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
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  if ((strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_MODE) == 0 ||
       strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_UID) == 0 ||
       strcmp (attribute, G_FILE_ATTRIBUTE_UNIX_GID) == 0)
      && g_vfs_afp_volume_get_attributes (afp_backend->volume) & AFP_VOLUME_ATTRIBUTES_BITMAP_SUPPORTS_UNIX_PRIVS)
    {
      if (type != G_FILE_ATTRIBUTE_TYPE_UINT32) 
      {
        g_vfs_job_failed (G_VFS_JOB (job),
                          G_IO_ERROR,
                          G_IO_ERROR_INVALID_ARGUMENT,
                          "%s",
                          _("Invalid attribute type (uint32 expected)"));
        return TRUE;
      }

      g_vfs_afp_volume_get_filedir_parms (afp_backend->volume, filename,
                                          AFP_FILEDIR_BITMAP_UNIX_PRIVS_BIT,
                                          AFP_FILEDIR_BITMAP_UNIX_PRIVS_BIT,
                                          G_VFS_JOB (job)->cancellable,
                                          set_attribute_get_filedir_parms_cb,
                                          job);
      return TRUE;
    }

  else {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                      _("Operation not supported"));
    return TRUE;
  }
}


static void
query_fs_info_get_vol_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobQueryFsInfo *job = G_VFS_JOB_QUERY_FS_INFO (user_data);

  GError *err = NULL;
  GFileInfo *info;

  info = g_vfs_afp_volume_get_parms_finish (volume, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  copy_file_info_into (info, job->file_info);
  g_object_unref (info);


  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_query_fs_info (GVfsBackend *backend,
                   GVfsJobQueryFsInfo *job,
                   const char *filename,
                   GFileInfo *info,
                   GFileAttributeMatcher *matcher)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
  
  guint16 vol_bitmap = 0;

  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "afp");
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, TRUE);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_IF_ALWAYS);
  
  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE))
    vol_bitmap |= AFP_VOLUME_BITMAP_EXT_BYTES_TOTAL_BIT;
  
  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_FILESYSTEM_FREE))
    vol_bitmap |= AFP_VOLUME_BITMAP_EXT_BYTES_FREE_BIT;

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_FILESYSTEM_USED))
  {
    vol_bitmap |= AFP_VOLUME_BITMAP_EXT_BYTES_TOTAL_BIT;
    vol_bitmap |= AFP_VOLUME_BITMAP_EXT_BYTES_FREE_BIT;
  }

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY))
    vol_bitmap |= AFP_VOLUME_BITMAP_ATTRIBUTE_BIT;

  if (vol_bitmap != 0)
  {
    g_vfs_afp_volume_get_parms (afp_backend->volume, vol_bitmap,
                                G_VFS_JOB (job)->cancellable,
                                query_fs_info_get_vol_parms_cb, job);
  }
  else
    g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static void
get_name_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpServer *server = G_VFS_AFP_SERVER (source_object);
  GVfsJobQueryInfo *job = G_VFS_JOB_QUERY_INFO (user_data);

  char *name;
  GVfsAfpMapIDFunction map_function;
  guint outstanding_requests;

  name = g_vfs_afp_server_map_id_finish (server, res, &map_function, NULL);
  if (name)
  {
    switch (map_function)
    {
      case GVFS_AFP_MAP_ID_FUNCTION_USER_ID_TO_NAME:
        g_file_info_set_attribute_string (job->file_info, G_FILE_ATTRIBUTE_OWNER_USER,
                                          name);
        break;
      case GVFS_AFP_MAP_ID_FUNCTION_USER_ID_TO_UTF8_NAME:
        g_file_info_set_attribute_string (job->file_info, G_FILE_ATTRIBUTE_OWNER_USER_REAL,
                                          name);
        break;
      case GVFS_AFP_MAP_ID_FUNCTION_GROUP_ID_TO_NAME:
        g_file_info_set_attribute_string (job->file_info, G_FILE_ATTRIBUTE_OWNER_GROUP,
                                          name);
        break;

      default:
        g_assert_not_reached ();
    }

    g_free (name);
  }

  outstanding_requests = GPOINTER_TO_UINT (G_VFS_JOB (job)->backend_data);
  if (--outstanding_requests == 0)
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    G_VFS_JOB (job)->backend_data = GUINT_TO_POINTER (outstanding_requests);
}

static void
set_root_info (GVfsBackendAfp *afp_backend, GFileInfo *info)
{
  GIcon *icon;
  
  g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
  g_file_info_set_name (info, "/");
  g_file_info_set_display_name (info,
                                g_vfs_backend_get_display_name (G_VFS_BACKEND (afp_backend)));
  g_file_info_set_content_type (info, "inode/directory");
  icon = g_vfs_backend_get_icon (G_VFS_BACKEND (afp_backend));
  if (icon != NULL)
    g_file_info_set_icon (info, icon);
  icon = g_vfs_backend_get_symbolic_icon (G_VFS_BACKEND (afp_backend));
  if (icon != NULL)
    g_file_info_set_symbolic_icon (info, icon);
}

static void
query_info_get_filedir_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpVolume *volume = G_VFS_AFP_VOLUME (source_object);
  GVfsJobQueryInfo *job = G_VFS_JOB_QUERY_INFO (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);
  
  GFileInfo *info;
  GError *err = NULL;

  GFileAttributeMatcher *matcher;
  guint outstanding_requests;
  
  info = g_vfs_afp_volume_get_filedir_parms_finish (volume, res, &err);
  if (!info)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  outstanding_requests = 0;
  matcher = job->attribute_matcher;
  
  if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_UID))  
  {
    guint32 uid;

    uid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID);

    if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_OWNER_USER))
    {
      g_vfs_afp_server_map_id (afp_backend->server,
                               GVFS_AFP_MAP_ID_FUNCTION_USER_ID_TO_NAME, uid,
                               G_VFS_JOB (job)->cancellable, get_name_cb, job);
      outstanding_requests++;
    }
    
    if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_OWNER_USER_REAL))
    {
      g_vfs_afp_server_map_id (afp_backend->server,
                               GVFS_AFP_MAP_ID_FUNCTION_USER_ID_TO_UTF8_NAME, uid,
                               G_VFS_JOB (job)->cancellable, get_name_cb, job);
      outstanding_requests++;
    }
  }

  if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_GID) &&
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_OWNER_GROUP))  
  {
    guint32 gid;

    gid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_GID);

    g_vfs_afp_server_map_id (afp_backend->server,
                             GVFS_AFP_MAP_ID_FUNCTION_GROUP_ID_TO_NAME, gid,
                             G_VFS_JOB (job)->cancellable, get_name_cb, job);
    outstanding_requests++;
  }
  
  G_VFS_JOB (job)->backend_data = GUINT_TO_POINTER (outstanding_requests);
  
  copy_file_info_into (info, job->file_info);
  g_object_unref (info);

  if (is_root (job->filename))
    set_root_info (afp_backend, job->file_info);
  
  if (outstanding_requests == 0)
    g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  g_debug ("Filename: %s\n", filename);
  
  if (is_root (filename))
  {
    guint16 dir_bitmap = 0;

    dir_bitmap = create_dir_bitmap (afp_backend, matcher);
    dir_bitmap &= ~AFP_DIR_BITMAP_UTF8_NAME_BIT;
    
    if (dir_bitmap != 0)
    {
      g_vfs_afp_volume_get_filedir_parms (afp_backend->volume, filename,
                                          0, dir_bitmap,
                                          G_VFS_JOB (job)->cancellable,
                                          query_info_get_filedir_parms_cb, job);
    }
    else
    {
      set_root_info (afp_backend, info);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  }
  
  else {
    guint16 file_bitmap, dir_bitmap;
    
    file_bitmap = create_file_bitmap (afp_backend, matcher);
    dir_bitmap = create_dir_bitmap (afp_backend, matcher);

    g_vfs_afp_volume_get_filedir_parms (afp_backend->volume, filename,
                                        file_bitmap, dir_bitmap,
                                        G_VFS_JOB (job)->cancellable,
                                        query_info_get_filedir_parms_cb, job);
  }

  return TRUE;
}

static void
do_unmount (GVfsBackend *backend,
            GVfsJobUnmount *job,
            GMountUnmountFlags flags,
            GMountSource *mount_source)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  if (!(flags & G_MOUNT_UNMOUNT_FORCE))
  {
    g_vfs_afp_server_logout_sync (afp_backend->server, G_VFS_JOB (job)->cancellable,
                                  NULL);
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  gboolean res;
  GError *err = NULL;

  const GVfsAfpServerInfo *info;
  GMountSpec *afp_mount_spec;
  char       *server_name;
  char       *display_name;

  afp_backend->server = g_vfs_afp_server_new (afp_backend->addr);

  res = g_vfs_afp_server_login (afp_backend->server, afp_backend->user, mount_source,
                                NULL, G_VFS_JOB (job)->cancellable, &err);
  if (!res)
    goto error;

  afp_backend->volume =
    g_vfs_afp_server_mount_volume_sync (afp_backend->server, afp_backend->volume_name,
                                        G_VFS_JOB (job)->cancellable, &err);
  if (!afp_backend->volume)
    goto error;
  
  /* set mount info */
  afp_mount_spec = g_mount_spec_new ("afp-volume");
  g_mount_spec_set (afp_mount_spec, "host",
                    g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)));
  g_mount_spec_set (afp_mount_spec, "volume", afp_backend->volume_name);
  if (afp_backend->user)
    g_mount_spec_set (afp_mount_spec, "user", afp_backend->user);

  g_vfs_backend_set_mount_spec (backend, afp_mount_spec);
  g_mount_spec_unref (afp_mount_spec);

  info = g_vfs_afp_server_get_info (afp_backend->server);
  
  if (info->utf8_server_name)
    server_name = info->utf8_server_name;
  else
    server_name = info->server_name;
  
  if (afp_backend->user)
    /* Translators: first %s is volumename, second username and third servername */ 
    display_name = g_strdup_printf (_("%s for %s on %s"),
                                    afp_backend->volume_name, afp_backend->user,
                                    server_name);
  else
    /* Translators: first %s is volumename and second servername */
    display_name = g_strdup_printf (_("%s on %s"),
                                    afp_backend->volume_name, server_name);
  
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);

  g_vfs_backend_set_icon_name (backend, "folder-remote-afp");
  g_vfs_backend_set_symbolic_icon_name (backend, "folder-remote-symbolic");
  g_vfs_backend_set_user_visible (backend, TRUE);

  g_vfs_job_succeeded (G_VFS_JOB (job));
  return;

error:
  g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
  return;
}
  
static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  const char *host, *volume, *portstr, *user;
  guint16 port = 548;

  host = g_mount_spec_get (mount_spec, "host");
  if (host == NULL)
  {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                      _("No hostname specified"));
    return TRUE;
  }

  volume = g_mount_spec_get (mount_spec, "volume");
  if (volume == NULL)
  {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                      _("No volume specified"));
    return TRUE;
  }
  afp_backend->volume_name = g_strdup (volume);

  portstr = g_mount_spec_get (mount_spec, "port");
  if (portstr != NULL)
  {
    port = atoi (portstr);
  }

  afp_backend->addr = G_NETWORK_ADDRESS (g_network_address_new (host, port));

  user = g_mount_spec_get (mount_spec, "user");
  afp_backend->user = g_strdup (user);

  return FALSE;
}

static void
g_vfs_backend_afp_init (GVfsBackendAfp *object)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (object);
  
  afp_backend->volume_name = NULL;
  afp_backend->user = NULL;

  afp_backend->addr = NULL;
}

static void
g_vfs_backend_afp_finalize (GObject *object)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (object);

  g_free (afp_backend->user);

  if (afp_backend->volume_name)
    g_free (afp_backend->volume_name);

  if (afp_backend->volume)
    g_object_unref (afp_backend->volume);
    
  if (afp_backend->addr)
    g_object_unref (afp_backend->addr);

  G_OBJECT_CLASS (g_vfs_backend_afp_parent_class)->finalize (object);
}

static void
g_vfs_backend_afp_class_init (GVfsBackendAfpClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

  object_class->finalize = g_vfs_backend_afp_finalize;

  backend_class->try_mount = try_mount;
  backend_class->mount = do_mount;
  backend_class->unmount = do_unmount;
  backend_class->try_query_info = try_query_info;
  backend_class->try_query_fs_info = try_query_fs_info;
  backend_class->try_set_attribute = try_set_attribute;
  backend_class->try_query_settable_attributes = try_query_settable_attributes;
  backend_class->try_enumerate = try_enumerate;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_close_read = try_close_read;
  backend_class->try_read = try_read;
  backend_class->try_seek_on_read = try_seek_on_read;
  backend_class->try_append_to = try_append_to;
  backend_class->try_edit = try_edit;
  backend_class->try_create = try_create;
  backend_class->try_replace = try_replace;
  backend_class->try_write = try_write;
  backend_class->try_seek_on_write = try_seek_on_write;
  backend_class->try_truncate = try_truncate;
  backend_class->try_close_write = try_close_write;
  backend_class->try_delete = try_delete;
  backend_class->try_make_directory = try_make_directory;
  backend_class->try_set_display_name = try_set_display_name;
  backend_class->try_move = try_move;
  backend_class->try_copy = try_copy;
}

void
g_vfs_afp_daemon_init (void)
{
  g_set_application_name (_("Apple Filing Protocol Service"));

#ifdef HAVE_GCRYPT
  gcry_check_version (NULL);
  gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
#endif
}
