/* gvfsfusedaemon.c - FUSE file system mapping daemon for GVFS
 * 
 * Copyright (C) 2007-2008 Hans Petter Jansson
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
 * Author: Hans Petter Jansson <hpj@novell.com>
 */

#include <config.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#include <dbus/dbus.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>
#include <gio/gio.h>

#include <glib/gurifuncs.h>

/* stuff from common/ */
#include <gdaemonmount.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsdbusutils.h>

#define FUSE_USE_VERSION 26
#include <fuse.h>

#define DEBUG_ENABLED 0

#define GET_FILE_HANDLE(fi)     ((gpointer) (fi)->fh)
#define SET_FILE_HANDLE(fi, fh) ((fi)->fh = (guint64) (fh))

typedef struct {
  time_t creation_time;
  char *name;
  GFile *root;
} MountRecord;

typedef enum {
  FILE_OP_NONE,
  FILE_OP_READ,
  FILE_OP_WRITE
} FileOp;

typedef struct {
  gint      refcount;

  GMutex   *mutex;
  gchar    *path;
  FileOp    op;
  gpointer  stream;
  goffset   pos;
} FileHandle;

static GThread        *subthread             = NULL;
static GMainLoop      *subthread_main_loop   = NULL;
static GVfs           *gvfs                  = NULL;

static GVolumeMonitor *volume_monitor        = NULL;

/* Contains pointers to MountRecord */
static GList          *mount_list            = NULL;
static GMutex         *mount_list_mutex;

static time_t          daemon_creation_time;
static uid_t           daemon_uid;
static gid_t           daemon_gid;

static GStaticMutex    global_mutex          = G_STATIC_MUTEX_INIT;
static GHashTable     *global_path_to_fh_map = NULL;
static GHashTable     *global_active_fh_map  = NULL;

/* ------- *
 * Helpers *
 * ------- */

static void
debug_print (const gchar *message, ...)
{
#if DEBUG_ENABLED

  static FILE *debug_fhd = NULL;
  va_list      var_args;
  char *file;

  if (!debug_fhd)
    {
      file = g_build_filename (g_get_home_dir (), "vfs.debug", NULL);
      debug_fhd = fopen (file, "at");
      g_free (file);
    }

  if (!debug_fhd)
    return;
  
  va_start (var_args, message);
  g_vfprintf (debug_fhd, message, var_args);
  va_end (var_args);
  
  fflush (debug_fhd);

#endif
}

typedef struct {
  gint gioerror;
  gint errno_value;
} ErrorMap;

static gint
errno_from_error (GError *error)
{
  gint i;
  static const ErrorMap error_map [] =
    {
      { G_IO_ERROR_FAILED,             EIO          },
      { G_IO_ERROR_NOT_FOUND,          ENOENT       },
      { G_IO_ERROR_EXISTS,             EEXIST       },
      { G_IO_ERROR_IS_DIRECTORY,       EISDIR       },
      { G_IO_ERROR_NOT_DIRECTORY,      ENOTDIR      },
      { G_IO_ERROR_NOT_EMPTY,          ENOTEMPTY    },
      { G_IO_ERROR_NOT_REGULAR_FILE,   EIO          },
      { G_IO_ERROR_NOT_SYMBOLIC_LINK,  EIO          },
      { G_IO_ERROR_NOT_MOUNTABLE_FILE, EIO          },
      { G_IO_ERROR_FILENAME_TOO_LONG,  ENAMETOOLONG },
      { G_IO_ERROR_INVALID_FILENAME,   EIO          },
      { G_IO_ERROR_TOO_MANY_LINKS,     ELOOP        },
      { G_IO_ERROR_NO_SPACE,           ENOSPC       },
      { G_IO_ERROR_INVALID_ARGUMENT,   EINVAL       },
      { G_IO_ERROR_PERMISSION_DENIED,  EACCES       },
      { G_IO_ERROR_NOT_SUPPORTED,      ENOTSUP      },
      { G_IO_ERROR_NOT_MOUNTED,        EIO          },
      { G_IO_ERROR_ALREADY_MOUNTED,    EIO          },
      { G_IO_ERROR_CLOSED,             EIO          },
      { G_IO_ERROR_CANCELLED,          EIO          },
      { G_IO_ERROR_PENDING,            EIO          },
      { G_IO_ERROR_READ_ONLY,          EACCES       },
      { G_IO_ERROR_CANT_CREATE_BACKUP, EIO          },
      { G_IO_ERROR_WRONG_ETAG,         EIO          },
      { G_IO_ERROR_TIMED_OUT,          EIO          },
      { G_IO_ERROR_BUSY,               EBUSY        },
      { G_IO_ERROR_WOULD_BLOCK,        EAGAIN       },
      { -1,                            -1           }
    };

  if (error->domain != G_IO_ERROR)
    return EIO;

  for (i = 0; error_map [i].gioerror >= 0; i++)
    {
      if (error_map [i].gioerror == error->code)
        break;
    }

  if (error_map [i].gioerror >= 0)
    return error_map [i].errno_value;

  return EIO;
}

/* Conveys the pid of the client to filesystem backends; see
 * get_pid_for_file() in gdaemonfile.c
 *
 * can only be called during a filesystem op
 */
static void
set_pid_for_file (GFile *file)
{
  struct fuse_context *context;

  if (file == NULL)
    goto out;

  context = fuse_get_context ();
  if (context == NULL)
    goto out;

  g_object_set_data (G_OBJECT (file), "gvfs-fuse-client-pid", GUINT_TO_POINTER (context->pid));

 out:
  ;
}

static FileHandle *
file_handle_new (const gchar *path)
{
  FileHandle *file_handle;

  file_handle = g_new0 (FileHandle, 1);
  file_handle->refcount = 1;
  file_handle->mutex = g_mutex_new ();
  file_handle->op = FILE_OP_NONE;
  file_handle->path = g_strdup (path);

  g_hash_table_insert (global_active_fh_map, file_handle, file_handle);

  return file_handle;
}

static FileHandle *
file_handle_ref (FileHandle *file_handle)
{
  g_atomic_int_inc (&file_handle->refcount);
  return file_handle;
}

static void
file_handle_unref (FileHandle *file_handle)
{
  if (g_atomic_int_dec_and_test (&file_handle->refcount))
    {
      gint refs;

      g_static_mutex_lock (&global_mutex);

      /* Test again, since e.g. get_file_handle_for_path() might have
       * snatched the global mutex and revived the file handle between
       * g_atomic_int_dec_and_test() and us obtaining the global lock. */

      refs = g_atomic_int_get (&file_handle->refcount);

      if (refs == 0)
        g_hash_table_remove (global_path_to_fh_map, file_handle->path);

      g_static_mutex_unlock (&global_mutex);
    }
}

static void
file_handle_close_stream (FileHandle *file_handle)
{
  debug_print ("file_handle_close_stream\n");
  if (file_handle->stream)
    {
      switch (file_handle->op)
        {
        case FILE_OP_READ:
          g_input_stream_close (file_handle->stream, NULL, NULL);
          break;
          
        case FILE_OP_WRITE:
          g_output_stream_close (file_handle->stream, NULL, NULL);
          break;
          
        default:
          g_assert_not_reached ();
        }
      
      g_object_unref (file_handle->stream);
      file_handle->stream = NULL;
      file_handle->op = FILE_OP_NONE;
    }
}

/* Called on hash table removal */
static void
file_handle_free (FileHandle *file_handle)
{
  g_hash_table_remove (global_active_fh_map, file_handle);

  file_handle_close_stream (file_handle);
  g_mutex_free (file_handle->mutex);
  g_free (file_handle->path);
  g_free (file_handle);
}

static FileHandle *
get_file_handle_for_path (const gchar *path)
{
  FileHandle *fh;

  g_static_mutex_lock (&global_mutex);

  fh = g_hash_table_lookup (global_path_to_fh_map, path);

  if (fh)
    file_handle_ref (fh);

  g_static_mutex_unlock (&global_mutex);
  return fh;
}

static FileHandle *
get_or_create_file_handle_for_path (const gchar *path)
{
  FileHandle *fh;

  g_static_mutex_lock (&global_mutex);

  fh = g_hash_table_lookup (global_path_to_fh_map, path);

  if (fh)
    {
      file_handle_ref (fh);
    }
  else
    {
      fh = file_handle_new (path);
      g_hash_table_insert (global_path_to_fh_map, fh->path, fh);
    }

  g_static_mutex_unlock (&global_mutex);
  return fh;
}

static FileHandle *
get_file_handle_from_info (struct fuse_file_info *fi)
{
  FileHandle *fh;

  g_static_mutex_lock (&global_mutex);

  fh = GET_FILE_HANDLE (fi);

  /* If the file handle is still valid, its value won't change. If
   * invalid, it's set to NULL. */
  fh = g_hash_table_lookup (global_active_fh_map, fh);

  if (fh)
    file_handle_ref (fh);

  g_static_mutex_unlock (&global_mutex);
  return fh;
}

static void
reindex_file_handle_for_path (const gchar *old_path, const gchar *new_path)
{
  gchar      *old_path_internal;
  FileHandle *fh;

  g_static_mutex_lock (&global_mutex);

  if (!g_hash_table_lookup_extended (global_path_to_fh_map, old_path,
                                     (gpointer *) &old_path_internal,
                                     (gpointer *) &fh))
      goto out;

  g_hash_table_steal (global_path_to_fh_map, old_path);

  g_free (fh->path);
  fh->path = g_strdup (new_path);

  g_hash_table_insert (global_path_to_fh_map, fh->path, fh);

 out:
  g_static_mutex_unlock (&global_mutex);
}

static MountRecord *
mount_record_new (GMount *mount)
{
  MountRecord *mount_record;
  char *name;

  mount_record = g_new (MountRecord, 1);
  
  mount_record->root = g_mount_get_root (mount);
  name = g_object_get_data (G_OBJECT (mount), "g-stable-name");
  if (name != NULL && *name != 0)
    name = g_strdup (name);
  else
    name = g_mount_get_name (mount);

  /* Keep in sync with gvfs daemon mount tracker */
  mount_record->name = g_uri_escape_string (name, "+@#$., ", TRUE);
  g_free (name);
  mount_record->creation_time = time (NULL);
  
  return mount_record;
}

static void
mount_record_free (MountRecord *mount_record)
{
  g_object_unref (mount_record->root);
  g_free (mount_record->name);
  g_free (mount_record);
}

static void
mount_list_lock (void)
{
  g_mutex_lock (mount_list_mutex);
}

static void
mount_list_unlock (void)
{
  g_mutex_unlock (mount_list_mutex);
}

static void
mount_list_free (void)
{
  g_list_foreach (mount_list, (GFunc) mount_record_free, NULL);
  g_list_free (mount_list);
  mount_list = NULL;
}

static gboolean
mount_record_for_mount_exists (GMount *mount)
{
  GList *l;
  GFile *root;
  gboolean res;

  g_assert (mount != NULL);

  root = g_mount_get_root (mount);

  res = FALSE;
  
  mount_list_lock ();

  for (l = mount_list; l != NULL; l = l->next)
    {
      MountRecord *this_mount_record = l->data;
      
      if (g_file_equal (root, this_mount_record->root))
        {
          res = TRUE;
          break;
        }
    }

  mount_list_unlock ();

  g_object_unref (root);
  
  return res;
}

static GFile *
mount_record_find_root_by_mount_name (const gchar *mount_name)
{
  GList       *l;
  GFile *root;

  g_assert (mount_name != NULL);

  root = NULL;
  
  mount_list_lock ();

  for (l = mount_list; l != NULL; l = l->next)
    {
      MountRecord *mount_record = l->data;

      if (strcmp (mount_name, mount_record->name) == 0)
        {
          root = g_object_ref (mount_record->root);
          break;
        }
    }

  mount_list_unlock ();

  return root;
}

static void
mount_list_update (void)
{
  GList *mounts;
  GList *l;

  mounts = g_volume_monitor_get_mounts (volume_monitor);

  for (l = mounts; l != NULL; l = l->next)
    {
      GMount *mount = l->data;

      if (!mount_record_for_mount_exists (mount))
        {
          mount_list_lock ();
          mount_list = g_list_prepend (mount_list, mount_record_new (mount));
          mount_list_unlock ();
        }
      
      g_object_unref (mount);
    }
  
  g_list_free (mounts);
}

#if 0

static gint
file_info_get_attribute_as_int (GFileInfo *file_info, const gchar *attribute)
{
  GFileAttributeType attribute_type;
  gint               int_result;

  attribute_type = g_file_info_get_attribute_type (file_info, attribute);

  switch (attribute_type)
    {
      case G_FILE_ATTRIBUTE_TYPE_UINT32:
        int_result = g_file_info_get_attribute_uint32 (file_info, attribute);
        break;

      case G_FILE_ATTRIBUTE_TYPE_INT32:
        int_result = g_file_info_get_attribute_int32 (file_info, attribute);
        break;

      case G_FILE_ATTRIBUTE_TYPE_UINT64:
        int_result = g_file_info_get_attribute_uint64 (file_info, attribute);
        break;

      case G_FILE_ATTRIBUTE_TYPE_INT64:
        int_result = g_file_info_get_attribute_int64 (file_info, attribute);
        break;

      default:
        int_result = 0;
        g_assert_not_reached ();
        break;
    }

  return int_result;
}

#endif

static guint
file_info_get_attribute_as_uint (GFileInfo *file_info, const gchar *attribute)
{
  GFileAttributeType attribute_type;
  guint              uint_result;

  attribute_type = g_file_info_get_attribute_type (file_info, attribute);

  switch (attribute_type)
    {
      case G_FILE_ATTRIBUTE_TYPE_UINT32:
        uint_result = g_file_info_get_attribute_uint32 (file_info, attribute);
        break;

      case G_FILE_ATTRIBUTE_TYPE_INT32:
        uint_result = g_file_info_get_attribute_int32 (file_info, attribute);
        break;

      case G_FILE_ATTRIBUTE_TYPE_UINT64:
        uint_result = g_file_info_get_attribute_uint64 (file_info, attribute);
        break;

      case G_FILE_ATTRIBUTE_TYPE_INT64:
        uint_result = g_file_info_get_attribute_int64 (file_info, attribute);
        break;

      default:
        uint_result = 0;
        g_assert_not_reached ();
        break;
    }

  return uint_result;
}

static gboolean
path_is_mount_list (const gchar *path)
{
  while (*path == '/')
    path++;

  return *path == 0;
}


static GFile *
file_from_full_path (const gchar *path)
{
  gchar *mount_name;
  GFile *file = NULL;
  const gchar *s1, *s2;
  GFile *root;

  file = NULL;
  
  s1 = path;
  while (*s1 == '/')
    s1++;
  
  if (*s1)
    {
      s2 = strchr (s1, '/');
      if (s2 == NULL)
        s2 = s1 + strlen (s1);
      
      mount_name = g_strndup (s1, s2 - s1);
      root = mount_record_find_root_by_mount_name (mount_name);
      g_free (mount_name);
      
      if (root)
        {
          while (*s2 == '/')
            s2++;
          file = g_file_resolve_relative_path (root, s2);
          g_object_unref (root);
        }
    }

  return file;
}

/* ------------- *
 * VFS functions *
 * ------------- */

#if 0

/* A stat -f <path> on a FUSE mount always results in this function being called with a path
 * of '/'. This means we can't return valid info for the various mounts. Hopefully we can
 * fix this in the future. */

static gint
vfs_statfs (const gchar *path, struct statvfs *stbuf)
{
  GFile  *file;
  GError *error  = NULL;
  gint    result = 0;

  debug_print ("vfs_statfs: %s\n", path);

  if ((file = file_from_full_path (path)))
    {
      GFileInfo *file_info;

      file_info = g_file_get_filesystem_info (file, "*", NULL, &error);

      if (file_info)
        {
          memset (stbuf, 0, sizeof (*stbuf));

          stbuf->f_bsize = 4096;
          stbuf->f_frsize = 4096;  /* Ignored by FUSE */
          stbuf->f_blocks = 0;
          stbuf->f_bfree = 0;
          stbuf->f_bavail = 0;
          stbuf->f_files = 0;
          stbuf->f_ffree = 0;
          stbuf->f_favail = 0;  /* Ignored by FUSE */
          stbuf->f_fsid = 1;  /* Ignored by FUSE */
          stbuf->f_flag = 0;  /* Ignored by FUSE */
          stbuf->f_namemax = 1024;

          if (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE))
            stbuf->f_blocks = g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) / 4096;
          if (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE))
            stbuf->f_bfree = stbuf->f_bavail = g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE) / 4096;
        }
      else if (error)
        {
          result = -errno_from_error (error);
          g_error_free (error);
        }
      else
        {
          result = -EIO;
        }

      g_object_unref (file);
    }

  debug_print ("vfs_statfs: -> %s\n", g_strerror (-result));

  return result;
}

#endif

static gint
vfs_statfs (const gchar *path, struct statvfs *stbuf)
{
  gint result = 0;

  debug_print ("vfs_statfs: %s\n", path);

  memset (stbuf, 0, sizeof (*stbuf));

  stbuf->f_bsize = 4096;
  stbuf->f_frsize = 4096;  /* Ignored by FUSE */
  stbuf->f_blocks = 0;
  stbuf->f_bfree = 0;
  stbuf->f_bavail = 0;
  stbuf->f_files = 0;
  stbuf->f_ffree = 0;
  stbuf->f_favail = 0;  /* Ignored by FUSE */
  stbuf->f_fsid = 1;  /* Ignored by FUSE */
  stbuf->f_flag = 0;  /* Ignored by FUSE */
  stbuf->f_namemax = 1024;

  debug_print ("vfs_statfs: -> %s\n", g_strerror (-result));

  return result;
}

static mode_t
file_info_get_stat_mode (GFileInfo *file_info)
{
  GFileType         file_type;
  mode_t            unix_mode;

  file_type = g_file_info_get_file_type (file_info);

  switch (file_type)
    {
      case G_FILE_TYPE_REGULAR:
        unix_mode = S_IFREG;
        break;

      case G_FILE_TYPE_DIRECTORY:
      case G_FILE_TYPE_MOUNTABLE:
        unix_mode = S_IFDIR;
        break;

      case G_FILE_TYPE_SYMBOLIC_LINK:
      case G_FILE_TYPE_SHORTCUT:
        unix_mode = S_IFLNK;
        break;

      case G_FILE_TYPE_SPECIAL:
      default:
        unix_mode = 0;
        break;
    }

  if (file_type == G_FILE_TYPE_DIRECTORY ||
      !g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ) ||
      g_file_info_get_attribute_boolean (file_info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ))
    unix_mode |= S_IRUSR;
  if (!g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE) ||
      g_file_info_get_attribute_boolean (file_info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
    unix_mode |= S_IWUSR;
  if (file_type == G_FILE_TYPE_DIRECTORY ||
      !g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE) ||
      g_file_info_get_attribute_boolean (file_info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE))
    unix_mode |= S_IXUSR;
  
  return unix_mode;
}

static gint
getattr_for_file (GFile *file, struct stat *sbuf)
{
  GFileInfo *file_info;
  GError    *error  = NULL;
  gint       result = 0;

  file_info = g_file_query_info (file, 
                                 G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                 G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                 G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK ","
                                 G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                                 G_FILE_ATTRIBUTE_UNIX_MODE ","
                                 G_FILE_ATTRIBUTE_TIME_CHANGED ","
                                 G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                                 G_FILE_ATTRIBUTE_TIME_ACCESS ","
                                 G_FILE_ATTRIBUTE_UNIX_BLOCK_SIZE ","
                                 G_FILE_ATTRIBUTE_UNIX_BLOCKS ","
				 "access::*",
                                 0, NULL, &error);

  if (file_info)
    {
      GTimeVal mod_time;

      sbuf->st_mode = file_info_get_stat_mode (file_info);
      sbuf->st_size = g_file_info_get_size (file_info);
      sbuf->st_uid = daemon_uid;
      sbuf->st_gid = daemon_gid;

      g_file_info_get_modification_time (file_info, &mod_time);
      sbuf->st_mtime = mod_time.tv_sec;
      sbuf->st_ctime = mod_time.tv_sec;
      sbuf->st_atime = mod_time.tv_sec;

      if (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_TIME_CHANGED))
        sbuf->st_ctime = file_info_get_attribute_as_uint (file_info, G_FILE_ATTRIBUTE_TIME_CHANGED);
      if (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_TIME_ACCESS))
        sbuf->st_atime = file_info_get_attribute_as_uint (file_info, G_FILE_ATTRIBUTE_TIME_ACCESS);

      if (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_UNIX_BLOCK_SIZE))
	sbuf->st_blksize = file_info_get_attribute_as_uint (file_info, G_FILE_ATTRIBUTE_UNIX_BLOCK_SIZE);
      if (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_UNIX_BLOCKS))
	sbuf->st_blocks = file_info_get_attribute_as_uint (file_info, G_FILE_ATTRIBUTE_UNIX_BLOCKS);
      else /* fake it to make 'du' work like 'du --apparent'. */
	sbuf->st_blocks = (sbuf->st_size + 511) / 512;

      /* Setting st_nlink to 1 for directories makes 'find' work */
      sbuf->st_nlink = 1;

      g_object_unref (file_info);
    }
  else
    {
      if (error)
        {
          debug_print ("Error from GVFS: %s\n", error->message);
          result = -errno_from_error (error);
          g_error_free (error);
        }
      else
        {
          debug_print ("No file info, but no error from GVFS.\n");
          result = -EIO;
        }
    }

  return result;
}

static gint
vfs_getattr (const gchar *path, struct stat *sbuf)
{
  GFile      *file;
  gint        result = 0;

  debug_print ("vfs_getattr: %s\n", path);

  memset (sbuf, 0, sizeof (*sbuf));

  sbuf->st_dev = 0;                     /* dev_t     ID of device containing file */
  sbuf->st_ino = 0;                     /* ino_t     inode number */
  sbuf->st_uid = 0;                     /* uid_t     user ID of owner */
  sbuf->st_gid = 0;                     /* gid_t     group ID of owner */
  sbuf->st_rdev = 0;                    /* dev_t     device ID (if special file) */
  sbuf->st_size = 0;                    /* off_t     total size, in bytes */
  sbuf->st_blocks = 0;                  /* blkcnt_t  number of blocks allocated */
  sbuf->st_atime = 0;                   /* time_t    time of last access */
  sbuf->st_mtime = 0;                   /* time_t    time of last modification */
  sbuf->st_ctime = 0;                   /* time_t    time of last status change */
  sbuf->st_blksize = 4096;              /* blksize_t blocksize for filesystem I/O */

  if (path_is_mount_list (path))
    {
      /* Mount list */

      sbuf->st_mode = S_IFDIR | 0500;                   /* mode_t    protection */
      sbuf->st_nlink = 2 + g_list_length (mount_list);  /* nlink_t   number of hard links */
      sbuf->st_atime = daemon_creation_time;
      sbuf->st_mtime = daemon_creation_time;
      sbuf->st_ctime = daemon_creation_time;
      sbuf->st_uid   = daemon_uid;
      sbuf->st_gid   = daemon_gid;
    }
  else if ((file = file_from_full_path (path)))
    {
      /* Submount */

      result = getattr_for_file (file, sbuf);
      g_object_unref (file);
    }
  else
    {
      result = -ENOENT;
    }

  debug_print ("vfs_getattr: -> %s\n", g_strerror (-result));

  return result;
}

static gint
vfs_readlink (const gchar *path, gchar *target, size_t size)
{
  debug_print ("vfs_readlink: %s\n", path);

  /* This is not implemented because it would allow remote servers to launch
   * symlink attacks on the local machine.  There's not much of a use for
   * "readlink" anyway since we don't pass G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
   * so non-broken symlinks will be followed transparently. */

  return -ENOSYS;
}

static gint
setup_input_stream (GFile *file, FileHandle *fh)
{
  GError *error  = NULL;
  gint    result = 0;

  if (fh->stream)
    {
      debug_print ("setup_input_stream: have stream\n");

      if (fh->op == FILE_OP_READ)
        {
          debug_print ("setup_input_stream: doing read\n");
        }
      else
        {
          debug_print ("setup_input_stream: doing write\n");

          g_output_stream_close (fh->stream, NULL, NULL);
          g_object_unref (fh->stream);
          fh->stream = NULL;
        }
    }

  if (!fh->stream)
    {
      debug_print ("setup_input_stream: no stream\n");
      fh->stream = g_file_read (file, NULL, &error);
      fh->pos = 0;
    }

  if (fh->stream)
    fh->op = FILE_OP_READ;
  else
    fh->op = FILE_OP_NONE;

  if (error)
    {
      debug_print ("setup_input_stream: error\n");
      result = -errno_from_error (error);
      g_error_free (error);
    }

  return result;
}

static gint
setup_output_stream (GFile *file, FileHandle *fh)
{
  GError *error  = NULL;
  gint    result = 0;

  if (fh->stream)
    {
      if (fh->op == FILE_OP_WRITE)
        {
        }
      else
        {
          g_input_stream_close (fh->stream, NULL, NULL);
          g_object_unref (fh->stream);
          fh->stream = NULL;
        }
    }

  if (!fh->stream)
    {
      fh->stream = g_file_append_to (file, 0, NULL, &error);
      if (fh->stream)
        fh->pos = g_seekable_tell (G_SEEKABLE (fh->stream));
    }

  if (fh->stream)
    fh->op = FILE_OP_WRITE;
  else
    fh->op = FILE_OP_NONE;

  if (error)
    {
      result = -errno_from_error (error);
      g_error_free (error);
    }

  return result;
}

static gint
vfs_open (const gchar *path, struct fuse_file_info *fi)
{
  GFile *file;
  gint   result = 0;

  debug_print ("vfs_open: %s\n", path);

  if (path_is_mount_list (path))
    result = -EISDIR;
  else if ((file = file_from_full_path (path)))
    {
      GFileInfo *file_info;
      GError    *error = NULL;

      file_info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_TYPE, 0, NULL, &error);

      if (file_info)
        {
          GFileType file_type = g_file_info_get_file_type (file_info);

          if (file_type == G_FILE_TYPE_REGULAR)
            {
              FileHandle *fh = get_or_create_file_handle_for_path (path);

              g_mutex_lock (fh->mutex);

              /* File exists */

              SET_FILE_HANDLE (fi, fh);

              debug_print ("vfs_open: flags=%o\n", fi->flags);

              /* Set up a stream here, so we can check for errors */
              set_pid_for_file (file);

              if (fi->flags & O_WRONLY || fi->flags & O_RDWR)
                result = setup_output_stream (file, fh);
              else
                result = setup_input_stream (file, fh);

              g_mutex_unlock (fh->mutex);

              /* The added reference to the file handle is released in vfs_release() */
            }
          else if (file_type == G_FILE_TYPE_DIRECTORY)
            {
              /* EISDIR is supposedly only for attempts to write to directory handles,
               * but outside readdir(), we don't support reading them either. */
              result = -EISDIR;
            }
          else
            {
              result = -EACCES;
            }

          g_object_unref (file_info);
        }
      else
        {
          if (error)
            {
              debug_print ("Error from GVFS: %s\n", error->message);
              result = -errno_from_error (error);
              g_error_free (error);
            }
          else
            {
              debug_print ("No file info, but no error from GVFS.\n");
              result = -EIO;
            }
        }

      g_object_unref (file);
    }
  else
    {
      result = -ENOENT;
    }

  debug_print ("vfs_open: -> %s\n", g_strerror (-result));

  return result;
}

static gint
vfs_create (const gchar *path, mode_t mode, struct fuse_file_info *fi)
{
  GFile *file;
  gint   result = 0;

  debug_print ("vfs_create: %s\n", path);

  if (path_is_mount_list (path))
    result = -EEXIST;
  if ((file = file_from_full_path (path)))
    {
      GFileInfo *file_info;
      GError    *error = NULL;

      file_info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_TYPE, 0, NULL, &error);

      if (file_info)
        {
          result = -EEXIST;
          g_object_unref (file_info);
        }
      else
        {
          GFileOutputStream *file_output_stream;

          if (error)
            {
              g_error_free (error);
              error = NULL;
            }

          file_output_stream = g_file_create (file, 0, NULL, &error);
          set_pid_for_file (file);
          if (file_output_stream)
            {
              FileHandle *fh = get_or_create_file_handle_for_path (path);

              /* Success */

              g_mutex_lock (fh->mutex);

              SET_FILE_HANDLE (fi, fh);

              file_handle_close_stream (fh);
              fh->stream = file_output_stream;
              fh->op = FILE_OP_WRITE;

              g_mutex_unlock (fh->mutex);

              /* The added reference to the file handle is released in vfs_release() */
            }
          else
            {
              result = -errno_from_error (error);
              g_error_free (error);
            }
        }

      g_object_unref (file);
    }
  else
    {
      result = -ENOENT;
    }

  debug_print ("vfs_create: -> %s\n", g_strerror (-result));

  return result;
}

static gint
vfs_release (const gchar *path, struct fuse_file_info *fi)
{
  FileHandle *fh = get_file_handle_from_info (fi);

  debug_print ("vfs_release: %s\n", path);

  if (fh)
    {
      /* get_file_handle_from_info () adds a "working ref", so unref twice. */
      file_handle_unref (fh);
      file_handle_unref (fh);
    }

  return 0;
}

static gint
read_stream (FileHandle *fh, gchar *output_buf, size_t output_buf_size, off_t offset)
{
  GInputStream *input_stream;
  gint          n_bytes_skipped = 0;
  gint          n_bytes_read    = 0;
  gint          result          = 0;
  GError       *error           = NULL;

  input_stream = fh->stream;

  if (offset != fh->pos)
    {
      if (g_seekable_can_seek (G_SEEKABLE (input_stream)))
        {
          /* Can seek */

          debug_print ("read_stream: seeking to offset %d.\n", offset);

          if (g_seekable_seek (G_SEEKABLE (input_stream), offset, G_SEEK_SET, NULL, &error))
            {
              fh->pos = offset;
            }
          else
            {
              result = -errno_from_error (error);
              g_error_free (error);
            }
        }
      else if (offset > fh->pos)
        {
          /* Can skip ahead */

          debug_print ("read_stream: skipping to offset %d.\n", offset);

          n_bytes_skipped = g_input_stream_skip (input_stream, offset - fh->pos, NULL, &error);

          if (n_bytes_skipped > 0)
            fh->pos += n_bytes_skipped;

          if (n_bytes_skipped != offset - fh->pos)
            {
              if (error)
                {
                  result = -errno_from_error (error);
                  g_error_free (error);
                }
              else
                {
                  result = -EIO;
                }
            }
        }
      else
        {
          /* Can't seek, can't skip backwards */

          debug_print ("read_stream: can't seek nor skip to offset %d!\n", offset);

          result = -ENOTSUP;
        }
    }

  if (result == 0)
    {
      while (n_bytes_read < output_buf_size)
        {
          gboolean part_result;
          gsize    part_bytes_read = 0;

          part_result = g_input_stream_read_all (input_stream,
                                                 output_buf + n_bytes_read,
                                                 output_buf_size - n_bytes_read,
                                                 &part_bytes_read,
                                                 NULL,
                                                 &error);

          n_bytes_read += part_bytes_read;
          fh->pos += part_bytes_read;

          if (!part_result || part_bytes_read == 0)
            break;
        }

      result = n_bytes_read;

      if (n_bytes_read < output_buf_size)
        {
          debug_print ("read_stream: wanted %d bytes, but got %d.\n", output_buf_size, n_bytes_read);

          if (error)
            {
              result = -errno_from_error (error);
              g_error_free (error);
            }
        }
    }

  return result;
}

static gint
vfs_read (const gchar *path, gchar *buf, size_t size,
          off_t offset, struct fuse_file_info *fi)
{
  GFile *file;
  gint   result = 0;

  debug_print ("vfs_read: %s\n", path);

  if ((file = file_from_full_path (path)))
    {
      FileHandle *fh = get_file_handle_from_info (fi);

      if (fh)
        {
          g_mutex_lock (fh->mutex);

          result = setup_input_stream (file, fh);

          if (result == 0)
            {
              result = read_stream (fh, buf, size, offset);
            }
          else
            {
              debug_print ("vfs_read: failed to setup input_stream!\n");
            }

          g_mutex_unlock (fh->mutex);
          file_handle_unref (fh);
        }
      else
        {
          result = -EINVAL;
        }

      g_object_unref (file);
    }
  else
    {
      result = -EIO;
    }

  if (result < 0)
    debug_print ("vfs_read: -> %s\n", g_strerror (-result));
  else
    debug_print ("vfs_read: -> %d bytes read.\n", result);

  return result;
}

static gint
write_stream (FileHandle *fh, const gchar *input_buf, size_t input_buf_size, off_t offset)
{
  GOutputStream *output_stream;
  gint           n_bytes_written = 0;
  gint           result          = 0;
  GError        *error           = NULL;

  debug_print ("write_stream: %d bytes at offset %d.\n", input_buf_size, offset);

  output_stream = fh->stream;

  if (offset != fh->pos)
    {
      if (g_seekable_can_seek (G_SEEKABLE (output_stream)))
        {
          /* Can seek */

          if (g_seekable_seek (G_SEEKABLE (output_stream), offset, G_SEEK_SET, NULL, &error))
            {
              fh->pos = offset;
            }
          else
            {
              result = -errno_from_error (error);
              g_error_free (error);
            }
        }
      else
        {
          /* Can't seek, and output streams can't skip */

          result = -ENOTSUP;
        }
    }

  if (result == 0)
    {
      while (n_bytes_written < input_buf_size)
        {
          gboolean part_result;
          gsize    part_bytes_written = 0;

          part_result = g_output_stream_write_all (output_stream,
                                                   (void *) (input_buf + n_bytes_written),
                                                   input_buf_size - n_bytes_written,
                                                   &part_bytes_written,
                                                   NULL,
                                                   &error);

          n_bytes_written += part_bytes_written;
          fh->pos += part_bytes_written;

          if (!part_result)
            break;
        }

      result = n_bytes_written;

      if (n_bytes_written < input_buf_size)
        {
          if (error)
            {
              result = -errno_from_error (error);
              g_error_free (error);
            }
        }
      else if (!g_output_stream_flush (output_stream, NULL, &error))
        {
          result = -errno_from_error (error);
          g_error_free (error);
        }
    }

  return result;
}

static gint
vfs_write (const gchar *path, const gchar *buf, size_t len, off_t offset,
           struct fuse_file_info *fi)
{
  GFile *file;
  gint   result = 0;

  debug_print ("vfs_write: %s\n", path);

  if ((file = file_from_full_path (path)))
    {
      FileHandle *fh = get_file_handle_from_info (fi);

      if (fh)
        {
          g_mutex_lock (fh->mutex);

          result = setup_output_stream (file, fh);
          if (result == 0)
            {
              result = write_stream (fh, buf, len, offset);
            }

          g_mutex_unlock (fh->mutex);
          file_handle_unref (fh);
        }
      else
        {
          result = -EINVAL;
        }

      g_object_unref (file);
    }
  else
    {
      result = -EIO;
    }

  if (result < 0)
    debug_print ("vfs_write: -> %s\n", g_strerror (-result));
  else
    debug_print ("vfs_write: -> %d bytes written.\n", result);

  return result;
}

static gint
vfs_flush (const gchar *path, struct fuse_file_info *fi)
{
  FileHandle *fh = get_file_handle_from_info (fi);

  debug_print ("vfs_flush: %s\n", path);

  if (fh)
    {
      g_mutex_lock (fh->mutex);
      file_handle_close_stream (fh);
      g_mutex_unlock (fh->mutex);

      /* get_file_handle_from_info () adds a "working ref", so release that. */
      file_handle_unref (fh);
    }

  /* TODO: Error handling. */
  return 0;
}

static gint
vfs_fsync (const gchar *path, gint sync_data_only, struct fuse_file_info *fi)
{
  FileHandle *fh = get_file_handle_from_info (fi);

  debug_print ("vfs_flush: %s\n", path);

  if (fh)
    {
      g_mutex_lock (fh->mutex);
      file_handle_close_stream (fh);
      g_mutex_unlock (fh->mutex);

      /* get_file_handle_from_info () adds a "working ref", so release that. */
      file_handle_unref (fh);
    }

  /* TODO: Error handling. */
  return 0;
}

static gint
vfs_opendir (const gchar *path, struct fuse_file_info *fi)
{
  GFile *file;
  gint result = 0;

  debug_print ("vfs_opendir: %s\n", path);

  if (path_is_mount_list (path))
    {
      /* Mount list */
    }
  else if ((file = file_from_full_path (path)) != NULL)
    {
      /* Submount */

      /* TODO: Check that path exists */

      g_object_unref (file);
    }
  else
    {
      /* Not found */

      result = -ENOENT;
    }

  return result;
}

static gint
readdir_for_file (GFile *base_file, gpointer buf, fuse_fill_dir_t filler)
{
  GFileEnumerator *enumerator;
  GFileInfo       *file_info;
  GError          *error = NULL;

  g_assert (base_file != NULL);

  enumerator = g_file_enumerate_children (base_file, G_FILE_ATTRIBUTE_STANDARD_NAME, 0, NULL, &error);
  if (!enumerator)
    {
      gint result;

      if (error)
        {
          debug_print ("Error from GVFS: %s\n", error->message);
          result = -errno_from_error (error);
          g_error_free (error);
        }
      else
        {
          debug_print ("No file info, but no error from GVFS.\n");
          result = -EIO;
        }

      return result;
    }

  filler (buf, ".", NULL, 0);
  filler (buf, "..", NULL, 0);

  while ((file_info = g_file_enumerator_next_file (enumerator, NULL, &error)) != NULL)
    {
      filler (buf, g_file_info_get_name (file_info), NULL, 0);
      g_object_unref (file_info);
    }

  g_object_unref (enumerator);

  return 0;
}

static gint
vfs_readdir (const gchar *path, gpointer buf, fuse_fill_dir_t filler, off_t offset,
             struct fuse_file_info *fi)
{
  GFile       *base_file;
  gint         result = 0;

  debug_print ("vfs_readdir: %s\n", path);

  if (path_is_mount_list (path))
    {
      GList *l; 

      /* Mount list */

      filler (buf, ".", NULL, 0);
      filler (buf, "..", NULL, 0);

      mount_list_lock ();

      for (l = mount_list; l; l = g_list_next (l))
        {
          MountRecord *mount_record = l->data;

          filler (buf, mount_record->name, NULL, 0);
        }

      mount_list_unlock ();
    }
  else if ((base_file = file_from_full_path (path)))
    {
      /* Submount */

      result = readdir_for_file (base_file, buf, filler);

      g_object_unref (base_file);
    }
  else
    {
      /* Not found */

      result = -ENOENT;
    }

  return result;
}

static gint
vfs_rename (const gchar *old_path, const gchar *new_path)
{
  GFile  *old_file;
  GFile  *new_file;
  GError *error  = NULL;
  gint    result = 0;

  debug_print ("vfs_rename: %s -> %s\n", old_path, new_path);

  old_file = file_from_full_path (old_path);
  new_file = file_from_full_path (new_path);

  if (old_file && new_file)
    {
      FileHandle *fh = get_file_handle_for_path (old_path);

      if (fh)
        {
          g_mutex_lock (fh->mutex);
          file_handle_close_stream (fh);
        }

      g_file_move (old_file, new_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error);

      if (error)
        {
          debug_print ("vfs_rename failed: %s\n", error->message);

          result = -errno_from_error (error);
          g_error_free (error);
        }
      else
        {
          reindex_file_handle_for_path (old_path, new_path);
        }

      if (fh)
        {
          g_mutex_unlock (fh->mutex);
          file_handle_unref (fh);
        }

      if (result == -EISDIR)
        {
          /* This makes e.g. 'mv' fall back to a recursive copy + delete operation */
          result = -EXDEV;
        }
    }
  else
    {
      result = -ENOENT;
    }

  if (old_file)
    g_object_unref (old_file);
  if (new_file)
    g_object_unref (new_file);

  debug_print ("vfs_rename: -> %s\n", g_strerror (-result));

  return result;
}

static gint
vfs_unlink (const gchar *path)
{
  GFile  *file;
  GError *error  = NULL;
  gint    result = 0;

  debug_print ("vfs_unlink: %s\n", path);

  file = file_from_full_path (path);

  if (file)
    {
      FileHandle *fh = get_file_handle_for_path (path);

      if (fh)
        {
          g_mutex_lock (fh->mutex);
          file_handle_close_stream (fh);
        }

      g_file_delete (file, NULL, &error);

      if (fh)
        {
          g_mutex_unlock (fh->mutex);
          file_handle_unref (fh);
        }

      if (error)
        {
          debug_print ("vfs_unlink failed: %s (%s)\n", path, error->message);

          result = -errno_from_error (error);
          g_error_free (error);
        }

      g_object_unref (file);
    }
  else
    {
      result = -ENOENT;
    }

  debug_print ("vfs_unlink: -> %s\n", g_strerror (-result));

  return result;
}

static gint
vfs_mkdir (const gchar *path, mode_t mode)
{
  GFile  *file;
  GError *error  = NULL;
  gint    result = 0;

  debug_print ("vfs_mkdir: %s\n", path);

  file = file_from_full_path (path);

  if (file)
    {
      if (g_file_make_directory (file, NULL, &error))
        {
          /* Ignore errors setting the mode. We already created the directory, and that's
           * good enough. */
          g_file_set_attribute_uint32 (file, G_FILE_ATTRIBUTE_UNIX_MODE, mode, 0, NULL, NULL);
        }

      if (error)
        {
          result = -errno_from_error (error);
          g_error_free (error);
        }

      g_object_unref (file);
    }
  else
    {
      result = -ENOENT;
    }

  debug_print ("vfs_mkdir: -> %s\n", g_strerror (-result));

  return result;
}

static gint
vfs_rmdir (const gchar *path)
{
  GFile  *file;
  GError *error = NULL;
  gint   result = 0;

  debug_print ("vfs_rmdir: %s\n", path);

  file = file_from_full_path (path);

  if (file)
    {
      GFileInfo *file_info;

      file_info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_TYPE, 0, NULL, &error);
      if (file_info)
        {
          if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY)
            {
              g_file_delete (file, NULL, &error);

              if (error)
                {
                  result = -errno_from_error (error);
                  g_error_free (error);
                }
            }
          else
            {
              result = -ENOTDIR;
            }
        }
      else
        {
          if (error)
            {
              result = -errno_from_error (error);
              g_error_free (error);
            }
          else
            {
              result = -ENOENT;
            }
        }
    }
  else
    {
      result = -ENOENT;
    }

  debug_print ("vfs_rmdir: -> %s\n", g_strerror (-result));

  return result;
}

static gboolean
file_handle_get_size (FileHandle *fh,
		      goffset *size)
{
  GFileInfo *info;
  gboolean res;

  if (fh->stream == NULL)
    return FALSE;
  
  info = NULL;
  if (fh->op == FILE_OP_READ)
    info = g_file_input_stream_query_info (fh->stream,
					   G_FILE_ATTRIBUTE_STANDARD_SIZE,
					   NULL, NULL);
  else if (fh->op == FILE_OP_WRITE)
    info = g_file_output_stream_query_info (fh->stream,
					    G_FILE_ATTRIBUTE_STANDARD_SIZE,
					    NULL, NULL);

  res = FALSE;
  if (info)
    {
      if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
	{
	  *size = g_file_info_get_size (info);
	  res = TRUE;
	}
      g_object_unref (info);
    }
  return res;
}

static gint
vfs_ftruncate (const gchar *path, off_t size, struct fuse_file_info *fi)
{
  GFile  *file;
  GError *error  = NULL;
  gint    result = 0;
  goffset current_size;

  debug_print ("vfs_ftruncate: %s\n", path);

  file = file_from_full_path (path);

  if (file)
    {
      FileHandle *fh = get_file_handle_from_info (fi);

      if (fh)
        {
          g_mutex_lock (fh->mutex);

          result = setup_output_stream (file, fh);

          if (result == 0)
            {
              if (g_seekable_can_truncate (G_SEEKABLE (fh->stream)))
                {
                  g_seekable_truncate (fh->stream, size, NULL, &error);
                }
              else if (size == 0)
                {
                  g_output_stream_close (fh->stream, NULL, NULL);
                  g_object_unref (fh->stream);
                  fh->stream = NULL;

                  fh->stream = g_file_replace (file, 0, FALSE, 0, NULL, &error);

                  if (fh->stream != NULL)
                    {
                      /* The stream created by g_file_replace() won't always replace
                       * the file until it's been closed. So close it now to make
                       * future operations consistent. */
                      g_output_stream_close (fh->stream, NULL, NULL);
                      g_object_unref (fh->stream);
                      fh->stream = NULL;
                    }
                }
              else if (file_handle_get_size (fh, &current_size) &&
		       current_size == size)
		{
		  /* Don't have to do anything to succeed */
		}
	      else
		{
		  result = -ENOTSUP;
                }

              if (error)
                {
                  result = -errno_from_error (error);
                  g_error_free (error);
                }
            }

          g_mutex_unlock (fh->mutex);
          file_handle_unref (fh);
        }
      else
        {
          result = -EINVAL;
        }

      g_object_unref (file);
    }
  else
    {
      result = -ENOENT;
    }

  debug_print ("vfs_ftruncate: -> %s\n", g_strerror (-result));

  return result;
}

static gint
vfs_truncate (const gchar *path, off_t size)
{
  GFile  *file;
  GError *error  = NULL;
  gint    result = 0;

  debug_print ("vfs_truncate: %s\n", path);

  file = file_from_full_path (path);

  if (file)
    {
      GFileOutputStream *file_output_stream = NULL;
      FileHandle        *fh;

      /* Get a file handle just to lock the path while we're working */
      fh = get_file_handle_for_path (path);
      if (fh)
        g_mutex_lock (fh->mutex);

      if (size == 0)
        {
          file_output_stream = g_file_replace (file, 0, FALSE, 0, NULL, &error);
        }
      else
        {
          file_output_stream = g_file_append_to (file, 0, NULL, &error);
          if (file_output_stream)
              g_seekable_truncate (G_SEEKABLE (file_output_stream), size, NULL, &error);
        }

      if (error)
        {
          result = -errno_from_error (error);
          g_error_free (error);
        }

      if (file_output_stream)
        {
          g_output_stream_close (G_OUTPUT_STREAM (file_output_stream), NULL, NULL);
          g_object_unref (file_output_stream);
        }

      if (fh)
        {
          g_mutex_unlock (fh->mutex);
          file_handle_unref (fh);
        }

      g_object_unref (file);
    }
  else
    {
      result = -ENOENT;
    }

  debug_print ("vfs_truncate: -> %s\n", g_strerror (-result));

  return result;
}

static gint
vfs_symlink (const gchar *path_old, const gchar *path_new)
{
  GFile  *file;
  GError *error  = NULL;
  gint    result = 0;

  debug_print ("vfs_symlink: %s -> %s\n", path_new, path_old);

  file = file_from_full_path (path_new);

  if (file)
    {
      g_file_make_symbolic_link (file, path_old, NULL, &error);

      if (error)
        {
          result = -errno_from_error (error);
          g_error_free (error);
        }
    }
  else
    {
      result = -ENOENT;
    }

  debug_print ("vfs_symlink: -> %s\n", g_strerror (-result));

  return result;
}

static gint
vfs_access (const gchar *path, gint mode)
{
  GFile  *file;
  GError *error  = NULL;
  gint    result = 0;

  debug_print ("vfs_access: %s\n", path);

  file = file_from_full_path (path);

  if (file)
    {
      GFileInfo *file_info;

      file_info = g_file_query_info (file,
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_READ ","
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE ","
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE ",",
                                     0, NULL, &error);
      if (file_info)
        {
          if ((mode & R_OK && (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ) &&
                               !g_file_info_get_attribute_boolean (file_info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ))) ||
              (mode & W_OK && (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE) &&
                               !g_file_info_get_attribute_boolean (file_info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))) ||
              (mode & X_OK && (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE) &&
                               !g_file_info_get_attribute_boolean (file_info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE))))
            result = -EACCES;

          g_object_unref (file_info);
        }
      else if (error)
        {
          result = -errno_from_error (error);
          g_error_free (error);
        }
      else
        {
          result = -EIO;
        }

      g_object_unref (file);
    }
  else if (path_is_mount_list (path))
    {
      result = 0;
    }
  else
    {
      result = -ENOENT;
    }

  debug_print ("vfs_access: -> %s\n", g_strerror (-result));
  return result;
}

static gint
vfs_utimens (const gchar *path, const struct timespec tv [2])
{
  GFile  *file;
  GError *error  = NULL;
  gint    result = 0;

  debug_print ("vfs_utimens: %s\n", path);

  file = file_from_full_path (path);

  if (file)
    {
      guint64 atime;
      guint32 atime_usec;
      guint64 mtime;
      guint32 mtime_usec;
      GFileInfo *info;

      if (tv)
        {
          atime = (guint64) tv [0].tv_sec;
          atime_usec = (guint32) tv [0].tv_nsec / (guint32) 1000;
          mtime = (guint64) tv [1].tv_sec;
          mtime_usec = (guint32) tv [1].tv_nsec / (guint32) 1000;
        }
      else
        {
          struct timeval tiv;

          gettimeofday (&tiv, NULL);
          atime = (guint64) tiv.tv_sec;
          atime_usec = (guint32) tiv.tv_usec;
          mtime = atime;
          mtime_usec = atime_usec;
        }

      info = g_file_info_new ();
      g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, mtime);
      g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, mtime_usec);
      g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS, atime);
      g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_ACCESS_USEC, atime_usec);

      g_file_set_attributes_from_info (file, info, 0, NULL, &error);

      if (error)
        {
          /* As long as not all backends support all attributes we set,
             report failure only if neither mtime and atime have been set. */
          if (g_file_info_get_attribute_status (info, G_FILE_ATTRIBUTE_TIME_ACCESS) == G_FILE_ATTRIBUTE_STATUS_ERROR_SETTING &&
              g_file_info_get_attribute_status (info, G_FILE_ATTRIBUTE_TIME_MODIFIED) == G_FILE_ATTRIBUTE_STATUS_ERROR_SETTING)
            {
              /* Note: we only get first error from the attributes we try to set,  might not be accurate
                       (a limitation of g_file_set_attributes_from_info()). */
              result = -errno_from_error (error);
            }
          g_error_free (error);
        }

      g_object_unref (file);
      g_object_unref (info);
    }
  else if (path_is_mount_list (path))
    {
      /* */
    }
  else
    {
      result = -ENOENT;
    }

  debug_print ("vfs_utimens: -> %s\n", g_strerror (-result));
  return result;
}

static gint
vfs_chmod (const gchar *path, mode_t mode)
{
  GFile  *file;
  GError *error  = NULL;
  gint    result = 0;

  file = file_from_full_path (path);

  if (file)
    {
      g_file_set_attribute_uint32 (file, G_FILE_ATTRIBUTE_UNIX_MODE, mode, 0, NULL, &error);

      if (error)
        {
          result = -errno_from_error (error);
          g_error_free (error);
        }

      g_object_unref (file);
    }

  return result;
}

static void
mount_tracker_mounted_cb (GVolumeMonitor *volume_monitor,
                          GMount         *mount)
{
  MountRecord *mount_record;

  if (mount_record_for_mount_exists (mount))
    return;

  mount_record = mount_record_new (mount);

  mount_list_lock ();
  mount_list = g_list_prepend (mount_list, mount_record);
  mount_list_unlock ();
}

static void
mount_tracker_unmounted_cb (GVolumeMonitor *volume_monitor,
                            GMount         *mount)
{
  GFile *root;
  GList *l;

  root = g_mount_get_root (mount);

  mount_list_lock ();

  for (l = mount_list; l != NULL; l = l->next)
    {
      MountRecord *mount_record = l->data;

      if (g_file_equal (root, mount_record->root))
        {
          mount_list = g_list_delete_link (mount_list, l);
          mount_record_free (mount_record);
          break;
        }
    }

  mount_list_unlock ();

  g_object_unref (root);
}

static gpointer
subthread_main (gpointer data)
{
  
  mount_list_update ();
  
  g_signal_connect (volume_monitor, "mount_added", (GCallback) mount_tracker_mounted_cb, NULL);
  g_signal_connect (volume_monitor, "mount_removed", (GCallback) mount_tracker_unmounted_cb, NULL);

  g_main_loop_run (subthread_main_loop);

  g_signal_handlers_disconnect_by_func (volume_monitor, mount_tracker_mounted_cb, NULL);
  g_signal_handlers_disconnect_by_func (volume_monitor, mount_tracker_unmounted_cb, NULL);

  g_main_loop_unref (subthread_main_loop);
  subthread_main_loop = NULL;

  g_object_unref (volume_monitor);
  volume_monitor = NULL;

  /* Tell the main thread to unmount. Using kill() is necessary according to FUSE maintainers. */
  kill (getpid (), SIGHUP);

  return NULL;
}

static DBusHandlerResult
dbus_filter_func (DBusConnection *connection,
                  DBusMessage    *message,
                  void           *data)
{
	if (dbus_message_is_signal (message,
                              DBUS_INTERFACE_DBUS,
                              "NameOwnerChanged"))
    {
      const char *service, *old_owner, *new_owner;
      
      dbus_message_get_args (message,
                             NULL,
                             DBUS_TYPE_STRING, &service,
                             DBUS_TYPE_STRING, &old_owner,
                             DBUS_TYPE_STRING, &new_owner,
                             DBUS_TYPE_INVALID);
      
      /* Handle monitor owner going away */
      if (service != NULL &&
          strcmp (G_VFS_DBUS_DAEMON_NAME, service) == 0 &&
          *new_owner == 0)
        {
          /* The daemon died, unmount */
          g_main_loop_quit (subthread_main_loop);
        }
    }
  else if (dbus_message_is_signal (message,
                                   DBUS_INTERFACE_LOCAL,
                                   "Disconnected"))
    {
      /* Session bus died, unmount */
      g_main_loop_quit (subthread_main_loop);
    }
	
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static gpointer
vfs_init (struct fuse_conn_info *conn)
{
	DBusConnection *dbus_conn;
  DBusMessage *message;
	DBusError error;
  
  daemon_creation_time = time (NULL);
  daemon_uid = getuid ();
  daemon_gid = getgid ();

  mount_list_mutex = g_mutex_new ();
  global_path_to_fh_map = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 NULL, (GDestroyNotify) file_handle_free);
  global_active_fh_map = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                NULL, NULL);

	dbus_error_init (&error);

	dbus_conn = dbus_bus_get (DBUS_BUS_SESSION, &error);
  if (dbus_conn == NULL)
    {
      g_printerr ("Failed to connect to the D-BUS daemon: %s\n",
                  error.message);
      dbus_error_free (&error);
      return NULL;
    }

  dbus_connection_set_exit_on_disconnect (dbus_conn, FALSE);

  _g_dbus_connection_integrate_with_main (dbus_conn);

	dbus_bus_add_match (dbus_conn,
                      "type='signal',sender='" DBUS_SERVICE_DBUS "',"
                      "interface='" DBUS_INTERFACE_DBUS "',"
                      "member='NameOwnerChanged',"
                      "arg0='"G_VFS_DBUS_DAEMON_NAME"'",
                      NULL);
	dbus_connection_add_filter (dbus_conn,
                              dbus_filter_func,
                              NULL,
                              NULL);

  message = dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
                                          G_VFS_DBUS_MOUNTTRACKER_PATH,
                                          G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
                                          G_VFS_DBUS_MOUNTTRACKER_OP_REGISTER_FUSE);
  dbus_message_set_auto_start (message, TRUE);
  dbus_connection_send (dbus_conn, message, NULL);
  dbus_connection_flush (dbus_conn);
  
  gvfs = g_vfs_get_default ();
  
  volume_monitor = g_object_new (g_type_from_name ("GDaemonVolumeMonitor"), NULL);
  
  subthread_main_loop = g_main_loop_new (NULL, FALSE);
  subthread = g_thread_create ((GThreadFunc) subthread_main, NULL, FALSE, NULL);

  return NULL;
}

static void
vfs_destroy (gpointer param)
{
  mount_list_free ();
  if (subthread_main_loop != NULL) 
    g_main_loop_quit (subthread_main_loop);
  g_mutex_free (mount_list_mutex);
  g_object_unref (gvfs);
}

static struct fuse_operations vfs_oper =
{
  .init        = vfs_init,
  .destroy     = vfs_destroy,

  .getattr     = vfs_getattr,
  .readdir     = vfs_readdir,

  .statfs      = vfs_statfs,

  .opendir     = vfs_opendir,
  .readdir     = vfs_readdir,
  .readlink    = vfs_readlink,

  .open        = vfs_open,
  .create      = vfs_create,
  .release     = vfs_release,
  .flush       = vfs_flush,
  .fsync       = vfs_fsync,

  .read        = vfs_read,
  .write       = vfs_write,

  .rename      = vfs_rename,
  .unlink      = vfs_unlink,
  .mkdir       = vfs_mkdir,
  .rmdir       = vfs_rmdir,
  .ftruncate   = vfs_ftruncate,
  .truncate    = vfs_truncate,
  .symlink     = vfs_symlink,
  .access      = vfs_access,
  .utimens     = vfs_utimens,
  .chmod       = vfs_chmod,

#if 0
  .chown       = vfs_chown,
  .setxattr    = vfs_setxattr,
  .getxattr    = vfs_getxattr,
  .listxattr   = vfs_listxattr,
  .removexattr = vfs_removexattr,
#endif
};

gint
main (gint argc, gchar *argv [])
{
  g_type_init ();
  g_thread_init (NULL);

  return fuse_main (argc, argv, &vfs_oper, NULL /* user data */);
}
