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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gunixmounts.h>

#include "gvfsbackendburn.h"
#include "gvfsmonitor.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobmountmountable.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobdelete.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsdaemonprotocol.h"

/* TODO:
 * Change notification
 * 
 */

typedef enum {
	VIRTUAL_NODE_FILE,
	VIRTUAL_NODE_DIRECTORY
} VirtualNodeType;

typedef struct {
	char           *filename;
	VirtualNodeType type;

	/* for files: */
	char           *backing_file; /* local filename */
	gboolean        owned_file;

	GList          *subscriptions;

	/* for directories: */
	GList          *children;
	volatile gint   ref_count;
} VirtualNode;

struct _GVfsBackendBurn
{
  GVfsBackend parent_instance;

  char        *tempdir;
  
  VirtualNode *root_node;
  
  GMountSpec *mount_spec;
};

G_DEFINE_TYPE (GVfsBackendBurn, g_vfs_backend_burn, G_VFS_TYPE_BACKEND)


static void virtual_node_unref (VirtualNode *node);

static VirtualNode *
virtual_node_new (const char     *filename,
                  VirtualNodeType type)
{
  VirtualNode *node;
  
  node = g_slice_new0 (VirtualNode);
  node->filename = g_strdup (filename);
  node->type = type;
  node->ref_count = 1;
  
  return node;
}

static void
virtual_node_free (VirtualNode *node,
                   gboolean     deep)
{
  GList *l;
  
  g_free (node->filename);
  
  switch (node->type)
    {
    case VIRTUAL_NODE_FILE:
      if (node->backing_file != NULL)
        {
          if (node->owned_file)
            g_unlink (node->backing_file);
          g_free (node->backing_file);
        }
      break;
    case VIRTUAL_NODE_DIRECTORY:
      if (deep)
        {
          for (l = node->children; l != NULL; l = l->next)
            virtual_node_unref ((VirtualNode *)l->data);
        }
      break;
    default:
      g_assert_not_reached ();
      break;
    }

  /* 
  while ((l = g_list_first (node->subscriptions)) != NULL) {
    MappingSubscription *sub = l->data;
    virtual_node_remove_subscription (node, sub);
  }
  */
  
  g_slice_free (VirtualNode, node);
}

static void
virtual_node_unref (VirtualNode *node)
{
  g_return_if_fail (node != NULL);
  g_return_if_fail (node->ref_count > 0);
  
  if (g_atomic_int_dec_and_test (&node->ref_count))
    virtual_node_free (node, TRUE);
}

static G_GNUC_UNUSED VirtualNode *
virtual_node_ref (VirtualNode *node)
{
  g_return_val_if_fail (node != NULL, NULL);
  g_return_val_if_fail (node->ref_count > 0, NULL);
  
  g_atomic_int_inc (&node->ref_count);
  return node;
}


static VirtualNode *
virtual_dir_lookup (VirtualNode *dir,
                    const char  *filename)
{
  GList       *l;
  VirtualNode *node;
  
  g_assert (dir->type == VIRTUAL_NODE_DIRECTORY);
  
  for (l = dir->children; l != NULL; l = l->next)
    {
      node = l->data;
      
      if (strcmp (node->filename, filename) == 0)
        return node;
    }
  
  return NULL;
}

static VirtualNode *
virtual_node_lookup (VirtualNode  *root_dir,
                     const char   *path,
                     VirtualNode **parent)
{
  char        *copy, *next, *copy_orig;
  VirtualNode *node;
  
  copy_orig = g_strdup (path);
  copy = copy_orig;
  
  if (parent != NULL) 
    *parent = NULL;
  
  node = root_dir;
  
  while (copy != NULL)
    {
      /* Skip initial/multiple slashes */
      while (G_IS_DIR_SEPARATOR (*copy))
        ++copy;
      
      if (*copy == 0)
        break;
      
      next = strchr (copy, G_DIR_SEPARATOR);
      if (next)
        {
          *next = 0;
          next++;
        }
      
      if (node->type != VIRTUAL_NODE_DIRECTORY)
        {
          /* Found a file in the middle of the path */
          node = NULL;
          break;
        }
      
      if (parent != NULL) 
        *parent = node;
      
      node = virtual_dir_lookup (node, copy);
      if (node == NULL) 
        break;
      
      copy = next;
    }
  
  g_free (copy_orig);
  
  return node;
}

static VirtualNode *
virtual_mkdir (VirtualNode *node,
               const char  *name)
{
  VirtualNode *subdir;
  
  g_assert (node->type == VIRTUAL_NODE_DIRECTORY);
  
  if (virtual_dir_lookup (node, name) != NULL)
    return NULL;
  
  subdir = virtual_node_new (name, VIRTUAL_NODE_DIRECTORY);
  
  /* list takes ownership of ref */
  node->children = g_list_append (node->children, subdir);
  
  return subdir;
}

static void
virtual_unlink (VirtualNode *dir,
                VirtualNode *node)
{
  g_assert (dir->type == VIRTUAL_NODE_DIRECTORY);
  
  dir->children = g_list_remove (dir->children, node);
  virtual_node_unref (node);
}

static VirtualNode *
virtual_create (GVfsBackendBurn *backend,
                VirtualNode *dir,
                const char  *name,
                const char  *backing_file)
{
  VirtualNode *file;
  char        *template;
  int          fd;
  
  g_assert (dir->type == VIRTUAL_NODE_DIRECTORY);
  
  if (virtual_dir_lookup (dir, name) != NULL)
    return NULL;
  
  file = virtual_node_new (name, VIRTUAL_NODE_FILE);
  
  if (backing_file != NULL)
    {
      file->backing_file = g_strdup (backing_file);
      file->owned_file = FALSE;
    }
  else
    {
      template = g_build_filename (backend->tempdir, "file.XXXXXX", NULL);
      
      fd = g_mkstemp (template);
      if (fd < 0)
        {
          g_free (template);
          virtual_node_unref (file);
          return NULL;
          
        }
      close (fd);
      g_unlink (template);
      
      file->backing_file = template;
      file->owned_file = TRUE;
    }
  
  /* list takes ownership of ref */
  dir->children = g_list_append (dir->children, file);
  
  return file;
}

static void
g_vfs_backend_burn_finalize (GObject *object)
{
  GVfsBackendBurn *backend;

  backend = G_VFS_BACKEND_BURN (object);

  g_mount_spec_unref (backend->mount_spec);

  if (G_OBJECT_CLASS (g_vfs_backend_burn_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_burn_parent_class)->finalize) (object);
}

static void
g_vfs_backend_burn_init (GVfsBackendBurn *burn_backend)
{
  GVfsBackend *backend = G_VFS_BACKEND (burn_backend);
  GMountSpec *mount_spec;
  
  /* Translators: This is the name of the backend */
  g_vfs_backend_set_display_name (backend, _("Burn"));
  g_vfs_backend_set_icon_name (backend, "computer");
  g_vfs_backend_set_symbolic_icon_name (backend, "computer-symbolic");
  g_vfs_backend_set_user_visible (backend, FALSE);

  mount_spec = g_mount_spec_new ("burn");
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  burn_backend->mount_spec = mount_spec;
}

static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendBurn *burn_backend = G_VFS_BACKEND_BURN (backend);
  char        *filename;

  filename = g_build_filename (g_get_user_runtime_dir (), "gvfs-burn", NULL);
  if (g_mkdir_with_parents (filename, 0700) < 0)
    {
      g_free (filename);
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Unable to create temporary directory"));
      return TRUE;
    }

  burn_backend->tempdir = filename;
  burn_backend->root_node =
    virtual_node_new (NULL, VIRTUAL_NODE_DIRECTORY);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
}

static gboolean
try_delete (GVfsBackend *backend,
            GVfsJobDelete *job,
            const char *filename)
{
  char *dirname, *basename;
  VirtualNode *file, *dir;

  dirname = g_path_get_dirname (filename);
  dir = virtual_node_lookup (G_VFS_BACKEND_BURN (backend)->root_node, dirname, NULL);
  g_free (dirname);

  if (dir == NULL ||
      dir->type != VIRTUAL_NODE_DIRECTORY)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("No such file or directory"));
      return TRUE;
    }

  basename = g_path_get_basename (filename);
  file = virtual_dir_lookup (dir, basename);
  g_free (basename);
  if (file == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("No such file or directory"));
      return TRUE;
    }
  
  if (file->type == VIRTUAL_NODE_DIRECTORY &&
      file->children != NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_EMPTY,
                        _("Directory not empty"));
      return TRUE;
    }
  
  virtual_unlink (dir, file);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
}


static gboolean
try_open_for_read (GVfsBackend *backend,
                   GVfsJobOpenForRead *job,
                   const char *filename)
{
  VirtualNode *node;
  GFileInputStream *stream;
  GFile *file;
  GError *error;

  node = virtual_node_lookup (G_VFS_BACKEND_BURN (backend)->root_node, filename, NULL);
  if (node == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("No such file or directory"));
      return TRUE;
    }

  if (node->type == VIRTUAL_NODE_DIRECTORY)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                        _("Can’t copy file over directory"));
      return TRUE;
    }

  file = g_file_new_for_path (node->backing_file);
  
  error = NULL;
  stream = g_file_read (file, G_VFS_JOB (job)->cancellable, &error);
  g_object_unref (file);
  
  if (stream)
    {
      g_vfs_job_open_for_read_set_can_seek (job, g_seekable_can_seek (G_SEEKABLE (stream)));
      g_vfs_job_open_for_read_set_handle (job, stream);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }

  return TRUE;
}


static void
do_read (GVfsBackend *backend,
         GVfsJobRead *job,
         GVfsBackendHandle _handle,
         char *buffer,
         gsize bytes_requested)
{
  GError *error;
  GFileInputStream *stream = _handle;
  gssize s;
  
  error = NULL;
  s = g_input_stream_read (G_INPUT_STREAM(stream),
                           buffer, bytes_requested,
                           G_VFS_JOB (job)->cancellable, &error); 
  if (s >= 0)
    {
      g_vfs_job_read_set_size (job, s);
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error); 
      g_error_free (error);
    }
}

static void
do_seek_on_read (GVfsBackend *backend,
                  GVfsJobSeekRead *job,
                  GVfsBackendHandle _handle,
                  goffset    offset,
                  GSeekType  type)
{
  GError *error;
  GFileInputStream *stream = _handle;
  
  error = NULL;
  if (g_seekable_seek (G_SEEKABLE (stream), offset, type,
                       G_VFS_JOB (job)->cancellable, &error))
    {
      g_vfs_job_seek_read_set_offset (job, g_seekable_tell (G_SEEKABLE (stream)));
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}


static void
do_close_read (GVfsBackend *backend,
               GVfsJobCloseRead *job,
               GVfsBackendHandle _handle)
{
  GError *error;
  GFileInputStream *stream = _handle;
  
  error = NULL;
  if (g_input_stream_close (G_INPUT_STREAM(stream),
                            G_VFS_JOB (job)->cancellable, &error))
    {
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
  
  g_object_unref (stream);
}

static char *
make_valid_utf8 (const char *name)
{
  GString *string;
  const gchar *remainder, *invalid;
  gint remaining_bytes, valid_bytes;
  
  string = NULL;
  remainder = name;
  remaining_bytes = strlen (name);
  
  while (remaining_bytes != 0) 
    {
      if (g_utf8_validate (remainder, remaining_bytes, &invalid)) 
        break;
      valid_bytes = invalid - remainder;
      
      if (string == NULL) 
        string = g_string_sized_new (remaining_bytes);
      
      g_string_append_len (string, remainder, valid_bytes);
      /* append U+FFFD REPLACEMENT CHARACTER */
      g_string_append (string, "\357\277\275");
      
      remaining_bytes -= valid_bytes + 1;
      remainder = invalid + 1;
    }
  
  if (string == NULL)
    return g_strdup (name);
  
  g_string_append (string, remainder);
  
  g_warn_if_fail (g_utf8_validate (string->str, -1, NULL));
  
  return g_string_free (string, FALSE);
}


static void
file_info_from_node (VirtualNode *node,
                     GFileInfo *info,
                     const char *attributes)
{
  GIcon *icon;
  GFile *file;
  GFileInfo *file_info;

  if (node->type == VIRTUAL_NODE_DIRECTORY)
    {
      const char *content_type = "inode/directory";

      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      icon = g_content_type_get_icon (content_type);
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
      icon = g_content_type_get_symbolic_icon (content_type);
      g_file_info_set_symbolic_icon (info, icon);
      g_object_unref (icon);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, TRUE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, TRUE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
      g_file_info_set_content_type (info, content_type);
   }
  else
    {
      file = g_file_new_for_path (node->backing_file);
      file_info = g_file_query_info (file,
                                     attributes,
                                     0, /* Always follow symlinks */
                                     NULL, NULL);
      if (file_info)
        {
          g_file_info_copy_into (file_info, info);
          g_object_unref (file_info);
        }
      
      g_file_info_set_attribute_byte_string (info,
                                             "burn::backing-file",
                                             node->backing_file);
    }

  if (node->filename != NULL)
    {
      char *utf8;
      
      g_file_info_set_name (info, node->filename);
      /* Ensure display name is utf8 */
      utf8 = make_valid_utf8 (node->filename);
      g_file_info_set_display_name (info, utf8);
      g_free (utf8);
    }
  else
    {
      g_file_info_set_name (info, "/");
      /* Translators: this is the display name of the backend */
      g_file_info_set_display_name (info, _("CD/DVD Creator"));
    }
}

static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  VirtualNode *node, *child;
  GFileInfo *info;
  GList *l;
  
  node = virtual_node_lookup (G_VFS_BACKEND_BURN (backend)->root_node, filename, NULL);
 
  if (node == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("No such file or directory"));
      return TRUE;
    }
  
  if (node->type != VIRTUAL_NODE_DIRECTORY)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_DIRECTORY,
                        _("The file is not a directory"));
      return TRUE;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  for (l = node->children; l != NULL; l = l->next)
    {
      child = l->data;
      
      info = g_file_info_new ();
      file_info_from_node (child, info, job->attributes);
      g_vfs_job_enumerate_add_info (job, info);
      g_object_unref (info);
    }

  g_vfs_job_enumerate_done (job);
 
  return TRUE;
}

static gboolean
try_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  VirtualNode *node;

  node = virtual_node_lookup (G_VFS_BACKEND_BURN (backend)->root_node, filename, NULL);
 
  if (node == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("No such file or directory"));
      return TRUE;
    }

  file_info_from_node (node, info, job->attributes);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  return TRUE;
}

static gboolean
try_make_directory (GVfsBackend *backend,
                    GVfsJobMakeDirectory *job,
                    const char *filename)
{
  char *dirname, *basename;
  VirtualNode *file, *dir;
  
  dirname = g_path_get_dirname (filename);
  dir = virtual_node_lookup (G_VFS_BACKEND_BURN (backend)->root_node, dirname, NULL);
  g_free (dirname);
  
  if (dir == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("No such file or directory"));
      return TRUE;
    }

  basename = g_path_get_basename (filename);
  file = virtual_dir_lookup (dir, basename);
  if (file != NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_EXISTS,
                        _("File exists"));
      g_free (basename);
      return TRUE;
    }

  file = virtual_mkdir (dir, basename);
  g_free (basename);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static gboolean
try_set_display_name (GVfsBackend *backend,
                      GVfsJobSetDisplayName *job,
                      const char *filename,
                      const char *display_name)
{
  VirtualNode *node, *dir;
  char *target_path;
  char *dirname;
  
  node = virtual_node_lookup (G_VFS_BACKEND_BURN (backend)->root_node, filename, &dir);
  if (node == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("No such file or directory"));
      return TRUE;
    }

  if (virtual_dir_lookup (dir, display_name) != NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_EXISTS,
                        _("File exists"));
      return TRUE;
    }

  /* We use UTF8 for filenames */
  g_free (node->filename);
  node->filename = g_strdup (display_name);

  dirname = g_path_get_dirname (filename);
  target_path = g_build_filename (dirname, display_name, NULL);
  g_vfs_job_set_display_name_set_new_path (job, target_path);
  g_free (dirname);
  g_free (target_path);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  return TRUE;
  
}

static gboolean
try_push (GVfsBackend *backend,
          GVfsJobPush *job,
          const char *destination,
          const char *local_path,
          GFileCopyFlags flags,
          gboolean remove_source,
          GFileProgressCallback progress_callback,
          gpointer progress_callback_data)
{
  VirtualNode *file, *dir;
  struct stat stat_buf;
  char *dirname, *basename;

  if (remove_source)
    {
      /* Fallback to copy & delete for now, fix that up later */
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
				G_IO_ERROR_NOT_SUPPORTED,
                                _("Operation not supported"));
      return TRUE;
    }

  if (g_stat (local_path, &stat_buf) == -1)
    {
      int errsv = errno;

      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        g_io_error_from_errno (errsv),
                        "%s", g_strerror (errsv));
      return TRUE;
    }

  dirname = g_path_get_dirname (destination);
  dir = virtual_node_lookup (G_VFS_BACKEND_BURN (backend)->root_node, dirname, NULL);
  g_free (dirname);
  file = NULL;

  if (dir == NULL)
    {
      /* Parent of created file doesn't exist */
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("No such file or directory"));
      return TRUE;
    }
  
  basename = g_path_get_basename (destination);
  file = virtual_dir_lookup (dir, basename);
  g_free (basename);
  
  if (S_ISDIR (stat_buf.st_mode))
    {
      /* The source is a directory, don't fail with WOULD_RECURSE immediately, 
       * as that is less useful to the app. Better check for errors on the 
       * target instead. 
       */
      
      if (file != NULL)
        {
          if (flags & G_FILE_COPY_OVERWRITE)
            {
              if (file->type == VIRTUAL_NODE_DIRECTORY)
                {
                  g_vfs_job_failed (G_VFS_JOB (job),
                                    G_IO_ERROR, G_IO_ERROR_WOULD_MERGE,
                                    _("Can’t copy directory over directory"));
                  return TRUE;
                }
              /* continue to would_recurse error */
            }
          else
            {
              g_vfs_job_failed (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_EXISTS,
                                _("Target file exists"));
              return TRUE;
            }
        }
      
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
                        _("Can’t recursively copy directory"));
      return TRUE;
    }

  if (file != NULL)
    {
      if (flags & G_FILE_COPY_OVERWRITE)
        {
          if (file->type == VIRTUAL_NODE_DIRECTORY)
            {
              g_vfs_job_failed (G_VFS_JOB (job),
                                G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                                _("Can’t copy file over directory"));
              return TRUE;
            }
          else
            {
              g_assert (file->type == VIRTUAL_NODE_FILE);
              if (file->owned_file)
                g_unlink (file->backing_file);
              g_free (file->backing_file);
              file->owned_file = FALSE;
              file->backing_file = g_strdup (local_path);
              
              g_vfs_job_succeeded (G_VFS_JOB (job));
              return TRUE;
            }
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR, G_IO_ERROR_EXISTS,
                            _("File exists"));
          return TRUE;
        }
    }
  else
    {
      basename = g_path_get_basename (destination);
      file = virtual_create (G_VFS_BACKEND_BURN (backend),
                             dir,
                             basename,
                             local_path);
      g_free (basename);
      
      g_vfs_job_succeeded (G_VFS_JOB (job));
      return TRUE;
    }
}

static gboolean
try_create_dir_monitor (GVfsBackend *backend,
                        GVfsJobCreateMonitor *job,
                        const char *filename,
                        GFileMonitorFlags flags)
{
  g_vfs_job_failed (G_VFS_JOB (job),
                    G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    _("Operation not supported"));
  return TRUE;
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
  VirtualNode *source_node, *dest_node, *root_node, *source_dir, *dest_dir;

  root_node = G_VFS_BACKEND_BURN (backend)->root_node;
  
  source_node = virtual_node_lookup (root_node, source, &source_dir);
  if (source_node == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("No such file or directory"));
      return TRUE;
    }

  dest_node = virtual_node_lookup (root_node, destination, &dest_dir);
  if (dest_node != NULL)
    {
      if (flags & G_FILE_COPY_OVERWRITE)
	{
	  if (dest_node->type == VIRTUAL_NODE_DIRECTORY)
	    {
	      if (source_node->type == VIRTUAL_NODE_DIRECTORY)
		g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
				  G_IO_ERROR_WOULD_MERGE,
				  _("File exists"));
	      else
		g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
				  G_IO_ERROR_IS_DIRECTORY,
				  _("File exists"));
	      return TRUE;
	    }
	  else
	    virtual_unlink (dest_dir, dest_node);
	}
      else
	{
	  g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			    G_IO_ERROR_EXISTS,
			    _("File exists"));
	  return TRUE;
	}
    }
  else if (dest_dir == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("No such file or directory"));
      return TRUE;
    }
  
  g_free (source_node->filename);
  source_node->filename = g_path_get_basename (destination);
  
  if (source_dir != dest_dir)
    {
      source_dir->children = g_list_remove (source_dir->children, source_node);
      dest_dir->children = g_list_append (dest_dir->children, source_node);
    }

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
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "burn");
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, FALSE);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_IF_LOCAL);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
}

static void
g_vfs_backend_burn_class_init (GVfsBackendBurnClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_burn_finalize;

  backend_class->try_mount = try_mount;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_query_info = try_query_info;
  backend_class->try_query_fs_info = try_query_fs_info;
  backend_class->try_enumerate = try_enumerate;
  backend_class->try_create_dir_monitor = try_create_dir_monitor;
  backend_class->try_make_directory = try_make_directory;
  backend_class->try_set_display_name = try_set_display_name;
  backend_class->try_push = try_push;
  backend_class->try_delete = try_delete;
  backend_class->try_move = try_move;
  backend_class->read = do_read;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->close_read = do_close_read;
}
