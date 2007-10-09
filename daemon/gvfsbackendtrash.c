/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gioerror.h>
#include <gio/gfile.h>
#include <gio/gthemedicon.h>
#include <gio/gunixmounts.h>

#include "gvfsuriutils.h"

#include "gvfsbackendtrash.h"
#include "gvfsmonitor.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobdelete.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsdaemonprotocol.h"


struct _GVfsBackendTrash
{
  GVfsBackend parent_instance;

  GMountSpec *mount_spec;
};


G_DEFINE_TYPE (GVfsBackendTrash, g_vfs_backend_trash, G_VFS_TYPE_BACKEND);

static char *
escape_pathname (const char *dir)
{
  const char *p;
  char *d, *res;
  int count;
  char c;
  const char *basename;
  const char *user_data_dir;

  /* Special case the homedir trash to get nice filenames for that */
  user_data_dir = g_get_user_data_dir ();
  if (g_str_has_prefix (dir, user_data_dir) &&
      (g_str_has_prefix (dir + strlen (user_data_dir), "/Trash/")))
    {
      basename = dir + strlen (user_data_dir) + strlen ("/Trash/");

      res = g_malloc (strlen (basename) + 2);
      res[0] = '_';
      strcpy (res + 1, basename);
      return res;
    }

  /* Skip initial slashes, we don't need those since they are always there */
  while (*dir == '/')
    dir++;

  /* Underscores are doubled, count them */
  count = 0;
  p = dir;
  while (*p)
    {
      if (*p == '_')
        count++;
      p++;
    }
  
  res = g_malloc (strlen (dir) + count + 1);
  
  p = dir;
  d = res;
  while (*p)
    {
      c = *p++;
      if (c == '_')
        {
          *d++ = '_';
          *d++ = '_';
        }
      else if (c == '/')
        {
          *d++ = '_';
          
          /* Skip consecutive slashes, they are unnecessary,
             and break our escaping */
          while (*p == '/')
            p++;
        }
      else
        *d++ = c;
    }
  *d = 0;
  
  return res;
}

static char *
unescape_pathname (const char *escaped_dir, int len)
{
  char *dir, *d;
  const char *p, *end;
  char c;

  if (len == -1)
    len = strlen (escaped_dir);

  /* If first char is _ this is a homedir trash file */
  if (len > 1 && *escaped_dir == '_')
    {
      char *trashname;
      trashname = g_strndup (escaped_dir + 1, len - 1);
      return g_build_filename (g_get_user_data_dir (), "Trash", trashname, NULL);
      g_free (trashname);
    }
  
  dir = g_malloc (len + 1 + 1);

  p = escaped_dir;
  d = dir;
  *d++ = '/';
  end = p + len;
  while (p < end)
    {
      c = *p++;
      if (c == '_')
        {
          if (p == end)
            *d++ = '_';
          else
            {
              c = *(p+1);
              if (c == '_')
                {
                  p++;
                  *d++ = '_';
                }
              else
                *d++ = '/';
            }
        }
      else
        *d++ = c;
    }
  *d = 0;

  return dir;
}

static char *
get_top_dir_for_trash_dir (const char *trash_dir)
{
  char *basename, *dirname;
  char *user_trash_basename;
  char *user_sys_dir, *res;

  basename = g_path_get_basename (trash_dir);
  if (strcmp (basename, "Trash") == 0)
    {
      /* This is $XDG_DATA_DIR/Trash */
      g_free (basename);
      return g_path_get_dirname (trash_dir);
    }
  
  user_trash_basename =  g_strdup_printf (".Trash-%d", getuid());
  if (strcmp (basename, user_trash_basename) == 0)
    {
      g_free (user_trash_basename);
      g_free (basename);
      return g_path_get_dirname (trash_dir);
    }
  g_free (user_trash_basename);

  user_sys_dir = g_strdup_printf ("%d", getuid());
  if (strcmp (basename, user_sys_dir) == 0)
    {
      g_free (user_sys_dir);
      dirname = g_path_get_dirname (trash_dir);
      g_free (basename);
      basename = g_path_get_basename (dirname);

      if (strcmp (basename, ".Trash") == 0)
        {
          res = g_path_get_dirname (dirname);
          g_free (dirname);
          g_free (basename);
          return res;
        }
      
      g_free (dirname);
    }
  g_free (user_sys_dir);
  g_free (basename);

  /* Weird, but we return something at least */
  return g_strdup (trash_dir);
}

/* FALSE => root */
static gboolean
decode_path (const char *filename, char **trashdir, char **trashfile, char **relative_path, char **topdir)
{
  const char *first_entry, *first_entry_end;
  char *first_item;
  
  if (*filename == 0 || *filename != '/')
    return FALSE;

  while (*filename == '/')
    filename++;

  if (*filename == 0)
    return FALSE;

  first_entry = filename;

  while (*filename != 0 && *filename != '/')
    filename++;

  first_entry_end = filename;

  while (*filename == '/')
    filename++;
  
  first_item = unescape_pathname (first_entry, first_entry_end - first_entry);
  *trashfile = g_path_get_basename (first_item);
  *trashdir = g_path_get_dirname (first_item);
  *topdir = get_top_dir_for_trash_dir (*trashdir);

  if (*filename)
    *relative_path = g_strdup (filename);
  else
    *relative_path = NULL;
  return TRUE;
}

typedef enum {
  HAS_SYSTEM_DIR = 1<<0,
  HAS_USER_DIR = 1<<1,
} TopdirInfo;

static TopdirInfo
check_topdir (const char *topdir)
{
  TopdirInfo res;
  struct stat statbuf;
  char *sysadmin_dir, *sysadmin_dir_uid;
  char *user_trash_basename, *user_trash;

  res = 0;
  
  sysadmin_dir = g_build_filename (topdir, ".Trash", NULL);
  if (lstat (sysadmin_dir, &statbuf) == 0 &&
      S_ISDIR (statbuf.st_mode) &&
      statbuf.st_mode & S_ISVTX)
    {
      /* We have a valid sysadmin .Trash dir, look for uid subdir */
      sysadmin_dir_uid = g_strdup_printf ("%s/%d", sysadmin_dir, getuid());
      
      if (lstat (sysadmin_dir_uid, &statbuf) == 0 &&
          S_ISDIR (statbuf.st_mode) &&
          statbuf.st_uid == getuid())
        res |= HAS_SYSTEM_DIR;
      
      g_free (sysadmin_dir_uid);
    }
  g_free (sysadmin_dir);

  user_trash_basename =  g_strdup_printf (".Trash-%d", getuid());
  user_trash = g_build_filename (topdir, user_trash_basename, NULL);
  g_free (user_trash_basename);
  
  if (lstat (user_trash, &statbuf) == 0 &&
      S_ISDIR (statbuf.st_mode) &&
      statbuf.st_uid == getuid())
    res |= HAS_USER_DIR;

  g_free (user_trash);

  return res;
}

static gboolean
wait_for_fd_with_timeout (int fd, int timeout_secs)
{
  int res;
          
  do
    {
#ifdef HAVE_POLL
      struct pollfd poll_fd;
      poll_fd.fd = fd;
      poll_fd.events = POLLIN;
      res = poll (&poll_fd, 1, timeout_secs * 1000);
#else
      struct timeval tv;
      fd_set read_fds;
      
      tv.tv_sec = timeout_secs;
      tv.tv_usec = 0;
      
      FD_ZERO(&read_fds);
      FD_SET(fd, &read_fds);
      
      res = select (fd + 1, &read_fds, NULL, NULL, &tv);
#endif
    } while (res == -1 && errno == EINTR);
  
  return res > 0;
}

/* We do this convoluted fork + pipe thing to avoid hanging
   on e.g stuck NFS mounts, which is somewhat common since
   we're basically stat:ing all mounted filesystems */
static GList *
get_topdir_info (GList *topdirs)
{
	GList *result = NULL;

	while (topdirs)
    {
      guint32 topdir_info = 0;
      pid_t pid;
      int pipes[2];
      int status;
      
      if (pipe (pipes) == -1)
        goto error;
      
      pid = fork ();
      if (pid == -1)
        {
          close (pipes[0]);
          close (pipes[1]);
          goto error;
        }
      
      if (pid == 0)
        {
          /* Child */
          close (pipes[0]);
          
          /* Fork an intermediate child that immediately exits
           * so we can waitpid it. This means the final process
           * will get owned by init and not go zombie.
           */
          pid = fork ();
          
          if (pid == 0)
            {
              /* Grandchild */
              while (topdirs)
                {
                  guint32 info;
                  info = check_topdir ((char *)topdirs->data);
                  write (pipes[1], (char *)&info, sizeof (guint32));
                  topdirs = topdirs->next;
                }
            }
          close (pipes[1]);
          _exit (0);
          g_assert_not_reached ();
        }
      
      /* Parent */
      close (pipes[1]);

      /* Wait for the intermidate process to die */
    retry_waitpid:
      if (waitpid (pid, &status, 0) < 0)
        {
          if (errno == EINTR)
            goto retry_waitpid;
          else if (errno == ECHILD)
            ; /* do nothing, child already reaped */
          else
            g_warning ("waitpid() should not fail in get_topdir_info");
        }
      
      while (topdirs)
        {
          if (!wait_for_fd_with_timeout (pipes[0], 3) ||
              read (pipes[0], (char *)&topdir_info, sizeof (guint32)) != sizeof (guint32))
            break;
          
          result = g_list_prepend (result, GUINT_TO_POINTER (topdir_info));
          topdirs = topdirs->next;
        }
      
      close (pipes[0]);
      
    error:
      if (topdirs)
        {
          topdir_info = 0;
          result = g_list_prepend (result, GUINT_TO_POINTER (topdir_info));
          topdirs = topdirs->next;
        }
    }
  
	return g_list_reverse (result);
}

static GList *
list_trash_dirs (void)
{
  GList *mounts, *l, *li;
  const char *topdir;
  char *home_trash;
  GUnixMount *mount;
  GList *dirs;
  GList *topdirs;
  GList *topdirs_info;

  dirs = NULL;
  
  home_trash = g_build_filename (g_get_user_data_dir (), "Trash", NULL);
  if (g_file_test (home_trash, G_FILE_TEST_IS_DIR))
    dirs = g_list_prepend (dirs, home_trash);
  else
    g_free (home_trash);

  topdirs = NULL;
  mounts = g_get_unix_mounts ();
  for (l = mounts; l != NULL; l = l->next)
    {
      mount = l->data;
      
      topdir = g_unix_mount_get_mount_path (mount);
      topdirs = g_list_prepend (topdirs, g_strdup (topdir));
      
      g_unix_mount_free (mount);
    }
  g_list_free (mounts);

  topdirs_info = get_topdir_info (topdirs);

  for (l = topdirs, li = topdirs_info; l != NULL && li != NULL; l = l->next, li = li->next)
    {
      topdir = l->data;
      TopdirInfo info = GPOINTER_TO_UINT (li->data);
      char *basename, *trashdir;

      if (info & HAS_SYSTEM_DIR)
        {
          basename = g_strdup_printf ("%d", getuid());
          trashdir = g_build_filename (topdir, ".Trash", basename, NULL);
          g_free (basename);
          dirs = g_list_prepend (dirs, trashdir);
        }
      
      if (info & HAS_USER_DIR)
        {
          basename = g_strdup_printf (".Trash-%d", getuid());
          trashdir = g_build_filename (topdir, basename, NULL);
          g_free (basename);
          dirs = g_list_prepend (dirs, trashdir);
        }
    }

  return g_list_reverse (dirs);
}

static void
g_vfs_backend_trash_finalize (GObject *object)
{
  GVfsBackendTrash *backend;

  backend = G_VFS_BACKEND_TRASH (object);

  g_mount_spec_unref (backend->mount_spec);

  
  if (G_OBJECT_CLASS (g_vfs_backend_trash_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_trash_parent_class)->finalize) (object);
}

static void
g_vfs_backend_trash_init (GVfsBackendTrash *trash_backend)
{
  GVfsBackend *backend = G_VFS_BACKEND (trash_backend);
  GMountSpec *mount_spec;
  
  g_vfs_backend_set_display_name (backend, "trash");

  mount_spec = g_mount_spec_new ("trash");
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  trash_backend->mount_spec = mount_spec;
}

static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
 }

static void
do_open_for_read (GVfsBackend *backend,
                  GVfsJobOpenForRead *job,
                  const char *filename)
{
  char *trashdir, *topdir, *relative_path, *trashfile;

  if (!decode_path (filename, &trashdir, &trashfile, &relative_path, &topdir)) 
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_IS_DIRECTORY,
                      _("Can't open directory"));
  else
    {
      GFile *file;
      char *dir;
      GError *error;
      GFileInputStream *stream;
      
      dir = g_build_filename (trashdir, "files", trashfile, relative_path, NULL);
      file = g_file_new_for_path (dir);
      
      error = NULL;
      stream = g_file_read (file,
                            G_VFS_JOB (job)->cancellable,
                            &error);
      g_object_unref (file);

      if (stream)
        {
          g_vfs_job_open_for_read_set_handle (job, stream);
          g_vfs_job_open_for_read_set_can_seek  (job,
                                                 g_file_input_stream_can_seek (G_FILE_INPUT_STREAM (stream)));
          g_vfs_job_succeeded (G_VFS_JOB (job));
        }
      else
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
        }
      
      g_free (trashdir);
      g_free (trashfile);
      g_free (relative_path);
      g_free (topdir);
    }
}


static void
do_read (GVfsBackend *backend,
         GVfsJobRead *job,
         GVfsBackendHandle _handle,
         char *buffer,
         gsize bytes_requested)
{
  GInputStream *stream;
  gssize res;
  GError *error;

  stream = G_INPUT_STREAM (_handle);

  error = NULL;
  res = g_input_stream_read (stream,
                             buffer, bytes_requested,
                             G_VFS_JOB (job)->cancellable,
                             &error);

  if (res != -1)
    {
      g_vfs_job_read_set_size (job, res);
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
  GFileInputStream *stream;
  GError *error;

  stream = G_FILE_INPUT_STREAM (_handle);

  error = NULL;
  if (g_file_input_stream_seek (stream,
                                offset, type,
                                G_VFS_JOB (job)->cancellable,
                                &error))
    {
      g_vfs_job_seek_read_set_offset (job,
                                      g_file_input_stream_tell (stream));
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
  GInputStream *stream;
  GError *error;

  stream = G_INPUT_STREAM (_handle);

  error = NULL;
  if (g_input_stream_close (stream,
                            G_VFS_JOB (job)->cancellable,
                            &error))
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }

}

static void
add_extra_trash_info (GFileInfo *file_info,
                      const char *topdir,
                      const char *info_dir,
                      const char *filename,
                      const char *relative_path)
{
  char *info_filename;
  char *info_path;
  char *orig_path, *orig_path_key, *orig_path_unescaped, *date;
  GKeyFile *keyfile;
  char *display_name;


  /* Override all writability */
  g_file_info_set_attribute_boolean (file_info,
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
                                     FALSE);
  g_file_info_set_attribute_boolean (file_info,
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
                                     FALSE);
  g_file_info_set_attribute_boolean (file_info,
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME,
                                     FALSE);
  
  /* But we can delete */
  g_file_info_set_attribute_boolean (file_info,
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE,
                                     TRUE);
  
  info_filename = g_strconcat (filename, ".trashinfo", NULL);
  info_path = g_build_filename (info_dir, info_filename, NULL);
  g_free (info_filename);

  keyfile = g_key_file_new ();
  if (g_key_file_load_from_file (keyfile, info_path, G_KEY_FILE_NONE, NULL))
    {
      orig_path_key = g_key_file_get_string (keyfile, "Trash Info", "Path", NULL);
      if (orig_path_key)
        {
          orig_path_unescaped = g_uri_unescape_string (orig_path_key, NULL, "");

          if (orig_path_unescaped)
            {
              /* Set display name and edit name based of original basename */
              display_name = g_filename_display_basename (orig_path_unescaped);

              g_file_info_set_edit_name (file_info, display_name);
              
              if (strstr (display_name, "\357\277\275") != NULL)
                {
                  char *p = display_name;
                  display_name = g_strconcat (display_name, _(" (invalid encoding)"), NULL);
                  g_free (p);
                }
              g_file_info_set_display_name (file_info, display_name);
              g_free (display_name);
              
              
              /* Set orig_path */
              
              if (g_path_is_absolute (orig_path_unescaped))
                orig_path = g_build_filename (orig_path_unescaped, relative_path, NULL);
              else
                orig_path = g_build_filename (topdir, orig_path_unescaped, relative_path, NULL);

              
              g_file_info_set_attribute_byte_string (file_info,
                                                     "trash:orig_path",
                                                     orig_path);
              g_free (orig_path);
              g_free (orig_path_unescaped);
            }
          

          g_free (orig_path_key);
        }
      
      date = g_key_file_get_string (keyfile, "Trash Info", "DeletionDate", NULL);
      if (date && g_utf8_validate (date, -1, NULL))
        g_file_info_set_attribute_string (file_info,
                                          "trash:deletion_date",
                                          date);
      g_free (date);
    }
  g_key_file_free (keyfile);
  g_free (info_path);
}

static void
enumerate_root_trashdir (GVfsBackend *backend,
                         GVfsJobEnumerate *job,
                         const char *topdir,
                         const char *trashdir)
{ 
  GFile *file, *files_file;
  GFileEnumerator *enumerator;
  GFileInfo *info;
  const char *name;
  char *new_name, *new_name_escaped;
  char *info_dir;

  info_dir = g_build_filename (trashdir, "info", NULL);
  
  file = g_file_new_for_path (trashdir);
  files_file = g_file_get_child (file, "files");
  enumerator =
    g_file_enumerate_children (files_file,
                               job->attributes,
                               job->flags,
                               G_VFS_JOB (job)->cancellable,
                               NULL);
  g_object_unref (files_file);
  g_object_unref (file);

  if (enumerator)
    {
      while ((info = g_file_enumerator_next_file (enumerator,
                                                  G_VFS_JOB (job)->cancellable,
                                                  NULL)) != NULL)
        {
          name = g_file_info_get_name (info);

          /* Get the display name, etc */
          add_extra_trash_info (info,
                                topdir,
                                info_dir,
                                name,
                                NULL);

          
          /* Update the name to also have the trash dir */
          new_name = g_build_filename (trashdir, name, NULL);
          new_name_escaped = escape_pathname (new_name);
          g_free (new_name);
          g_file_info_set_name (info, new_name_escaped);
          g_free (new_name_escaped);
          
          g_vfs_job_enumerate_add_info   (job, info);
          g_object_unref (info);
        }
      
      g_file_enumerator_close (enumerator,
                               G_VFS_JOB (job)->cancellable,
                               NULL);
      g_object_unref (enumerator);
    }
}

static void
enumerate_root (GVfsBackend *backend,
                GVfsJobEnumerate *job)
{
  GList *trashdirs, *l;
  char *trashdir;
  char *topdir;

  /* Always succeeds */
  g_vfs_job_succeeded (G_VFS_JOB (job));

  trashdirs = list_trash_dirs ();
  
  for (l = trashdirs; l != NULL; l = l->next)
    {
      trashdir = l->data;
      topdir = get_top_dir_for_trash_dir (trashdir);

      enumerate_root_trashdir (backend, job, topdir, trashdir);
      g_free (trashdir);
      g_free (topdir);
    }
  g_list_free (trashdirs);
  
  g_vfs_job_enumerate_done (job);
}

static void
do_enumerate (GVfsBackend *backend,
              GVfsJobEnumerate *job,
              const char *filename,
              GFileAttributeMatcher *attribute_matcher,
              GFileQueryInfoFlags flags)
{
  char *trashdir, *topdir, *relative_path, *trashfile;

  if (!decode_path (filename, &trashdir, &trashfile, &relative_path, &topdir))
    enumerate_root (backend, job);
  else
    {
      GFile *file;
      GFileEnumerator *enumerator;
      GFileInfo *info;
      const char *name;
      char *dir;
      GError *error;

      dir = g_build_filename (trashdir, "files", trashfile, relative_path, NULL);
      file = g_file_new_for_path (dir);
      error = NULL;
      enumerator =
        g_file_enumerate_children (file,
                                   job->attributes,
                                   job->flags,
                                   G_VFS_JOB (job)->cancellable,
                                   &error);
      g_free (dir);
      g_object_unref (file);

      if (enumerator)
        {
          g_vfs_job_succeeded (G_VFS_JOB (job));
          
          while ((info = g_file_enumerator_next_file (enumerator,
                                                      G_VFS_JOB (job)->cancellable,
                                                      NULL)) != NULL)
            {
              name = g_file_info_get_name (info);
              
              g_vfs_job_enumerate_add_info   (job, info);
              g_object_unref (info);
            }
          
          g_file_enumerator_close (enumerator,
                                   G_VFS_JOB (job)->cancellable,
                                   NULL);
          g_object_unref (enumerator);
               
          g_vfs_job_enumerate_done (job);
        }
      else
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
        }

      g_free (trashdir);
      g_free (trashfile);
      g_free (relative_path);
      g_free (topdir);
    }
}

static void
do_query_info (GVfsBackend *backend,
               GVfsJobQueryInfo *job,
               const char *filename,
               GFileQueryInfoFlags flags,
               GFileInfo *info,
               GFileAttributeMatcher *matcher)
{
  char *trashdir, *topdir, *relative_path, *trashfile;
  GIcon *icon;

  if (!decode_path (filename, &trashdir, &trashfile, &relative_path, &topdir))
    {
      /* The trash:/// root */
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_display_name (info, _("Trashcan"));
      g_file_info_set_content_type (info, "inode/directory");

      /* TODO: Add -full version? */
      icon = g_themed_icon_new ("user-trash");
      /*TODO: Crashes: g_file_info_set_icon (info, icon); */
      g_object_unref (icon);
      
      g_file_info_set_attribute_boolean (info,
                                         G_FILE_ATTRIBUTE_STD_IS_VIRTUAL,
                                         TRUE);
      g_file_info_set_attribute_boolean (info,
                                         G_FILE_ATTRIBUTE_ACCESS_CAN_READ,
                                         TRUE);
      g_file_info_set_attribute_boolean (info,
                                         G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
                                         FALSE);
      g_file_info_set_attribute_boolean (info,
                                         G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
                                         FALSE);
      g_file_info_set_attribute_boolean (info,
                                         G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE,
                                         FALSE);
      g_file_info_set_attribute_boolean (info,
                                         G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME,
                                         FALSE);
      
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      GFile *file;
      GFileInfo *local_info;
      char *path;
      GError *error; 
      char *info_dir;
     
      path = g_build_filename (trashdir, "files", trashfile, relative_path, NULL);
      file = g_file_new_for_path (path);
      g_free (path);
      
      error = NULL;
      local_info = g_file_query_info (file,
                                      job->attributes,
                                      job->flags,
                                      G_VFS_JOB (job)->cancellable,
                                      &error);
      g_object_unref (file);
      
      if (local_info)
        {
          g_file_info_copy_into (local_info, info);

          info_dir = g_build_filename (trashdir, "info", NULL);
          add_extra_trash_info (info,
                                topdir,
                                info_dir,
                                trashfile,
                                relative_path);
          g_free (info_dir);
          
          g_object_unref (local_info);
          g_vfs_job_succeeded (G_VFS_JOB (job));
        }
      else
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
        }
  
      g_free (trashdir);
      g_free (trashfile);
      g_free (relative_path);
      g_free (topdir);
    }
}

static void
do_delete (GVfsBackend *backend,
           GVfsJobDelete *job,
           const char *filename)
{
  char *trashdir, *topdir, *relative_path, *trashfile;
  
  if (!decode_path (filename, &trashdir, &trashfile, &relative_path, &topdir))
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_PERMISSION_DENIED,
                      _("Can't delete trash"));
  else
    {
      GFile *file;
      GError *error; 
      char *path, *info_filename, *info_path;

      path = g_build_filename (trashdir, "files", trashfile, relative_path, NULL);
      file = g_file_new_for_path (path);
      g_free (path);
      
      error = NULL;
      if (g_file_delete (file,
                         G_VFS_JOB (job)->cancellable,
                         &error))
        {
          g_vfs_job_succeeded (G_VFS_JOB (job));

          if (relative_path == NULL)
            {
              info_filename = g_strconcat (trashfile, ".trashinfo", NULL);
              info_path = g_build_filename (trashdir, "info", info_filename, NULL);
              g_free (info_filename);
              g_unlink (info_path);
              g_free (info_path);
            }
        }
      else
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
        }
      
      g_object_unref (file);
  
      g_free (trashdir);
      g_free (trashfile);
      g_free (relative_path);
      g_free (topdir);
    }
}

typedef struct {
  GVfsMonitor *vfs_monitor;
  GObject *monitor;
  GFile *base_file;
  char *base_path;
  GMountSpec *mount_spec;
} MonitorProxy;

static void
monitor_proxy_free (MonitorProxy *proxy)
{
  g_object_unref (proxy->monitor);
  g_object_unref (proxy->base_file);
  g_mount_spec_unref (proxy->mount_spec);
  g_free (proxy->base_path);
  g_free (proxy);
}

static char *
proxy_get_trash_path (MonitorProxy *proxy,
                      GFile *file)
{
  char *file_path, *basename;
  
  if (g_file_equal (file, proxy->base_file))
    file_path = g_strdup (proxy->base_path);
  else
    {
      basename = g_file_get_relative_path (proxy->base_file, file);
      file_path = g_build_filename (proxy->base_path, basename, NULL);
      g_free (basename);
    }

  return file_path;
}

static void
proxy_changed (GFileMonitor* monitor,
               GFile* file,
               GFile* other_file,
               GFileMonitorEvent event_type,
               MonitorProxy *proxy)
{
  GMountSpec *file_spec;
  char *file_path;
  GMountSpec *other_file_spec;
  char *other_file_path;

  file_spec = proxy->mount_spec;
  file_path = proxy_get_trash_path (proxy, file);

  if (other_file)
    {
      other_file_spec = proxy->mount_spec;
      other_file_path = proxy_get_trash_path (proxy, other_file);
    }
  else
    {
      other_file_spec = NULL;
      other_file_path = NULL;
    }
  
  g_vfs_monitor_emit_event (proxy->vfs_monitor,
                            event_type,
                            file_spec, file_path,
                            other_file_spec, other_file_path);

  g_free (file_path);
  g_free (other_file_path);
}

static void
do_create_dir_monitor (GVfsBackend *backend,
                       GVfsJobCreateMonitor *job,
                       const char *filename,
                       GFileMonitorFlags flags)
{
  char *trashdir, *topdir, *relative_path, *trashfile;
  
  if (!decode_path (filename, &trashdir, &trashfile, &relative_path, &topdir))
    {
      /* The trash:/// root */
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        "trash: notification not supported yet");
    }
  else
    {
      GFile *file;
      char *path;
      GDirectoryMonitor *monitor;
      MonitorProxy *proxy;
      
      path = g_build_filename (trashdir, "files", trashfile, relative_path, NULL);
      file = g_file_new_for_path (path);
      g_free (path);

      monitor = g_file_monitor_directory (file,
                                          flags,
                                          G_VFS_JOB (job)->cancellable);
      
      if (monitor)
        {
          proxy = g_new0 (MonitorProxy, 1); 
          proxy->vfs_monitor = g_vfs_monitor_new (g_vfs_backend_get_daemon (backend));
          proxy->monitor = G_OBJECT (monitor);
          proxy->base_path = g_strdup (filename);
          proxy->base_file = g_object_ref (file);
          proxy->mount_spec = g_mount_spec_ref (G_VFS_BACKEND_TRASH (backend)->mount_spec);
          
          g_object_set_data_full (G_OBJECT (proxy->vfs_monitor), "monitor-proxy", proxy, (GDestroyNotify) monitor_proxy_free);
          g_signal_connect (monitor, "changed", G_CALLBACK (proxy_changed), proxy);

          g_vfs_job_create_monitor_set_obj_path (job,
                                                 g_vfs_monitor_get_object_path (proxy->vfs_monitor));

          g_vfs_job_succeeded (G_VFS_JOB (job));
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Trash directory notification not supported"));
        }
      g_object_unref (file);
  
      g_free (trashdir);
      g_free (trashfile);
      g_free (relative_path);
      g_free (topdir);
    }
}

static void
do_create_file_monitor (GVfsBackend *backend,
                        GVfsJobCreateMonitor *job,
                        const char *filename,
                        GFileMonitorFlags flags)
{
  char *trashdir, *topdir, *relative_path, *trashfile;
  
  if (!decode_path (filename, &trashdir, &trashfile, &relative_path, &topdir))
    {
      /* The trash:/// root */
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        "trash: notification not supported yet");
    }
  else
    {
      GFile *file;
      char *path;
      GFileMonitor *monitor;
      MonitorProxy *proxy;
      
      path = g_build_filename (trashdir, "files", trashfile, relative_path, NULL);
      file = g_file_new_for_path (path);
      g_free (path);

      monitor = g_file_monitor_file (file,
                                     flags,
                                     G_VFS_JOB (job)->cancellable);
      
      if (monitor)
        {
          proxy = g_new0 (MonitorProxy, 1); 
          proxy->vfs_monitor = g_vfs_monitor_new (g_vfs_backend_get_daemon (backend));
          proxy->monitor = G_OBJECT (monitor);
          proxy->base_path = g_strdup (filename);
          proxy->base_file = g_object_ref (file);
          proxy->mount_spec = g_mount_spec_ref (G_VFS_BACKEND_TRASH (backend)->mount_spec);
          
          g_object_set_data_full (G_OBJECT (proxy->vfs_monitor), "monitor-proxy", proxy, (GDestroyNotify) monitor_proxy_free);
          g_signal_connect (monitor, "changed", G_CALLBACK (proxy_changed), proxy);

          g_vfs_job_create_monitor_set_obj_path (job,
                                                 g_vfs_monitor_get_object_path (proxy->vfs_monitor));

          g_vfs_job_succeeded (G_VFS_JOB (job));
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Trash directory notification not supported"));
        }
      g_object_unref (file);
  
      g_free (trashdir);
      g_free (trashfile);
      g_free (relative_path);
      g_free (topdir);
    }
}

static void
g_vfs_backend_trash_class_init (GVfsBackendTrashClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_trash_finalize;

  backend_class->try_mount = try_mount;
  backend_class->open_for_read = do_open_for_read;
  backend_class->read = do_read;
  backend_class->seek_on_read = do_seek_on_read;
  backend_class->close_read = do_close_read;
  backend_class->query_info = do_query_info;
  backend_class->enumerate = do_enumerate;
  backend_class->delete = do_delete;
  backend_class->create_dir_monitor = do_create_dir_monitor;
  backend_class->create_file_monitor = do_create_file_monitor;
}
