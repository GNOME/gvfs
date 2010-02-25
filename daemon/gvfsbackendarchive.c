/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2008 Benjamin Otte <otte@gnome.org>
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
 * Author: Benjmain Otte <otte@gnome.org>
 */


#include <config.h>

#include <glib/gi18n.h>
#include <string.h>
#include <archive.h>
#include <archive_entry.h>

#include "gvfsbackendarchive.h"
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
#include "gvfskeyring.h"

#define MOUNT_ICON_NAME "drive-removable-media"

/* #define PRINT_DEBUG  */

#ifdef PRINT_DEBUG
#define DEBUG g_print
#else
#define DEBUG(...)
#endif

/*** TYPE DEFINITIONS ***/

typedef struct _ArchiveFile ArchiveFile;
struct _ArchiveFile {
  char *	name;			/* name of the file inside the archive */
  GFileInfo *	info;			/* file info created from archive_entry */
  GSList *	children;		/* (unordered) list of child files */
};

struct _GVfsBackendArchive
{
  GVfsBackend		backend;

  GFile *		file;
  ArchiveFile *		files;		/* the tree of files */
};

G_DEFINE_TYPE (GVfsBackendArchive, g_vfs_backend_archive, G_VFS_TYPE_BACKEND)

/*** AN ARCHIVE WE CAN OPERATE ON ***/

typedef struct {
  struct archive *  archive;
  GFile *	    file;
  GFileInputStream *stream;
  GVfsJob *	    job;
  GError *	    error;
  guchar	    data[4096];
} GVfsArchive;

#define gvfs_archive_return(d) ((d)->error ? ARCHIVE_FATAL : ARCHIVE_OK)

static int
gvfs_archive_open (struct archive *archive, 
                   void           *data)
{
  GVfsArchive *d = data;

  DEBUG ("OPEN\n");
  g_assert (d->stream == NULL);
  d->stream = g_file_read (d->file,
			   d->job->cancellable,
			   &d->error);
  return gvfs_archive_return (d);
}

static ssize_t
gvfs_archive_read (struct archive *archive, 
		   void           *data,
		   const void    **buffer)
{
  GVfsArchive *d = data;
  gssize read_bytes;

  *buffer = d->data;
  read_bytes = g_input_stream_read (G_INPUT_STREAM (d->stream),
				    d->data,
				    sizeof (d->data),
				    d->job->cancellable,
				    &d->error);

  DEBUG ("READ %d\n", (int) read_bytes);
  return read_bytes;
}

static off_t
gvfs_archive_skip (struct archive *archive,
		   void           *data,
		   off_t	    request)
{
  GVfsArchive *d = data;

  if (g_seekable_can_seek (G_SEEKABLE (d->stream)))
    g_seekable_seek (G_SEEKABLE (d->stream),
		     request,
		     G_SEEK_CUR,
		     d->job->cancellable,
		     &d->error);
  else
    return 0;

  if (d->error)
    {
      g_clear_error (&d->error);
      request = 0;
    }
  DEBUG ("SEEK %d (%d)\n", (int) request,
      (int) g_seekable_tell (G_SEEKABLE (d->stream)));

  return request;
}

static int
gvfs_archive_close (struct archive *archive,
	      void *data)
{
  GVfsArchive *d = data;

  DEBUG ("CLOSE\n");
  g_object_unref (d->stream);
  d->stream = NULL;
  return ARCHIVE_OK;
}

#define gvfs_archive_in_error(archive) ((archive)->error != NULL)

static void
gvfs_archive_set_error_from_errno (GVfsArchive *archive)
{
  if (gvfs_archive_in_error (archive))
    return;

  g_vfs_job_failed_literal (archive->job,
                            G_IO_ERROR,
                            g_io_error_from_errno (archive_errno (archive->archive)),
                            archive_error_string (archive->archive));
}

static void 
gvfs_archive_push_job (GVfsArchive *archive, GVfsJob *job)
{
  archive->job = job;
}

static void 
gvfs_archive_pop_job (GVfsArchive *archive)
{
  if (archive->job == NULL)
    return;

  DEBUG ("popping job %s\n", G_OBJECT_TYPE_NAME (archive->job));
  if (archive->error)
    {
      g_vfs_job_failed_from_error (archive->job, archive->error);
      g_clear_error (&archive->error);
    }
  else
    g_vfs_job_succeeded (archive->job);


  archive->job = NULL;
}

static void
gvfs_archive_finish (GVfsArchive *archive)
{
  gvfs_archive_pop_job (archive);

  archive_read_finish (archive->archive);
  g_slice_free (GVfsArchive, archive);
}

/* NB: assumes an GVfsArchive initialized with ARCHIVE_DATA_INIT */
static GVfsArchive *
gvfs_archive_new (GVfsBackendArchive *ba, GVfsJob *job)
{
  GVfsArchive *d;
  
  d = g_slice_new0 (GVfsArchive);

  d->file = ba->file;
  gvfs_archive_push_job (d, job);

  d->archive = archive_read_new ();
  archive_read_support_compression_all (d->archive);
  archive_read_support_format_all (d->archive);
  archive_read_open2 (d->archive,
		      d,
		      gvfs_archive_open,
		      gvfs_archive_read,
		      gvfs_archive_skip,
		      gvfs_archive_close);

  return d;
}

/*** BACKEND ***/

static void
g_vfs_backend_archive_finalize (GObject *object)
{
  GVfsBackendArchive *archive = G_VFS_BACKEND_ARCHIVE (object);

  g_assert (archive->file == NULL);

  if (G_OBJECT_CLASS (g_vfs_backend_archive_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_archive_parent_class)->finalize) (object);
}

static void
g_vfs_backend_archive_init (GVfsBackendArchive *archive)
{
}

/*** FILE TREE HANDLING ***/

/* NB: filename must NOT start with a slash */
static ArchiveFile *
archive_file_get_from_path (ArchiveFile *file, const char *filename, gboolean add)
{
  char **names = g_strsplit (filename, "/", -1);
  ArchiveFile *cur;
  GSList *walk;
  guint i;

  DEBUG ("%s %s\n", add ? "add" : "find", filename);
  for (i = 0; file && names[i] != NULL; i++)
    {
      cur = NULL;
      for (walk = file->children; walk; walk = walk->next)
	{
	  cur = walk->data;
	  if (g_str_equal (cur->name, names[i]))
	    break;
	  cur = NULL;
	}
      if (cur == NULL && add != FALSE)
	{
	  DEBUG ("adding node %s to %s\n", names[i], file->name);
	  /* (hopefully) clever trick to avoid string copy */
	  if (names[i][0] != 0 &&
              strcmp (names[i], ".") != 0)
	    {
	      cur = g_slice_new0 (ArchiveFile);
	      cur->name = names[i];
	      names[i] = NULL;
	      file->children = g_slist_prepend (file->children, cur);
	    }
	  else
	    {
	      /* happens when adding directories, their path ends with a / */
              /* Can also happen with "." in e.g. iso files */
	      g_assert (names[i + 1] == NULL);
	      g_free (names[i]);
	      names[i] = NULL;
	      cur = file;
	    }
	}
      file = cur;
    }
  g_strfreev (names);
  return file;
}
#define archive_file_find(ba, filename) archive_file_get_from_path((ba)->files, (filename) + 1, FALSE)

static void
create_root_file (GVfsBackendArchive *ba)
{
  ArchiveFile *root;
  GFileInfo *info;
  char *s, *display_name;
  GIcon *icon;

  root = g_slice_new0 (ArchiveFile);
  root->name = g_strdup ("/");
  ba->files = root;

  info = g_file_info_new ();
  root->info = info;

  g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);

  g_file_info_set_name (info, "/");
  s = g_file_get_basename (ba->file);
  /* FIXME: this should really be "/ in %s", but can't change
     due to string freeze. */
  display_name = g_strdup_printf (_("/ on %s"), s);
  g_free (s);
  g_file_info_set_display_name (info, display_name);
  g_free (display_name);
  g_file_info_set_edit_name (info, "/");

  g_file_info_set_content_type (info, "inode/directory");
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, "inode/directory");

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);

  icon = g_themed_icon_new ("folder");
  g_file_info_set_icon (info, icon);
  g_object_unref (icon);
}

static void
archive_file_set_info_from_entry (ArchiveFile *	        file, 
				  struct archive_entry *entry)
{
  GFileInfo *info = g_file_info_new ();
  GFileType type;
  file->info = info;

  DEBUG ("setting up %s (%s)\n", archive_entry_pathname (entry), file->name);

  g_file_info_set_attribute_uint64 (info,
				    G_FILE_ATTRIBUTE_TIME_ACCESS,
				    archive_entry_atime (entry));
  g_file_info_set_attribute_uint32 (info,
				    G_FILE_ATTRIBUTE_TIME_ACCESS_USEC,
				    archive_entry_atime_nsec (entry) / 1000);
  g_file_info_set_attribute_uint64 (info,
				    G_FILE_ATTRIBUTE_TIME_CHANGED,
				    archive_entry_ctime (entry));
  g_file_info_set_attribute_uint32 (info,
				    G_FILE_ATTRIBUTE_TIME_CHANGED_USEC,
				    archive_entry_ctime_nsec (entry) / 1000);
  g_file_info_set_attribute_uint64 (info,
				    G_FILE_ATTRIBUTE_TIME_MODIFIED,
				    archive_entry_mtime (entry));
  g_file_info_set_attribute_uint32 (info,
				    G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
				    archive_entry_mtime_nsec (entry) / 1000);

  switch (archive_entry_filetype (entry))
    {
      case AE_IFREG:
	type = G_FILE_TYPE_REGULAR;
	break;
      case AE_IFLNK:
	g_file_info_set_symlink_target (info,
	                                archive_entry_symlink (entry));
	type = G_FILE_TYPE_SYMBOLIC_LINK;
	break;
      case AE_IFDIR:
	type = G_FILE_TYPE_DIRECTORY;
	break;
      case AE_IFCHR:
      case AE_IFBLK:
      case AE_IFIFO:
	type = G_FILE_TYPE_SPECIAL;
	break;
      default:
	g_warning ("unknown file type %u", archive_entry_filetype (entry));
	type = G_FILE_TYPE_SPECIAL;
	break;
    }
  g_file_info_set_name (info, file->name);
  gvfs_file_info_populate_default (info,
				   file->name,
				   type);

  g_file_info_set_size (info,
			archive_entry_size (entry));

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, type == G_FILE_TYPE_DIRECTORY);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);

  /* FIXME: add info for these
dev_t			 archive_entry_dev(struct archive_entry *);
dev_t			 archive_entry_devmajor(struct archive_entry *);
dev_t			 archive_entry_devminor(struct archive_entry *);
void			 archive_entry_fflags(struct archive_entry *,
			     unsigned long *set, unsigned long *clear);
const char		*archive_entry_fflags_text(struct archive_entry *);
gid_t			 archive_entry_gid(struct archive_entry *);
const char		*archive_entry_gname(struct archive_entry *);
const char		*archive_entry_hardlink(struct archive_entry *);
ino_t			 archive_entry_ino(struct archive_entry *);
mode_t			 archive_entry_mode(struct archive_entry *);
unsigned int		 archive_entry_nlink(struct archive_entry *);
dev_t			 archive_entry_rdev(struct archive_entry *);
dev_t			 archive_entry_rdevmajor(struct archive_entry *);
dev_t			 archive_entry_rdevminor(struct archive_entry *);
uid_t			 archive_entry_uid(struct archive_entry *);
const char		*archive_entry_uname(struct archive_entry *);
  */

  /* FIXME: do ACLs */
}

static void
fixup_dirs (ArchiveFile *file)
{
  GSList *l;

  if (file->info == NULL)
    {
      GFileInfo *info = g_file_info_new ();
      
      file->info = info;
      g_file_info_set_name (info, file->name);
      gvfs_file_info_populate_default (info,
                                       file->name,
                                       G_FILE_TYPE_DIRECTORY);
    }
  
  for (l = file->children; l != NULL; l = l->next)
    fixup_dirs (l->data);
}

static void
create_file_tree (GVfsBackendArchive *ba, GVfsJob *job)
{
  GVfsArchive *archive;
  struct archive_entry *entry;
  int result;

  archive = gvfs_archive_new (ba, job);

  g_assert (ba->files != NULL);

  do
    {
      result = archive_read_next_header (archive->archive, &entry);
      if (result >= ARCHIVE_WARN && result <= ARCHIVE_OK)
	{
  	  if (result < ARCHIVE_OK) {
  	    DEBUG ("archive_read_next_header: result = %d, error = '%s'\n", result, archive_error_string (archive->archive));
  	    archive_set_error (archive->archive, ARCHIVE_OK, "No error");
  	    archive_clear_error (archive->archive);
	  }
  
	  ArchiveFile *file = archive_file_get_from_path (ba->files, 
	                                                  archive_entry_pathname (entry), 
							  TRUE);
          /* Don't set info for root */
          if (file != ba->files)
            archive_file_set_info_from_entry (file, entry);
	  archive_read_data_skip (archive->archive);
	}
    }
  while (result != ARCHIVE_FATAL && result != ARCHIVE_EOF);

  if (result == ARCHIVE_FATAL)
    gvfs_archive_set_error_from_errno (archive);
  fixup_dirs (ba->files);
  
  gvfs_archive_finish (archive);
}

static void
archive_file_free (ArchiveFile *file)
{
  g_slist_foreach (file->children, (GFunc) archive_file_free, NULL);
  g_slist_free (file->children);
  if (file->info)
    g_object_unref (file->info);
  g_free (file->name);
}

static void
do_mount (GVfsBackend *backend,
	  GVfsJobMount *job,
	  GMountSpec *mount_spec,
	  GMountSource *mount_source,
	  gboolean is_automount)
{
  GVfsBackendArchive *archive = G_VFS_BACKEND_ARCHIVE (backend);
  const char *host, *file;
  GFileInfo *info;
  GIcon *icon;
  char *filename, *s;
  GError *error = NULL;

  host = g_mount_spec_get (mount_spec, "host");
  file = g_mount_spec_get (mount_spec, "file");
  if (host == NULL &&
      file == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                       G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                       _("No hostname specified"));
      return;
    }

  if (host != NULL)
    {
      filename = g_uri_unescape_string (host, NULL);
      if (filename == NULL)
        {
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            _("Invalid mount spec"));
          return;
        }
      
      archive->file = g_file_new_for_commandline_arg (filename);
      g_free (filename);
    }
  else
    archive->file = g_file_new_for_commandline_arg (file);
  
  DEBUG ("Trying to mount %s\n", g_file_get_uri (archive->file));

  info = g_file_query_info (archive->file,
			    "*",
			    G_FILE_QUERY_INFO_NONE,
			    G_VFS_JOB (job)->cancellable,
			    &error);
  if (info == NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job),
				   error);
      g_error_free (error);
      return;
    }

  if (g_file_info_get_file_type (info) != G_FILE_TYPE_REGULAR)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Invalid mount spec"));
      return;
    }

  /* FIXME: check if this file is an archive */
  
  filename = g_file_get_uri (archive->file);
  DEBUG ("mounted %s\n", filename);
  s = g_uri_escape_string (filename, NULL, FALSE);
  mount_spec = g_mount_spec_new ("archive");
  g_mount_spec_set (mount_spec, "host", s);
  g_free (s);
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  g_mount_spec_unref (mount_spec);

  g_vfs_backend_set_display_name (backend, g_file_info_get_display_name (info));

  icon = g_file_info_get_icon (info);

  g_vfs_backend_set_icon_name (backend, MOUNT_ICON_NAME);

  create_root_file (archive);
  create_file_tree (archive, G_VFS_JOB (job));
  g_object_unref (info);
}

static void
do_unmount (GVfsBackend *backend,
	    GVfsJobUnmount *job,
            GMountUnmountFlags flags,
            GMountSource *mount_source)
{
  GVfsBackendArchive *ba = G_VFS_BACKEND_ARCHIVE (backend);

  g_object_unref (ba->file);
  ba->file = NULL;
  archive_file_free (ba->files);
  ba->files = NULL;

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_open_for_read (GVfsBackend *       backend,
		  GVfsJobOpenForRead *job,
		  const char *        filename)
{
  GVfsBackendArchive *ba = G_VFS_BACKEND_ARCHIVE (backend);
  GVfsArchive *archive;
  struct archive_entry *entry;
  int result;
  ArchiveFile *file;

  file = archive_file_find (ba, filename);
  if (file == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
		        G_IO_ERROR,
			G_IO_ERROR_NOT_FOUND,
			_("File doesn't exist"));
      return;
    }

  if (g_file_info_get_file_type (file->info) == G_FILE_TYPE_DIRECTORY)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
			G_IO_ERROR_IS_DIRECTORY,
			_("Can't open directory"));
      return;
    }
  
  archive = gvfs_archive_new (ba, G_VFS_JOB (job));

  do
    {
      result = archive_read_next_header (archive->archive, &entry);
      if (result >= ARCHIVE_WARN && result <= ARCHIVE_OK)
        {
	  if (result < ARCHIVE_OK) {
	    DEBUG ("do_open_for_read: result = %d, error = '%s'\n", result, archive_error_string (archive->archive));
	    archive_set_error (archive->archive, ARCHIVE_OK, "No error");
	    archive_clear_error (archive->archive);
	  }
                              
          if (g_str_equal (archive_entry_pathname (entry), filename + 1))
            {
              /* SUCCESS */
              g_vfs_job_open_for_read_set_handle (job, archive);
              g_vfs_job_open_for_read_set_can_seek (job, FALSE);
              gvfs_archive_pop_job (archive);
              return;
            }
          else
            archive_read_data_skip (archive->archive);
        }
    }
  while (result != ARCHIVE_FATAL && result != ARCHIVE_EOF);

  if (!gvfs_archive_in_error (archive))
    {
      g_set_error_literal (&archive->error,
			   G_IO_ERROR,
			   G_IO_ERROR_NOT_FOUND,
			   _("File doesn't exist"));
    }
  gvfs_archive_finish (archive);
}

static void
do_close_read (GVfsBackend *backend,
	       GVfsJobCloseRead *job,
	       GVfsBackendHandle handle)
{
  GVfsArchive *archive = handle;

  gvfs_archive_push_job (archive, G_VFS_JOB (job));
  gvfs_archive_finish (archive);
}

static void
do_read (GVfsBackend *backend,
	 GVfsJobRead *job,
	 GVfsBackendHandle handle,
	 char *buffer,
	 gsize bytes_requested)
{
  GVfsArchive *archive = handle;
  gssize bytes_read;

  gvfs_archive_push_job (archive, G_VFS_JOB (job));
  bytes_read = archive_read_data (archive->archive, buffer, bytes_requested);
  if (bytes_read >= 0)
    g_vfs_job_read_set_size (job, bytes_read);
  else
    gvfs_archive_set_error_from_errno (archive);
  gvfs_archive_pop_job (archive);
}

static void
do_query_info (GVfsBackend *backend,
	       GVfsJobQueryInfo *job,
	       const char *filename,
	       GFileQueryInfoFlags flags,
	       GFileInfo *info,
	       GFileAttributeMatcher *attribute_matcher)
{
  GVfsBackendArchive *ba = G_VFS_BACKEND_ARCHIVE (backend);
  ArchiveFile *file;

  file = archive_file_find (ba, filename);
  if (file == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
		        G_IO_ERROR,
			G_IO_ERROR_NOT_FOUND,
			_("File doesn't exist"));
      return;
    }

  if (!(flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS))
    g_warning ("FIXME: follow symlinks");

  g_file_info_copy_into (file->info, info);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_enumerate (GVfsBackend *backend,
	      GVfsJobEnumerate *job,
	      const char *filename,
	      GFileAttributeMatcher *attribute_matcher,
	      GFileQueryInfoFlags flags)
{
  GVfsBackendArchive *ba = G_VFS_BACKEND_ARCHIVE (backend);
  ArchiveFile *file;
  GSList *walk;

  file = archive_file_find (ba, filename);
  if (file == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
		        G_IO_ERROR,
			G_IO_ERROR_NOT_FOUND,
			_("File doesn't exist"));
      return;
    }

  if (g_file_info_get_file_type (file->info) != G_FILE_TYPE_DIRECTORY)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
		        G_IO_ERROR,
			G_IO_ERROR_NOT_DIRECTORY,
			_("The file is not a directory"));
      return;
    }

  if (!(flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS))
    g_warning ("FIXME: follow symlinks");

  for (walk = file->children; walk; walk = walk->next)
    {
      GFileInfo *info = g_file_info_dup (((ArchiveFile *) walk->data)->info);
      g_vfs_job_enumerate_add_info (job, info);
      g_object_unref (info);
    }
  g_vfs_job_enumerate_done (job);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_query_fs_info (GVfsBackend *backend,
                   GVfsJobQueryFsInfo *job,
                   const char *filename,
                   GFileInfo *info,
                   GFileAttributeMatcher *attribute_matcher)
{
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, TRUE);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_IF_LOCAL);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
}

static void
g_vfs_backend_archive_class_init (GVfsBackendArchiveClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_archive_finalize;

  backend_class->mount = do_mount;
  backend_class->unmount = do_unmount;
  backend_class->open_for_read = do_open_for_read;
  backend_class->close_read = do_close_read;
  backend_class->read = do_read;
  backend_class->enumerate = do_enumerate;
  backend_class->query_info = do_query_info;
  backend_class->try_query_fs_info = try_query_fs_info;
}
