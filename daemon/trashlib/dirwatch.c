/*
 * Copyright © 2008 Ryan Lortie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.   
 */

#include <sys/stat.h>

#include "dirwatch.h"

/* DirWatch
 *
 * a directory watcher utility for use by the trash:/ backend.
 *
 * A DirWatch monitors a given directory for existence under a very
 * specific set of circumstances.  When the directory comes into
 * existence, the create() callback is invoked.  When the directory
 * stops existing the destroy() callback is invoked.  If the directory
 * initially exists, then create() is invoked before the call to
 * dir_watch_new() returns.
 *
 * The directory to watch is considered to exist only if it is a
 * directory (and not a symlink) and its parent directory also exists.
 * A topdir must be given, which is always assumed to "exist".
 *
 * For example, if '/mnt/disk/.Trash/1000/files/' is monitored with
 * '/mnt/disk/' as a topdir then the following conditions must be true
 * in order for the directory to be reported as existing:
 *
 *   /mnt/disk/ is blindly assumed to exist
 *   /mnt/disk/.Trash must be a directory (not a symlink)
 *   /mnt/disk/.Trash/1000 must be a directory (not a symlink)
 *   /mnt/disk/.Trash/1000/files must be a directory (not a symlink)
 *
 * If any of these ceases to be true (even momentarily), the directory
 * will be reported as having been destroyed.  create() and destroy()
 * callbacks are never issued spuriously (ie: two calls to one
 * callback will never occur in a row).  Events where the directory
 * exists momentarily might be missed, but events where the directory
 * stops existing momentarily will (hopefully) always be reported.
 * The first call (if it happens) will always be to create().
 *
 * This implementation is currently tweaked a bit for how GFileMonitor
 * currently works with inotify.  If GFileMonitor's implementation is
 * changed it might be a good idea to take another look at this code.
 */

struct OPAQUE_TYPE__DirWatch
{
  GFile *directory;
  GFile *topdir;

  DirWatchFunc create;
  DirWatchFunc destroy;
  gpointer user_data;
  gboolean state;
  gboolean active;

  DirWatch *parent;

  GFileMonitor *parent_monitor;
};

#ifdef DIR_WATCH_DEBUG
# define dir_watch_created(watch) \
    G_STMT_START {                                              \
      char *path = g_file_get_path ((watch)->directory);        \
      g_print (">> created '%s'\n", path);                      \
      g_free (path);                                            \
      (watch)->create ((watch)->user_data);                     \
    } G_STMT_END

# define dir_watch_destroyed(watch) \
    G_STMT_START {                                              \
      char *path = g_file_get_path ((watch)->directory);        \
      g_print (">> destroyed '%s'\n", path);                    \
      g_free (path);                                            \
      (watch)->destroy ((watch)->user_data);                    \
    } G_STMT_END
#else
# define dir_watch_created(watch) (watch)->create ((watch)->user_data)
# define dir_watch_destroyed(watch) (watch)->destroy ((watch)->user_data)
#endif

#ifdef DIR_WATCH_DEBUG
#include <errno.h>
#include <string.h>
#endif

static gboolean
dir_exists (GFile *file)
{
  gboolean result;
  struct stat buf;
  char *path;

  path = g_file_get_path (file);
#ifdef DIR_WATCH_DEBUG
  errno = 0;
#endif
  result = !lstat (path, &buf) && S_ISDIR (buf.st_mode);

#ifdef DIR_WATCH_DEBUG
  g_print ("    lstat ('%s') -> is%s a directory (%s)\n",
           path, result ? "" : " not", strerror (errno));
#endif
  g_free (path);

  return result;
}

static void
dir_watch_parent_changed (GFileMonitor      *monitor,
                          GFile             *file,
                          GFile             *other_file,
                          GFileMonitorEvent  event_type,
                          gpointer           user_data)
{
  DirWatch *watch = user_data;

  g_assert (watch->parent_monitor == monitor);

  if (!g_file_equal (file, watch->directory))
    return;

  if (event_type == G_FILE_MONITOR_EVENT_CREATED)
    {
      if (watch->state)
        return;

      /* we were just created.  ensure that it's a directory. */
      if (dir_exists (file))
        {
          /* we're official now.  report it. */
          watch->state = TRUE;
          dir_watch_created (watch);
        }
    }
  else if (event_type == G_FILE_MONITOR_EVENT_DELETED)
    {
      if (!watch->state)
        return;

      watch->state = FALSE;
      dir_watch_destroyed (watch);
    }
}

static void
dir_watch_recursive_create (gpointer user_data)
{
  DirWatch *watch = user_data;
  GFile *parent;

  g_assert (watch->parent_monitor == NULL);

  parent = g_file_get_parent (watch->directory);
  watch->parent_monitor = g_file_monitor_directory (parent, 0,
                                                    NULL, NULL);
  g_object_unref (parent);
  g_signal_connect (watch->parent_monitor, "changed",
                    G_CALLBACK (dir_watch_parent_changed), watch);
  
  /* check if directory was created before we started to monitor */
  if (dir_exists (watch->directory))
    {
      watch->state = TRUE;
      dir_watch_created (watch);
    }
}

static void
dir_watch_recursive_destroy (gpointer user_data)
{
  DirWatch *watch = user_data;

  /* exactly one monitor should be active */
  g_assert (watch->parent_monitor != NULL);

  /* if we were monitoring the directory... */
  if (watch->state)
    {
      dir_watch_destroyed (watch);
      watch->state = FALSE;
    }

  g_object_unref (watch->parent_monitor);
  watch->parent_monitor = NULL;
}

DirWatch *
dir_watch_new (GFile        *directory,
               GFile        *topdir,
               gboolean      watching,
               DirWatchFunc  create,
               DirWatchFunc  destroy,
               gpointer      user_data)
{
  DirWatch *watch;

  watch = g_slice_new0 (DirWatch);
  watch->create = create;
  watch->destroy = destroy;
  watch->user_data = user_data;

  watch->directory = g_object_ref (directory);
  watch->topdir = g_object_ref (topdir);

  /* the top directory always exists */
  if (g_file_equal (directory, topdir))
    {
      if (watching)
        dir_watch_created (watch);

      watch->state = TRUE;
    }

  else
    {
      GFile *parent;

      parent = g_file_get_parent (directory);
      g_assert (parent != NULL);

      watch->parent = dir_watch_new (parent, topdir, watching,
                                     dir_watch_recursive_create,
                                     dir_watch_recursive_destroy,
                                     watch);

      g_object_unref (parent);

      if (!watching)
        watch->state = watch->parent->state && dir_exists (directory);
    }

  return watch;
}

void
dir_watch_free (DirWatch *watch)
{
  if (watch != NULL)
    {
      if (watch->parent_monitor)
        g_object_unref (watch->parent_monitor);

      g_object_unref (watch->directory);
      g_object_unref (watch->topdir);

      dir_watch_free (watch->parent);
    }
}

void
dir_watch_enable (DirWatch *watch)
{
  /* topdir always exists.  say so. */
  if (watch->parent == NULL)
    dir_watch_created (watch);

  else
    dir_watch_enable (watch->parent);
}

void
dir_watch_disable (DirWatch *watch)
{
  if (watch->parent_monitor)
    g_object_unref (watch->parent_monitor);

  watch->parent_monitor = NULL;

  if (watch->parent)
    dir_watch_disable (watch->parent);
}

gboolean
dir_watch_is_valid (DirWatch *watch)
{
  return watch->state;
}

gboolean
dir_watch_double_check (DirWatch *watch)
{
  gboolean old_state;

  old_state = watch->state;

  if (watch->parent == NULL)
    {
      g_assert (watch->state == TRUE);
      return TRUE;
    }

  if (dir_watch_double_check (watch->parent))
    {
      if (dir_watch_is_valid (watch->parent))
        watch->state = dir_exists (watch->directory);
      else
        watch->state = FALSE;


      if (!old_state && watch->state && watch->parent_monitor)
        dir_watch_created (watch);

      else if (old_state && !watch->state && watch->parent_monitor)
        dir_watch_destroyed (watch);

      else if (old_state || watch->state)
        return TRUE;
    }

  return FALSE;
}
