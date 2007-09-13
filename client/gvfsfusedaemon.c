/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include <config.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/vfs.h>
#include <errno.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gprintf.h>

#include "gdaemonvfs.h"
#include "gdaemonfile.h"
#include <gmounttracker.h>
#include <gio/gseekable.h>
#include <gio/gioerror.h>

#define FUSE_USE_VERSION 26
#include <fuse.h>

#define DEBUG_ENABLED 1

#define GET_FILE_HANDLE(fi)     (GUINT_TO_POINTER ((guint) (fi)->fh))
#define SET_FILE_HANDLE(fi, fh) ((fi)->fh = (guint64) GPOINTER_TO_UINT (fh))

typedef struct
{
  time_t      creation_time;
  GMountInfo *info;
}
MountRecord;

typedef enum
{
  FILE_OP_NONE,
  FILE_OP_READ,
  FILE_OP_WRITE
}
FileOp;

typedef struct
{
  GMutex   *mutex;
  FileOp    op;
  gpointer  stream;
  gint      length;
  size_t    pos;
}
FileHandle;

static GThread       *subthread           = NULL;
static GMainLoop     *subthread_main_loop = NULL;
static GVfs          *gvfs                = NULL;

static GMountTracker *mount_tracker       = NULL;

/* Contains pointers to MountRecord */
static GList         *mount_list          = NULL;
static GMutex        *mount_list_mutex;

static time_t         daemon_creation_time;
static uid_t          daemon_uid;
static gid_t          daemon_gid;

/* ------- *
 * Helpers *
 * ------- */

static void
debug_print (const gchar *message, ...)
{
#ifdef DEBUG_ENABLED

  static FILE *debug_fhd = NULL;
  va_list      var_args;

  if (!debug_fhd)
    debug_fhd = fopen ("/home/hpj/vfs.debug", "at");

  if (!debug_fhd)
    return;

  va_start (var_args, message);
  g_vfprintf (debug_fhd, message, var_args);
  va_end (var_args);

  fflush (debug_fhd);

#endif
}

typedef struct
{
  gint gioerror;
  gint errno_value;
}
ErrorMap;

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
      { G_IO_ERROR_NOT_MOUNTABLE,      EIO          },
      { G_IO_ERROR_FILENAME_TOO_LONG,  ENAMETOOLONG },
      { G_IO_ERROR_INVALID_FILENAME,   EIO          },
      { G_IO_ERROR_TOO_MANY_LINKS,     ELOOP        },
      { G_IO_ERROR_NO_SPACE,           ENOSPC       },
      { G_IO_ERROR_INVALID_ARGUMENT,   EINVAL       },
      { G_IO_ERROR_PERMISSION_DENIED,  EACCES       },
      { G_IO_ERROR_NOT_SUPPORTED,      EIO          },
      { G_IO_ERROR_NOT_MOUNTED,        EIO          },
      { G_IO_ERROR_ALREADY_MOUNTED,    EIO          },
      { G_IO_ERROR_CLOSED,             EIO          },
      { G_IO_ERROR_CANCELLED,          EIO          },
      { G_IO_ERROR_PENDING,            EIO          },
      { G_IO_ERROR_READ_ONLY,          EACCES       },
      { G_IO_ERROR_CANT_CREATE_BACKUP, EIO          },
      { G_IO_ERROR_WRONG_MTIME,        EIO          },
      { G_IO_ERROR_TIMED_OUT,          EIO          },
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

static FileHandle *
file_handle_new (void)
{
  FileHandle *file_handle;

  file_handle = g_new0 (FileHandle, 1);
  file_handle->mutex = g_mutex_new ();

  return file_handle;
}

static void
file_handle_free (FileHandle *file_handle)
{
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
    }

  g_mutex_free (file_handle->mutex);
  g_free (file_handle);
}

static MountRecord *
mount_record_new (GMountInfo *mount_info)
{
  MountRecord *mount_record;

  mount_record = g_new (MountRecord, 1);

  mount_record->info          = mount_info;
  mount_record->creation_time = time (NULL);

  return mount_record;
}

static void
mount_record_free (MountRecord *mount_record)
{
  g_mount_info_free (mount_record->info);
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

#if 0

static void
mount_list_update (void)
{
  GList *tracker_list;
  GList *l;

  tracker_list = g_mount_tracker_list_mounts (mount_tracker);

  for (l = tracker_list; l; l = g_list_next (l))
    {
      GMountInfo *this_mount_info = l->data;

      if (!mount_record_find_by_mount_spec (this_mount_info->mount_spec))
        {
          mount_list_lock ();
          mount_list = g_list_prepend (mount_list, mount_record_new (this_mount_info));
          mount_list_unlock ();
        }
      else
        {
          g_mount_info_free (this_mount_info);
        }
    }

  g_list_free (tracker_list);
}

#endif

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
  return strcmp (path, "/") ? FALSE : TRUE;
}

static gchar
nibble_to_ascii (guint value)
{
  gchar c;

  g_assert (value < 16);

  if (value < 10)
    c = '0' + value;
  else
    c = 'a' + value - 10;

  return c;
}

static gchar *
escape_to_uri_syntax (const gchar *string, const gchar *allowed_characters)
{
  gchar *escaped_string;
  gint   i;
  gint   j = 0;

  if (!string)
    return NULL;

  if (!allowed_characters)
    allowed_characters = "";

  escaped_string = g_malloc (strlen (string) * 3 + 1);

  for (i = 0; string [i]; i++)
  {
    guchar c = string [i];

    if (strchr (allowed_characters, c))
    {
      escaped_string [j++] = c;
      continue;
    }

    escaped_string [j++] = '%';
    escaped_string [j++] = nibble_to_ascii (c >> 4);
    escaped_string [j++] = nibble_to_ascii (c & 0x0f);
  }

  escaped_string [j] = '\0';
  escaped_string = g_realloc (escaped_string, j + 1);

  return escaped_string;
}

static gchar *
escape_fs_name (const gchar *name)
{
  return escape_to_uri_syntax (name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-+@#$., ");
}

#if 0

static gchar *
escape_uri_component (const gchar *uri_component)
{
  return escape_to_uri_syntax (uri_component, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_-+., ");
}

#endif

static MountRecord *
mount_record_find_by_mount_spec (GMountSpec *mount_spec)
{
  MountRecord *mount_record = NULL;
  GList       *l;

  g_assert (mount_spec != NULL);

  mount_list_lock ();

  for (l = mount_list; !mount_record && l; l = g_list_next (l))
    {
      MountRecord *this_mount_record = l->data;
      GMountSpec  *this_mount_spec   = this_mount_record->info->mount_spec;

      if (g_mount_spec_equal (mount_spec, this_mount_spec))
        mount_record = this_mount_record;
    }

  mount_list_unlock ();

  return mount_record;
}

static gchar *
mount_info_to_mount_name (GMountInfo *mount_info)
{
  g_assert (mount_info != NULL);

  return escape_fs_name (mount_info->display_name);
}

static MountRecord *
mount_record_find_by_mount_name (const gchar *mount_name)
{
  MountRecord *mount_record = NULL;
  GList       *l;

  g_assert (mount_name != NULL);

  mount_list_lock ();

  for (l = mount_list; !mount_record && l; l = g_list_next (l))
    {
      MountRecord *this_mount_record = l->data;
      GMountInfo  *this_mount_info   = this_mount_record->info;
      gchar       *this_mount_name   = mount_info_to_mount_name (this_mount_info);

      g_assert (this_mount_name != NULL);

      if (!strcmp (mount_name, this_mount_name))
        mount_record = this_mount_record;

      g_free (this_mount_name);
    }

  mount_list_unlock ();

  return mount_record;
}

#if 0

static gchar *
mount_spec_to_uri (GMountSpec *mount_spec)
{
  const gchar *type;
  gchar       *uri = NULL;

  type = g_mount_spec_get (mount_spec, "type");
  if (!type)
    return NULL;

  if (!strcmp (type, "smb-share"))
    {
      gchar *server = escape_uri_component (g_mount_spec_get (mount_spec, "server"));
      gchar *share  = escape_uri_component (g_mount_spec_get (mount_spec, "share"));
      gchar *domain = escape_uri_component (g_mount_spec_get (mount_spec, "domain"));
      gchar *user   = escape_uri_component (g_mount_spec_get (mount_spec, "user"));

      if (server && share)
        {
          uri = g_strdup_printf ("smb://%s%s%s%s%s/%s",
                                 domain ? domain : "",
                                 domain ? ";"    : "",
                                 user   ? user   : "",
                                 user   ? "@"    : "",
                                 server,
                                 share);
        }

      g_free (server);
      g_free (share);
      g_free (domain);
      g_free (user);
    }

  return uri;
}

#endif

static gboolean
path_to_mount_record_and_path (const gchar *full_path, MountRecord **mount_record, gchar **mount_path)
{
  MountRecord *mount_record_internal;
  gchar       *mount_path_internal = NULL;
  gchar       *mount_name;
  const gchar *s1;
  const gchar *s2;
  gboolean    success = FALSE;

  s1 = full_path;
  if (*s1 == '/')
    s1++;

  if (*s1)
    {
      s2 = strchr (s1, '/');
      if (!s2)
        s2 = s1 + strlen (s1);

      mount_name = g_strndup (s1, s2 - s1);
      mount_record_internal = mount_record_find_by_mount_name (mount_name);
      g_free (mount_name);

      if (mount_record_internal)
        {
          if (mount_record)
            *mount_record = mount_record_internal;

          if (*s2)
            {
              /* s2 is at the initial '/' of the mount subpath */
              mount_path_internal = g_strdup (s2);
            }
          else
            {
              /* No subpath specified; we want the mount's root */
              mount_path_internal = g_strdup ("/");
            }

          if (mount_path)
            *mount_path = mount_path_internal;

          success = TRUE;
        }
    }

  return success;
}

static GFile *
file_from_mount_record_and_path (MountRecord *mount_record, const gchar *path)
{
#if 0
  gchar *mount_uri;
  gchar *base_uri;
  gchar *escaped_path;
#endif
  GFile *file;

  file = g_daemon_file_new (mount_record->info->mount_spec, path);

#if 0
  mount_uri = mount_spec_to_uri (mount_record->info->mount_spec);
  escaped_path = escape_uri_component (path);
  base_uri = g_strconcat (mount_uri, escaped_path, NULL);
  file = g_file_get_for_uri (base_uri);

  g_free (mount_uri);
  g_free (escaped_path);
  g_free (base_uri);
#endif

  g_assert (file != NULL);

  return file;
}

static GFile *
file_from_full_path (const gchar *path)
{
  MountRecord *mount_record;
  gchar       *mount_path;
  GFile       *file = NULL;

  if (path_to_mount_record_and_path (path, &mount_record, &mount_path))
    {
      file = file_from_mount_record_and_path (mount_record, mount_path);
      g_free (mount_path);
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

          if (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_FS_SIZE))
            stbuf->f_blocks = g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_FS_SIZE) / 4096;
          if (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_FS_FREE))
            stbuf->f_bfree = stbuf->f_bavail = g_file_info_get_attribute_uint64 (file_info, G_FILE_ATTRIBUTE_FS_FREE) / 4096;
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

  return 0;
}

#endif

static gint
vfs_statfs (const gchar *path, struct statvfs *stbuf)
{
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

  if (statvfs ("/", stbuf) != 0)
    return -errno;

  return 0;
}

static mode_t
file_info_get_stat_mode (GFileInfo *file_info)
{
  GFileType         file_type;
  mode_t            unix_mode;

  file_type   = g_file_info_get_file_type (file_info);

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

  if (!g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_ACCESS_READ)
      || g_file_info_get_attribute_uint32 (file_info, G_FILE_ATTRIBUTE_ACCESS_READ))
    unix_mode |= S_IRUSR;
  if (!g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_ACCESS_WRITE)
      || g_file_info_get_attribute_uint32 (file_info, G_FILE_ATTRIBUTE_ACCESS_WRITE))
    unix_mode |= S_IWUSR;
  if (!g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_ACCESS_EXECUTE)
      || g_file_info_get_attribute_uint32 (file_info, G_FILE_ATTRIBUTE_ACCESS_EXECUTE))
    unix_mode |= S_IXUSR;

  return unix_mode;
}

static gint
getattr_for_file (GFile *file, struct stat *sbuf)
{
  GFileInfo *file_info;
  GError    *error  = NULL;
  gint       result = 0;

  file_info = g_file_get_info (file, "*", 0, NULL, &error);

  if (file_info)
    {
      GTimeVal mod_time;

      sbuf->st_mode = file_info_get_stat_mode (file_info);
      sbuf->st_size = g_file_info_get_size (file_info);
      sbuf->st_uid = daemon_uid;
      sbuf->st_gid = daemon_gid;

      if (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_UNIX_UID))
        sbuf->st_uid = file_info_get_attribute_as_uint (file_info, G_FILE_ATTRIBUTE_UNIX_UID);
      if (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_UNIX_GID))
        sbuf->st_gid = file_info_get_attribute_as_uint (file_info, G_FILE_ATTRIBUTE_UNIX_GID);

      g_file_info_get_modification_time (file_info, &mod_time);
      sbuf->st_mtime = mod_time.tv_sec;
      sbuf->st_ctime = mod_time.tv_sec;
      sbuf->st_atime = mod_time.tv_sec;

      if (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_TIME_CHANGED))
        sbuf->st_ctime = file_info_get_attribute_as_uint (file_info, G_FILE_ATTRIBUTE_TIME_CHANGED);
      if (g_file_info_has_attribute (file_info, G_FILE_ATTRIBUTE_TIME_ACCESS))
        sbuf->st_atime = file_info_get_attribute_as_uint (file_info, G_FILE_ATTRIBUTE_TIME_ACCESS);

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
  GFile *file;
  gint   result = 0;

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

      sbuf->st_mode = S_IFDIR | 0555;                   /* mode_t    protection */
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

  return result;
}

static gint
vfs_readlink (const gchar *path, gchar *target, size_t size)
{
  debug_print ("vfs_readlink: %s\n", path);

  return 0;
}

static gint
vfs_open (const gchar *path, struct fuse_file_info *fi)
{
  GFile *file;
  gint   result = 0;

  debug_print ("vfs_open: %s\n", path);

  if ((file = file_from_full_path (path)))
    {
      GFileInfo *file_info;
      GError    *error = NULL;

      file_info = g_file_get_info (file, "*", 0, NULL, &error);

      if (file_info)
        {
          GFileType file_type = g_file_info_get_file_type (file_info);

          if (file_type == G_FILE_TYPE_REGULAR)
            {
              FileHandle *fh = file_handle_new ();
              
              /* Success */

              SET_FILE_HANDLE (fi, fh);
              fh->op = FILE_OP_NONE;

              /* TODO: Cache the GFile for performance? */
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

  return result;
}

static gint
vfs_create (const gchar *path, mode_t mode, struct fuse_file_info *fi)
{
  GFile *file;
  gint   result = 0;

  debug_print ("vfs_create: %s\n", path);

  if ((file = file_from_full_path (path)))
    {
      GFileInfo *file_info;
      GError    *error = NULL;

      file_info = g_file_get_info (file, "*", 0, NULL, &error);

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

          file_output_stream = g_file_create (file, NULL, &error);
          if (file_output_stream)
            {
              FileHandle *fh = file_handle_new ();

              /* Success */

              SET_FILE_HANDLE (fi, fh);
              fh->stream = file_output_stream;
              fh->op = FILE_OP_WRITE;
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

  return result;
}

static gint
vfs_release (const gchar *path, struct fuse_file_info *fi)
{
  FileHandle *fh = GET_FILE_HANDLE (fi);

  debug_print ("vfs_release: %s\n", path);

  if (fh)
    file_handle_free (fh);

  return 0;
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

  fh->op = FILE_OP_READ;

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
      return -EIO;
#if 0
      fh->stream = g_file_read (file, NULL, &error);
      fh->pos = 0;
#endif
    }

  fh->op = FILE_OP_WRITE;

  if (error)
    {
      result = -errno_from_error (error);
      g_error_free (error);
    }

  return result;
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
      if (G_IS_SEEKABLE (input_stream))
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
          /* Can't seek, can't skip */

          debug_print ("read_stream: can't seek nor skip to offset %d!\n", offset);

          result = -EIO;  /* TODO: Better error code? */
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

  if ((file = file_from_full_path (path)))
    {
      FileHandle *fh = GET_FILE_HANDLE (fi);

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

      g_object_unref (file);
      g_mutex_unlock (fh->mutex);
    }
  else
    {
      result = -EIO;
    }

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
      if (G_IS_SEEKABLE (output_stream))
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
      FileHandle *fh = GET_FILE_HANDLE (fi);

      g_mutex_lock (fh->mutex);

      result = setup_output_stream (file, fh);
      if (result == 0)
        {
          result = write_stream (fh, buf, len, offset);
        }

      g_object_unref (file);
      g_mutex_unlock (fh->mutex);
    }
  else
    {
      result = -EIO;
    }

  return result;
}

static gint
vfs_flush (const gchar *path, struct fuse_file_info *fi)
{
  debug_print ("vfs_flush: %s\n", path);

  return 0;
};

static gint
vfs_opendir (const gchar *path, struct fuse_file_info *fi)
{
  MountRecord *mount_record;
  gchar       *mount_path;
  gint         result = 0;

  debug_print ("vfs_opendir: %s\n", path);

  if (path_is_mount_list (path))
    {
      /* Mount list */
    }
  else if (path_to_mount_record_and_path (path, &mount_record, &mount_path))
    {
      /* Submount */

      /* TODO */

      g_free (mount_path);
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

  enumerator = g_file_enumerate_children (base_file, G_FILE_ATTRIBUTE_STD_NAME, 0, NULL, &error);
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
          MountRecord *this_mount_record = l->data;
          gchar       *mount_name;

          mount_name = mount_info_to_mount_name (this_mount_record->info);
          filler (buf, mount_name, NULL, 0);
          g_free (mount_name);
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
      g_file_move (old_file, new_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error);

      if (error)
        {
          debug_print ("vfs_rename failed: %s\n", error->message);

          result = -errno_from_error (error);
          g_error_free (error);
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
      g_file_delete (file, NULL, &error);

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

  return result;
}

static gint
vfs_mkdir (const gchar *path, mode_t mode)
{
  GFile  *file;
  GError *error  = NULL;
  gint    result = 0;

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

  return result;
}

static gint
vfs_rmdir (const gchar *path)
{
  GFile  *file;
  GError *error = NULL;
  gint   result = 0;

  file = file_from_full_path (path);

  if (file)
    {
      GFileInfo *file_info;

      file_info = g_file_get_info (file, "*", 0, NULL, &error);
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

  return result;
}

static gint
vfs_truncate (const gchar *path, off_t size)
{
  GFile  *file;
  GError *error  = NULL;
  gint    result = 0;

  return -EIO;
}

static void
mount_tracker_mounted_cb (GMountTracker *tracer, GMountInfo *mount_info)
{
  MountRecord *mount_record;

  mount_record = mount_record_find_by_mount_spec (mount_info->mount_spec);
  g_assert (mount_record == NULL);

  mount_record = mount_record_new (g_mount_info_dup (mount_info));

  mount_list_lock ();
  mount_list = g_list_prepend (mount_list, mount_record);
  mount_list_unlock ();
}

static void
mount_tracker_unmounted_cb (GMountTracker *tracer, GMountInfo *mount_info)
{
  MountRecord *mount_record;

  mount_record = mount_record_find_by_mount_spec (mount_info->mount_spec);
  g_assert (mount_record != NULL);

  mount_list_lock ();
  mount_list = g_list_remove (mount_list, mount_record);
  mount_list_unlock ();

  mount_record_free (mount_record);
}

static gpointer
subthread_main (gpointer data)
{
  g_signal_connect (mount_tracker, "mounted", (GCallback) mount_tracker_mounted_cb, NULL);
  g_signal_connect (mount_tracker, "unmounted", (GCallback) mount_tracker_unmounted_cb, NULL);

  g_main_loop_run (subthread_main_loop);

  g_signal_handlers_disconnect_by_func (mount_tracker, mount_tracker_mounted_cb, NULL);
  g_signal_handlers_disconnect_by_func (mount_tracker, mount_tracker_unmounted_cb, NULL);

  g_main_loop_unref (subthread_main_loop);

  g_object_unref (mount_tracker);
  mount_tracker = NULL;

  return NULL;
}

static gpointer
vfs_init (struct fuse_conn_info *conn)
{
  daemon_creation_time = time (NULL);
  daemon_uid = getuid ();
  daemon_gid = getgid ();

  mount_list_mutex = g_mutex_new ();

  /* Initializes D-Bus and other VFS necessities */
  gvfs = G_VFS (g_daemon_vfs_new ());

  mount_tracker = g_mount_tracker_new ();

  subthread_main_loop = g_main_loop_new (NULL, FALSE);
  subthread = g_thread_create ((GThreadFunc) subthread_main, NULL, FALSE, NULL);

  return NULL;
}

static void
vfs_destroy (gpointer param)
{
  mount_list_free ();
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
#if 0
  .releasedir  = vfs_releasedir,
  .fsyncdir    = vfs_fsyncdir,
#endif
  .readlink    = vfs_readlink,

  .open        = vfs_open,
  .create      = vfs_create,
  .release     = vfs_release,
  .flush       = vfs_flush,

  .read        = vfs_read,
  .write       = vfs_write,

  .rename      = vfs_rename,
  .unlink      = vfs_unlink,
  .mkdir       = vfs_mkdir,
  .rmdir       = vfs_rmdir,
  .truncate    = vfs_truncate,

#if 0
  .mknod       = vfs_mknod,
  .symlink     = vfs_symlink,
  .chmod       = vfs_chmod,
  .chown       = vfs_chown,
  .utime       = vfs_utime,
  .fsync       = vfs_fsync,
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













#if 0

#define MOUNT_NAME_SEPARATOR_S ":"

static gchar *
generate_options_string (const gchar *first_option_to_omit, ...)
{
  va_list vargs;

  va_start (first_option_to_omit, &vargs);

  va_end ();
}

static gchar *
mount_spec_to_mount_name (GMountSpec *mount_spec)
{
  const gchar *type;

  GDecodedUri *decoded_uri;
  gchar       *mount_name;
  gchar       *temp;

  type = g_mount_spec_get (mount_spec, "type");
  if (!type)
    return NULL;

  if (!strcmp (type, "smb-share"))
    {
      const gchar *server;
      const gchar *share;

      server = g_mount_spec_get (mount_spec, "server");
      share = g_mount_spec_get (mount_spec, "share");

      if (server && share)
        {
          mount_name = g_strjoin (MOUNT_NAME_SEPARATOR_S,
                                  type,
                                  server,
                                  share,
                                  NULL);
        }
    }







  /* TODO: Escape all strings */

  decoded_uri = _g_decode_uri (uri);
  g_assert (decoded_uri != NULL);

  mount_name = g_strjoin (":",
                          decoded_uri->scheme,
                          decoded_uri->host,
                          decoded_uri->path,
                          NULL);

  if (decoded_uri->query)
  {
    temp = g_strjoin ("?",
                      mount_name,
                      decoded_uri->query,
                      NULL);

    g_free (mount_name);
    mount_name = temp;
  }

  if (decoded_uri->fragment)
  {
    temp = g_strjoin ("#",
                      mount_name,
                      decoded_uri->fragment,
                      NULL);

    g_free (mount_name);
    mount_name = temp;
  }

  if (decoded_uri->port != -1)
  {
    gchar *port_opt_str;

    port_opt_str = g_strdup_printf ("port=%d", decoded_uri->port);

    temp = g_strjoin (":",
                      mount_name,
                      port_opt_str,
                      NULL);

    g_free (mount_name);
    mount_name = temp;
  }

  if (decoded_uri->userinfo)
  {
    gchar *user_opt_str;

    user_opt_str = g_strdup_printf ("user=%s", decoded_uri->userinfo);

    temp = g_strjoin (":",
                      mount_name,
                      user_opt_str,
                      NULL);

    g_free (mount_name);
    mount_name = temp;
  }

  return mount_name;
}

#endif

