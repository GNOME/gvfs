/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2009 Benjamin Otte <otte@gnome.org>
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
 * Author: Benjamin Otte <otte@gnome.org>
 */

#include <stdio.h>
#include <sys/stat.h>

#include <config.h>

#include <glib/gi18n.h>

#include "gvfsftpdircache.h"

/*** CACHE ENTRY ***/

struct _GVfsFtpDirCacheEntry
{
  GHashTable *          files;          /* GVfsFtpFile => GFileInfo mapping */
  guint                 stamp;          /* cache's stamp when this entry was created */
  volatile int          refcount;       /* need to refount this struct for thread safety */
};

static GVfsFtpDirCacheEntry *
g_vfs_ftp_dir_cache_entry_new (guint stamp)
{
  GVfsFtpDirCacheEntry *entry;

  entry = g_slice_new0 (GVfsFtpDirCacheEntry);
  entry->files = g_hash_table_new_full (g_vfs_ftp_file_hash,
                                        g_vfs_ftp_file_equal,
                                        (GDestroyNotify) g_vfs_ftp_file_free,
                                        g_object_unref);
  entry->stamp = stamp;
  entry->refcount = 1;

  return entry;
}

static GVfsFtpDirCacheEntry *
g_vfs_ftp_dir_cache_entry_ref (GVfsFtpDirCacheEntry *entry)
{
  g_atomic_int_inc (&entry->refcount);

  return entry;
}

static void
g_vfs_ftp_dir_cache_entry_unref (GVfsFtpDirCacheEntry *entry)
{
  if (!g_atomic_int_dec_and_test (&entry->refcount))
    return;

  g_hash_table_destroy (entry->files);
  g_slice_free (GVfsFtpDirCacheEntry, entry);
}

/**
 * g_vfs_ftp_dir_cache_entry_add:
 * @entry: the entry to add data to
 * @file: the file to add. The function takes ownership of the argument.
 * @info: the file info of the @file. The function takes ownership of the
 *        reference.
 *
 * Adds a new file entry to the directory belonging to @entry. This function
 * must only be called from a @GVfsFtpListDirFunc.
 **/
void
g_vfs_ftp_dir_cache_entry_add (GVfsFtpDirCacheEntry *entry, GVfsFtpFile *file, GFileInfo *info)
{
  g_return_if_fail (entry != NULL);
  g_return_if_fail (file != NULL);
  g_return_if_fail (G_IS_FILE_INFO (info));

  g_hash_table_insert (entry->files, file, info);
}

/*** CACHE ***/

struct _GVfsFtpDirCache
{
  GHashTable *          directories;    /* GVfsFtpFile of directory => GVfsFtpDirCacheEntry mapping */
  guint                 stamp;          /* used to identify validity of cache when flushing */
  GMutex                lock;           /* mutex for thread safety of stamp and hash table */
  const GVfsFtpDirFuncs *funcs;         /* functions to call */
};

GVfsFtpDirCache *
g_vfs_ftp_dir_cache_new (const GVfsFtpDirFuncs *funcs)
{
  GVfsFtpDirCache *cache;

  g_return_val_if_fail (funcs != NULL, NULL);

  cache = g_slice_new0 (GVfsFtpDirCache);
  cache->directories = g_hash_table_new_full (g_vfs_ftp_file_hash,
                                              g_vfs_ftp_file_equal,
                                              (GDestroyNotify) g_vfs_ftp_file_free,
                                              (GDestroyNotify) g_vfs_ftp_dir_cache_entry_unref);
  g_mutex_init (&cache->lock);
  cache->funcs = funcs;

  return cache;
}

void
g_vfs_ftp_dir_cache_free (GVfsFtpDirCache *cache)
{
  g_return_if_fail (cache != NULL);

  g_hash_table_destroy (cache->directories);
  g_mutex_clear (&cache->lock);
  g_slice_free (GVfsFtpDirCache, cache);
}

static GVfsFtpDirCacheEntry *
g_vfs_ftp_dir_cache_lookup_entry (GVfsFtpDirCache *  cache,
                                  GVfsFtpTask *      task,
                                  const GVfsFtpFile *dir,
                                  guint              stamp)
{
  GVfsFtpDirCacheEntry *entry;

  g_mutex_lock (&cache->lock);
  entry = g_hash_table_lookup (cache->directories, dir);
  if (entry)
    g_vfs_ftp_dir_cache_entry_ref (entry);
  g_mutex_unlock (&cache->lock);
  if (entry && entry->stamp < stamp)
    g_vfs_ftp_dir_cache_entry_unref (entry);
  else if (entry)
    return entry;

  if (g_vfs_ftp_task_send (task,
        	           G_VFS_FTP_PASS_550,
        		   "CWD %s", g_vfs_ftp_file_get_ftp_path (dir)) == 550)
    {
      g_set_error_literal (&task->error,
        	           G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,
        		   _("The file is not a directory"));
    }
  g_vfs_ftp_task_setup_data_connection (task);
  g_vfs_ftp_task_send (task,
        	       G_VFS_FTP_PASS_100 | G_VFS_FTP_FAIL_200,
                       "%s", cache->funcs->command);
  g_vfs_ftp_task_open_data_connection (task);
  if (g_vfs_ftp_task_is_in_error (task))
    return NULL;

  entry = g_vfs_ftp_dir_cache_entry_new (stamp);
  cache->funcs->process (g_io_stream_get_input_stream (g_vfs_ftp_connection_get_data_stream (task->conn)),
                         g_vfs_ftp_connection_get_debug_id (task->conn),
                         dir,
                         entry,
                         task->cancellable,
                         &task->error);
  g_vfs_ftp_task_close_data_connection (task);
  g_vfs_ftp_task_receive (task, 0, NULL);
  if (g_vfs_ftp_task_is_in_error (task))
    {
      g_vfs_ftp_dir_cache_entry_unref (entry);
      return NULL;
    }
  g_mutex_lock (&cache->lock);
  g_hash_table_insert (cache->directories,
                       g_vfs_ftp_file_copy (dir),
                       g_vfs_ftp_dir_cache_entry_ref (entry));
  g_mutex_unlock (&cache->lock);
  return entry;
}

static GFileInfo *
g_vfs_ftp_dir_cache_lookup_file_internal (GVfsFtpDirCache *  cache,
                                          GVfsFtpTask *      task,
                                          const GVfsFtpFile *file,
                                          guint              stamp)
{
  GVfsFtpDirCacheEntry *entry;
  GVfsFtpFile *dir;
  GFileInfo *info;

  if (g_vfs_ftp_task_is_in_error (task))
    return NULL;

  if (!g_vfs_ftp_file_is_root (file))
    {
      dir = g_vfs_ftp_file_new_parent (file);
      entry = g_vfs_ftp_dir_cache_lookup_entry (cache, task, dir, stamp);
      g_vfs_ftp_file_free (dir);
      if (entry == NULL)
        return NULL;

      info = g_hash_table_lookup (entry->files, file);
      if (info != NULL)
        {
          /* NB: the order of ref/unref is important here */
          g_object_ref (info);
          g_vfs_ftp_dir_cache_entry_unref (entry);
          return info;
        }

      g_vfs_ftp_dir_cache_entry_unref (entry);
    }

  if (g_vfs_ftp_task_is_in_error (task))
    return NULL;

  return cache->funcs->lookup_uncached (task, file);
}

static GFileInfo *
g_vfs_ftp_dir_cache_resolve_symlink (GVfsFtpDirCache *  cache,
                                     GVfsFtpTask *      task,
                                     const GVfsFtpFile *file,
                                     GFileInfo *        original,
                                     guint              stamp)
{
  static const char *copy_attributes[] = {
    G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK,
    G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
    G_FILE_ATTRIBUTE_STANDARD_NAME,
    G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
    G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME,
    G_FILE_ATTRIBUTE_STANDARD_COPY_NAME,
    G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET
  };
  GFileInfo *info, *result;
  GVfsFtpFile *tmp, *link;
  guint i, lookups = 0;
  const char *target;

  if (!g_file_info_get_is_symlink (original) ||
      g_vfs_ftp_task_is_in_error (task))
    return original;

  info = g_object_ref (original);
  link = g_vfs_ftp_file_copy (file);
  do {
      target = g_file_info_get_symlink_target (info);
      if (target == NULL)
        {
          /* This happens when bad servers don't report a symlink target.
           * We now want to figure out if this is a directory or regular file,
           * so we can at least report something useful.
           */
          g_object_unref (info);
          info = cache->funcs->lookup_uncached (task, file);
          break;
        }
      tmp = link;
      link = cache->funcs->resolve_symlink (task, tmp, g_file_info_get_symlink_target (info));
      g_vfs_ftp_file_free (tmp);
      g_object_unref (info);
      if (link == NULL)
        {
          g_vfs_ftp_task_clear_error (task);
          return original;
        }
      info = g_vfs_ftp_dir_cache_lookup_file_internal (cache, task, link, stamp);
      if (info == NULL)
        {
          g_vfs_ftp_file_free (link);
          g_vfs_ftp_task_clear_error (task);
          return original;
        }
    }
  while (g_file_info_get_is_symlink (info) && lookups++ < 8);

  g_vfs_ftp_file_free (link);
  if (g_file_info_get_is_symlink (info))
    {
      /* too many recursions */
      g_object_unref (info);
      return original;
    }

  result = g_file_info_dup (info);
  g_object_unref (info);
  for (i = 0; i < G_N_ELEMENTS (copy_attributes); i++)
    {
      GFileAttributeType type;
      gpointer value;

      if (!g_file_info_get_attribute_data (original,
        				   copy_attributes[i],
        				   &type,
        				   &value,
        				   NULL))
        continue;
     
      g_file_info_set_attribute (result,
                                 copy_attributes[i],
        			 type,
        			 value);
    }
  g_object_unref (original);

  return result;
}

/* Try to obtain correct mtime using the MDTM command if time is 00:00:00. */
static void
g_vfs_ftp_dir_cache_fix_mtime (GVfsFtpTask *      task,
                               const GVfsFtpFile *file,
                               GFileInfo *        info)
{
  g_autoptr(GDateTime) dt = NULL;
  g_auto(GStrv) reply = NULL;
  struct tm tm = { 0 };
  gint num;
  time_t mtime;

  if (!g_vfs_backend_ftp_has_feature (task->backend, G_VFS_FTP_FEATURE_MDTM))
    return;

  if (g_file_info_get_file_type (info) != G_FILE_TYPE_REGULAR)
    return;

  dt = g_file_info_get_modification_date_time (info);
  if (g_date_time_get_hour (dt) != 0 ||
      g_date_time_get_minute (dt) != 0 ||
      g_date_time_get_second (dt) != 0)
    return;

  if (g_vfs_ftp_task_send_and_check (task, 0, NULL, NULL, &reply, "MDTM %s", g_vfs_ftp_file_get_ftp_path (file)) != 213)
    {
      g_vfs_ftp_task_clear_error (task);
      return;
    }

  num = sscanf (reply[0] + 4, "%4d%2d%2d%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
  if (num != 6)
    return;

  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  mtime = timegm (&tm);
  if (mtime == -1)
    return;

  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, mtime);
}

GFileInfo *
g_vfs_ftp_dir_cache_lookup_file (GVfsFtpDirCache *  cache,
                                 GVfsFtpTask *      task,
                                 const GVfsFtpFile *file,
                                 gboolean           resolve_symlinks)
{
  GFileInfo *info;

  g_return_val_if_fail (cache != NULL, NULL);
  g_return_val_if_fail (task != NULL, NULL);
  g_return_val_if_fail (file != NULL, NULL);

  info = g_vfs_ftp_dir_cache_lookup_file_internal (cache, task, file, 0);
  g_vfs_ftp_dir_cache_fix_mtime (task, file, info);

  if (info != NULL && resolve_symlinks)
    info = g_vfs_ftp_dir_cache_resolve_symlink (cache, task, file, info, 0);

  return info;
}

GList *
g_vfs_ftp_dir_cache_lookup_dir (GVfsFtpDirCache *  cache,
                                GVfsFtpTask *      task,
                                const GVfsFtpFile *dir,
                                gboolean           flush,
                                gboolean           resolve_symlinks)
{
  GVfsFtpDirCacheEntry *entry;
  GHashTableIter iter;
  gpointer file, info;
  guint stamp;
  GList *result = NULL;

  g_return_val_if_fail (cache != NULL, NULL);
  g_return_val_if_fail (task != NULL, NULL);
  g_return_val_if_fail (dir != NULL, NULL);

  if (g_vfs_ftp_task_is_in_error (task))
    return NULL;

  if (flush)
    {
      g_mutex_lock (&cache->lock);
      g_assert (cache->stamp != G_MAXUINT);
      stamp = ++cache->stamp;
      g_mutex_unlock (&cache->lock);
    }
  else
    stamp = 0;

  entry = g_vfs_ftp_dir_cache_lookup_entry (cache, task, dir, stamp);
  if (entry == NULL)
    return NULL;

  g_hash_table_iter_init (&iter, entry->files);
  while (g_hash_table_iter_next (&iter, &file, &info))
    {
      g_object_ref (info);

      g_vfs_ftp_dir_cache_fix_mtime (task, file, info);

      if (resolve_symlinks)
        info = g_vfs_ftp_dir_cache_resolve_symlink (cache, task, file, info, stamp);
      g_assert (!g_vfs_ftp_task_is_in_error (task));
      result = g_list_prepend (result, info);
    }
  g_vfs_ftp_dir_cache_entry_unref (entry);

  return result;
}

void
g_vfs_ftp_dir_cache_purge_dir (GVfsFtpDirCache *  cache,
                               const GVfsFtpFile *dir)
{
  g_return_if_fail (cache != NULL);
  g_return_if_fail (dir != NULL);

  g_mutex_lock (&cache->lock);
  g_hash_table_remove (cache->directories, dir);
  g_mutex_unlock (&cache->lock);
}

void
g_vfs_ftp_dir_cache_purge_file (GVfsFtpDirCache *  cache,
        			const GVfsFtpFile *file)
{
  GVfsFtpFile *dir;
 
  g_return_if_fail (cache != NULL);
  g_return_if_fail (file != NULL);

  if (g_vfs_ftp_file_is_root (file))
    return;

  dir = g_vfs_ftp_file_new_parent (file);
  g_vfs_ftp_dir_cache_purge_dir (cache, dir);
  g_vfs_ftp_file_free (dir);
}

/*** DIR CACHE FUNCS ***/

#include "ParseFTPList.h"
#include "gvfsdaemonutils.h"

static GFileInfo *
create_root_file_info (GVfsBackendFtp *ftp)
{
  GFileInfo *info;
  GIcon *icon;
  char *display_name;
 
  info = g_file_info_new ();
  g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);

  g_file_info_set_name (info, "/");
  display_name = g_strdup_printf (_("/ on %s"), ftp->host_display_name);
  g_file_info_set_display_name (info, display_name);
  g_free (display_name);
  g_file_info_set_edit_name (info, "/");

  g_file_info_set_content_type (info, "inode/directory");
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, "inode/directory");
  g_file_info_set_is_symlink (info, FALSE);

  icon = g_themed_icon_new ("folder-remote");
  g_file_info_set_icon (info, icon);
  g_object_unref (icon);
  icon = g_themed_icon_new ("folder-remote-symbolic");
  g_file_info_set_symbolic_icon (info, icon);
  g_object_unref (icon);

  return info;
}

static GFileInfo *
g_vfs_ftp_dir_cache_funcs_lookup_uncached (GVfsFtpTask *      task,
                                           const GVfsFtpFile *file)
{
  GFileInfo *info;
  char **reply;

  if (g_vfs_ftp_file_is_root (file))
    return create_root_file_info (task->backend);

  /* the directory cache fails when the parent directory of the file is not readable.
   * This cannot happen on Unix, but it can happen on FTP.
   * In this case we try to figure out as much as possible about the file (does it even exist?)
   * using standard ftp commands.
   */
  if (g_vfs_ftp_task_send (task, 0, "CWD %s", g_vfs_ftp_file_get_ftp_path (file)))
    {
      char *tmp;

      info = g_file_info_new ();

      tmp = g_path_get_basename (g_vfs_ftp_file_get_gvfs_path (file));
      g_file_info_set_name (info, tmp);
      g_free (tmp);

      gvfs_file_info_populate_default (info, g_vfs_ftp_file_get_gvfs_path (file), G_FILE_TYPE_DIRECTORY);

      g_file_info_set_is_hidden (info, TRUE);
      
      return info;
    }
  
  g_vfs_ftp_task_clear_error (task);
  if (g_vfs_ftp_task_send_and_check (task, 0, NULL, NULL, &reply, "SIZE %s", g_vfs_ftp_file_get_ftp_path (file)))
    {
      char *tmp;

      info = g_file_info_new ();

      tmp = g_path_get_basename (g_vfs_ftp_file_get_gvfs_path (file));
      g_file_info_set_name (info, tmp);
      g_free (tmp);

      gvfs_file_info_populate_default (info, g_vfs_ftp_file_get_gvfs_path (file), G_FILE_TYPE_REGULAR);

      g_file_info_set_size (info, g_ascii_strtoull (reply[0] + 4, NULL, 0));
      g_strfreev (reply);

      g_file_info_set_is_hidden (info, TRUE);
      return info;
    }
      
  g_vfs_ftp_task_clear_error (task);

  /* note that there might still be a file/directory, we just have
   * no way to figure this out (in particular on ftp servers that
   * don't support SIZE.
   * If you have ways to improve file detection, patches are welcome. */

  return NULL;
}

static gboolean
g_vfs_ftp_parse_mode (char       file_mode[10],
                      guint32   *mode,
                      GFileType *file_type)
{
  /* File type */
  switch (file_mode[0])
    {
    case '-': /* Regular file */
      *mode = S_IFREG;
      *file_type = G_FILE_TYPE_REGULAR;
      break;
    case 'b': /* Block special file */
      *mode = S_IFBLK;
      *file_type = G_FILE_TYPE_SPECIAL;
      break;
    case 'c': /* Character special */
      *mode = S_IFCHR;
      *file_type = G_FILE_TYPE_SPECIAL;
      break;
    case 'd': /* Directory */
      *mode = S_IFDIR;
      *file_type = G_FILE_TYPE_DIRECTORY;
      break;
    case 'l': /* Symbolic link */
      *mode = S_IFLNK;
      *file_type = G_FILE_TYPE_SYMBOLIC_LINK;
      break;
    case 'p': /* FIFO */
      *mode = S_IFIFO;
      *file_type = G_FILE_TYPE_SPECIAL;
      break;
    default:
      g_debug ("# couldn't parse file type from mode %.10s\n", file_mode);
      *mode = 0;
      *file_type = G_FILE_TYPE_UNKNOWN;
      return FALSE;
    }

  /* Permissions */
  if (file_mode[1] == 'r')
    *mode |= S_IRUSR;
  if (file_mode[2] == 'w')
    *mode |= S_IWUSR;
  switch (file_mode[3])
    {
    case 'x': *mode |= S_IXUSR; break;
    case 'S': *mode |= S_ISUID; break;
    case 's': *mode |= S_ISUID | S_IXUSR; break;
    }

  if (file_mode[4] == 'r')
    *mode |= S_IRGRP;
  if (file_mode[5] == 'w')
    *mode |= S_IWGRP;
  switch (file_mode[6])
    {
    case 'x': *mode |= S_IXGRP; break;
    case 'S': *mode |= S_ISGID; break;
    case 's': *mode |= S_ISGID | S_IXGRP; break;
    }

  if (file_mode[7] == 'r')
    *mode |= S_IROTH;
  if (file_mode[8] == 'w')
    *mode |= S_IWOTH;
  switch (file_mode[9])
    {
    case 'x': *mode |= S_IXOTH; break;
    case 'T': *mode |= S_ISVTX; break;
    case 't': *mode |= S_ISVTX | S_IXOTH; break;
    }

  return TRUE;
}

static gboolean
g_vfs_ftp_dir_cache_funcs_process (GInputStream *        stream,
                                   int                   debug_id,
                                   const GVfsFtpFile *   dir,
                                   GVfsFtpDirCacheEntry *entry,
                                   gboolean              is_unix,
                                   GCancellable *        cancellable,
                                   GError **             error)
{
  struct list_state state = { NULL, };
  GDataInputStream *data;
  GFileInfo *info;
  int type;
  GVfsFtpFile *file;
  char *line, *s;
  gsize length;

  /* protect against code reorg - in current code, error never is NULL */
  g_assert (error != NULL);
  g_assert (*error == NULL);

  data = g_data_input_stream_new (stream);
  /* we use LF only, because the mozilla code can handle lines ending in CR */
  g_data_input_stream_set_newline_type (data, G_DATA_STREAM_NEWLINE_TYPE_LF);
  while ((line = g_data_input_stream_read_line (data, &length, cancellable, error)))
    {
      struct list_result result = { 0, };
      GFileType file_type = G_FILE_TYPE_UNKNOWN;
      time_t mtime;

      /* strip trailing \r - ParseFTPList only removes it if the line ends in \r\n,
       * but we stripped the \n already.
       */
      if (length > 0 && line[length - 1] == '\r')
        line[--length] = '\0';

      g_debug ("<<%2d <<  %s\n", debug_id, line);
      type = ParseFTPList (line, &state, &result);
      if (type != 'd' && type != 'f' && type != 'l')
        {
          g_free (line);
          continue;
        }

      /* don't list . and .. directories
       * Let's hope they're not important files on some ftp servers
       */
      if (result.fe_fnlen == 1 &&
          result.fe_fname[0] == '.')
        {
          g_free (line);
          continue;
        }
      if (result.fe_fnlen == 2 &&
          result.fe_fname[0] == '.' &&
          result.fe_fname[1] == '.')
        {
          g_free (line);
          continue;
        }

      s = g_strndup (result.fe_fname, result.fe_fnlen);
      file = g_vfs_ftp_file_new_child  (dir, s, NULL);
      g_free (s);
      if (file == NULL)
        {
          g_debug ("# invalid filename, skipping");
          g_free (line);
          continue;
        }

      info = g_file_info_new ();

      s = g_path_get_basename (g_vfs_ftp_file_get_gvfs_path (file));
      g_file_info_set_name (info, s);
      g_free (s);

      if (type == 'l')
        {
          char *link;

          link = g_strndup (result.fe_lname, result.fe_lnlen);
          g_file_info_set_symlink_target (info, link);
          g_file_info_set_is_symlink (info, TRUE);
          g_free (link);
        }
      else
        g_file_info_set_is_symlink (info, FALSE);

      g_file_info_set_size (info, g_ascii_strtoull (result.fe_size, NULL, 10));

      /* If unix format then parse the attributes */
      if (state.lstyle == 'U')
        {
          char file_mode[10], uid[64], gid[64];
          guint32 mode;

          /* POSIX ls -l form: mode, links, owner, group */
          if (sscanf(line, "%10c %*u %63s %63s", file_mode, uid, gid) == 3)
            {
              if (g_vfs_ftp_parse_mode (file_mode, &mode, &file_type))
                {
                  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE, mode);
                  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_OWNER_USER, uid);
                  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_OWNER_GROUP, gid);
                }
            }
          else
            g_debug ("# unknown listing format\n");
        }

      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);

      if (file_type == G_FILE_TYPE_UNKNOWN)
        {
          file_type = type == 'f' ? G_FILE_TYPE_REGULAR :
                      type == 'l' ? G_FILE_TYPE_SYMBOLIC_LINK :
                      G_FILE_TYPE_DIRECTORY;
        }

      gvfs_file_info_populate_default (info,
                                       g_vfs_ftp_file_get_gvfs_path (file),
                                       file_type);

      if (is_unix)
        g_file_info_set_is_hidden (info, result.fe_fnlen > 0 &&
                                         result.fe_fname[0] == '.');

      /* Workaround:
       * result.fetime.tm_year contains actual year instead of offset-from-1900,
       * which timegm expects.
       */
      if (result.fe_time.tm_year >= 1900)
              result.fe_time.tm_year -= 1900;

      mtime = timegm (&result.fe_time);
      if (mtime != -1)
        {
          char *etag = g_strdup_printf ("%ld", mtime);
          g_file_info_set_attribute_string (info,
                                            G_FILE_ATTRIBUTE_ETAG_VALUE,
                                            etag);
          g_free (etag);

          g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, mtime);
          g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, 0);
        }

      g_vfs_ftp_dir_cache_entry_add (entry, file, info);
      g_free (line);
    }

  g_object_unref (data);
  return *error != NULL;
}

static GVfsFtpFile *
g_vfs_ftp_dir_cache_funcs_resolve_default (GVfsFtpTask *      task,
                                           const GVfsFtpFile *file,
                                           const char *       target)
{
  GVfsFtpFile *link;
  GString *new_path;
  char *match;

  g_return_val_if_fail (file != NULL, NULL);
  g_return_val_if_fail (target != NULL, NULL);
 
  if (target[0] == '/')
    {
      new_path = g_string_new (target);
    }
  else
    {
      new_path = g_string_new (g_vfs_ftp_file_get_ftp_path (file));
      /* only take directory */
      match = strrchr (new_path->str, '/');
      g_string_truncate (new_path, match - new_path->str + 1);
      g_string_append (new_path, target);
    }

  g_string_append_c (new_path, '/'); /* slash at end makes code easier */
  /* cleanup: remove all double slashes */
  while ((match = strstr (new_path->str, "//")) != NULL)
    {
      g_string_erase (new_path, match - new_path->str, 1);
    }
  /* cleanup: remove all ".." and the preceeding directory */
  while ((match = strstr (new_path->str, "/../")) != NULL)
    {
      if (match == new_path->str)
        {
          g_string_erase (new_path, 0, 3);
        }
      else
        {
          char *start = match - 1;
          while (*start != '/')
            start--;
          g_string_erase (new_path, start - new_path->str, match - start + 3);
        }
    }
  /* cleanup: remove all "." directories */
  while ((match = strstr (new_path->str, "/./")) != NULL)
    g_string_erase (new_path, match - new_path->str, 2);
  /* remove trailing / */
  g_string_set_size (new_path, new_path->len - 1);

  link = g_vfs_ftp_file_new_from_ftp (task->backend, new_path->str);
  g_string_free (new_path, TRUE);
  return link;
}

static gboolean
g_vfs_ftp_dir_cache_funcs_process_unix (GInputStream *        stream,
                                        int                   debug_id,
                                        const GVfsFtpFile *   dir,
                                        GVfsFtpDirCacheEntry *entry,
                                        GCancellable *        cancellable,
                                        GError **             error)
{
  return g_vfs_ftp_dir_cache_funcs_process (stream, debug_id, dir, entry, TRUE, cancellable, error);
}

static gboolean
g_vfs_ftp_dir_cache_funcs_process_default (GInputStream *        stream,
                                           int                   debug_id,
                                           const GVfsFtpFile *   dir,
                                           GVfsFtpDirCacheEntry *entry,
                                           GCancellable *        cancellable,
                                           GError **             error)
{
  return g_vfs_ftp_dir_cache_funcs_process (stream, debug_id, dir, entry, FALSE, cancellable, error);
}

const GVfsFtpDirFuncs g_vfs_ftp_dir_cache_funcs_unix = {
  "LIST -a",
  g_vfs_ftp_dir_cache_funcs_process_unix,
  g_vfs_ftp_dir_cache_funcs_lookup_uncached,
  g_vfs_ftp_dir_cache_funcs_resolve_default
};

const GVfsFtpDirFuncs g_vfs_ftp_dir_cache_funcs_default = {
  "LIST",
  g_vfs_ftp_dir_cache_funcs_process_default,
  g_vfs_ftp_dir_cache_funcs_lookup_uncached,
  g_vfs_ftp_dir_cache_funcs_resolve_default
};
