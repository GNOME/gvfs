/*
 * Copyright © 2008 Ryan Lortie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.   
 */

#include "trashdir.h"

#include <sys/stat.h>
#include <string.h>

#include "dirwatch.h"

struct OPAQUE_TYPE__TrashDir
{
  TrashRoot *root;
  GSList *items;

  GFile *directory;
  GFile *topdir;
  gboolean is_homedir;

  DirWatch *watch;
  GFileMonitor *monitor;
};

static gint
compare_basename (gconstpointer a,
                  gconstpointer b)
{
  GFile *file_a, *file_b;
  char *name_a, *name_b;
  gint result;

  file_a = (GFile *) a;
  file_b = (GFile *) b;

  name_a = g_file_get_basename (file_a);
  name_b = g_file_get_basename (file_b);

  result = strcmp (name_a, name_b);

  g_free (name_a);
  g_free (name_b);

  return result;
}

static void
trash_dir_set_files (TrashDir *dir,
                     GSList   *items)
{
  GSList **old, *new;

  items = g_slist_sort (items, (GCompareFunc) compare_basename);
  old = &dir->items;
  new = items;

  while (new || *old)
    {
      int result;

      if ((result = (new == NULL) - (*old == NULL)) == 0)
        result = compare_basename (new->data, (*old)->data);

      if (result < 0)
        {
          /* new entry.  add it. */
          *old = g_slist_prepend (*old, new->data); /* take reference */
          old = &(*old)->next;
          trash_root_add_item (dir->root, new->data, dir->is_homedir);
          new = new->next;
        }
      else if (result > 0)
        {
          /* old entry.  remove it. */
          trash_root_remove_item (dir->root, (*old)->data, dir->is_homedir);
          g_object_unref ((*old)->data);
          *old = g_slist_delete_link (*old, *old);
        }
      else
        {
          /* match.  no change. */
          old = &(*old)->next;
          g_object_unref (new->data);
          new = new->next;
        }
    }

  g_slist_free (items);

  trash_root_thaw (dir->root);
}

static void
trash_dir_empty (TrashDir *dir)
{
  trash_dir_set_files (dir, NULL);
}

static void
trash_dir_enumerate (TrashDir *dir)
{
  GFileEnumerator *enumerator;
  GSList *files = NULL;

  enumerator = g_file_enumerate_children (dir->directory,
                                          G_FILE_ATTRIBUTE_STANDARD_NAME,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          NULL, NULL);

  if (enumerator)
    {
      GFileInfo *info;

      while ((info = g_file_enumerator_next_file (enumerator, NULL, NULL)))
        {
          GFile *file;

          file = g_file_get_child (dir->directory,
                                   g_file_info_get_name (info));
          files = g_slist_prepend (files, file);

          g_object_unref (info);
        }

      g_object_unref (enumerator);
    }

  trash_dir_set_files (dir, files); /* consumes files */
}

static void
trash_dir_changed (GFileMonitor      *monitor,
                   GFile             *file,
                   GFile             *other_file,
                   GFileMonitorEvent  event_type,
                   gpointer           user_data)
{
  TrashDir *dir = user_data;

  if (event_type == G_FILE_MONITOR_EVENT_CREATED)
    trash_root_add_item (dir->root, file, dir->is_homedir);

  else if (event_type == G_FILE_MONITOR_EVENT_DELETED)
    trash_root_remove_item (dir->root, file, dir->is_homedir);

  else if (event_type == G_FILE_MONITOR_EVENT_PRE_UNMOUNT ||
           event_type == G_FILE_MONITOR_EVENT_UNMOUNTED)
    ;

  else
    {
      static gboolean already_did_warning;
      char *dirname;
      char *name;

      g_warning ("*** Unsupported operation detected on trash directory");
      if (!already_did_warning)
        {
          g_warning ("    A trash files/ directory should only have files "
                     "linked or unlinked (via moves or deletes).  Some other "
                     "operation has been detected on a file in the directory "
                     "(eg: a file has been modified).  Likely, the data "
                     "reported by the trash backend will now be "
                     "inconsistent.");
          already_did_warning = TRUE;
        }

      name = file ? g_file_get_basename (file) : NULL;
      dirname = g_file_get_path (dir->directory);
      g_warning ("  dir: %s, file: %s, type: %d\n\n",
                 dirname, name, event_type);
      g_free (dirname);
      g_free (name);
    }

  trash_root_thaw (dir->root);
}

static void
trash_dir_created (gpointer user_data)
{
  TrashDir *dir = user_data;

  g_assert (dir->monitor == NULL);
  dir->monitor = g_file_monitor_directory (dir->directory, 0, NULL, NULL);
  g_signal_connect (dir->monitor, "changed",
                    G_CALLBACK (trash_dir_changed), dir);
  trash_dir_enumerate (dir);
}

static void
trash_dir_check (gpointer user_data)
{
  TrashDir *dir = user_data;

  trash_dir_enumerate (dir);
}

static void
trash_dir_destroyed (gpointer user_data)
{
  TrashDir *dir = user_data;

  g_assert (dir->monitor != NULL);
  g_object_unref (dir->monitor);
  dir->monitor = NULL;

  trash_dir_empty (dir);
}

void
trash_dir_watch (TrashDir *dir)
{
  g_assert (dir->monitor == NULL);
  g_assert (dir->watch == NULL);

  /* start monitoring after a period of not monitoring.
   *
   * there are two possible cases here:
   *   1) the directory now exists
   *      - we have to rescan the directory to ensure that we notice
   *        any changes that have occured since we last looked
   *
   *   2) the directory does not exist
   *      - if it existed last time we looked then we may have stale
   *        toplevel items that need to be removed.
   *
   * in case 1, trash_dir_created() will be called from
   * dir_watch_new().  it calls trash_enumerate() itself.
   *
   * in case 2, no other function will be called and we must manually
   * call trash_dir_empty().
   *
   * we can tell if case 1 happened because trash_dir_created() also
   * sets the dir->monitor.
   */
  dir->watch = dir_watch_new (dir->directory, dir->topdir,
                              trash_dir_created,
                              trash_dir_check,
                              trash_dir_destroyed,
                              dir);

  if (dir->monitor == NULL)
    /* case 2 */
    trash_dir_empty (dir);
}

void
trash_dir_unwatch (TrashDir *dir)
{
  g_assert (dir->watch != NULL);

  /* stop monitoring.
   *
   * in all cases, we just fall silent.
   */

  if (dir->monitor != NULL)
    {
      g_object_unref (dir->monitor);
      dir->monitor = NULL;
    }

  dir_watch_free (dir->watch);
  dir->watch = NULL;
}

static gboolean
dir_exists (GFile *directory,
            GFile *top_dir)
{
  gboolean result = FALSE;
  GFile *parent;

  if (g_file_equal (directory, top_dir))
    return TRUE;

  parent = g_file_get_parent (directory);

  if (dir_exists (parent, top_dir))
    {
      struct stat buf;
      gchar *path;

      path = g_file_get_path (directory);
      result = !lstat (path, &buf) && S_ISDIR (buf.st_mode);

      g_free (path);
    }

  g_object_unref (parent);

  return result;
}

void
trash_dir_rescan (TrashDir *dir)
{
  if (dir->watch)
    dir_watch_check (dir->watch);

  else if (dir_exists (dir->directory, dir->topdir))
    trash_dir_enumerate (dir);

  else
    trash_dir_empty (dir);
}

static trash_dir_ui_hook ui_hook;

TrashDir *
trash_dir_new (TrashRoot  *root,
               gboolean    watching,
               gboolean    is_homedir,
               const char *mount_point,
               const char *format,
               ...)
{
  TrashDir *dir;
  va_list ap;
  char *rel;

  va_start (ap, format);
  rel = g_strdup_vprintf (format, ap);
  va_end (ap);

  dir = g_slice_new (TrashDir);

  dir->root = root;
  dir->items = NULL;
  dir->topdir = g_file_new_for_path (mount_point);
  dir->directory = g_file_get_child (dir->topdir, rel);
  dir->monitor = NULL;
  dir->is_homedir = is_homedir;

  if (watching)
    dir->watch = dir_watch_new (dir->directory,
                                dir->topdir,
                                trash_dir_created,
                                trash_dir_check,
                                trash_dir_destroyed,
                                dir);
  else
    dir->watch = NULL;

  if (ui_hook)
    ui_hook (dir, dir->directory);

  g_free (rel);

  return dir;
}

void
trash_dir_set_ui_hook (trash_dir_ui_hook _ui_hook)
{
  ui_hook = _ui_hook;
}

void
trash_dir_free (TrashDir *dir)
{
  if (dir->watch)
    dir_watch_free (dir->watch);

  if (dir->monitor)
    g_object_unref (dir->monitor);

  trash_dir_set_files (dir, NULL);

  g_object_unref (dir->directory);
  g_object_unref (dir->topdir);

  g_slice_free (TrashDir, dir);
}
