/*
 * Copyright © 2008 Ryan Lortie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.   
 */

#include "trashwatcher.h"

#include <gio/gunixmounts.h>
#include <gio/gio.h>
#include <unistd.h>
#include <string.h>

#include "trashitem.h"
#include "trashdir.h"

typedef enum
{
  TRASH_WATCHER_TRUSTED,
  TRASH_WATCHER_WATCH,
  TRASH_WATCHER_NO_WATCH,
} WatchType;

/* decide_watch_type:
 *
 * This function is responsible for determining what sort of watching
 * we should do on a given mountpoint according to the type of
 * filesystem.  It must return one of the WatchType constants above.
 *
 *   TRASH_WATCHER_TRUSTED:
 *
 *     This is used for filesystems on which notification is supported
 *     and all file events are reliably reported.  After initialisation
 *     the trash directories are never manually rescanned since any
 *     changes are already known to us from the notifications we
 *     received about them (that's where the "trust" comes in).
 *
 *     This should be used for local filesystems such as ext3.
 *
 *   TRASH_WATCHER_WATCH:
 *
 *     This is used for filesystems on which notification is supported
 *     but works unreliably.  Some changes to the filesystem may not
 *     be delivered by the operating system.  The events which are
 *     delivered are immediately reported but events which are not
 *     delivered are not reported until the directory is manually
 *     rescanned (ie: trash_watcher_rescan() is called).
 *
 *     This should be used for filesystems like NFS where local
 *     changes are reported by the kernel but changes made on other
 *     hosts are not.
 *
 *   TRASH_WATCHER_NO_WATCH:
 *
 *     Don't bother watching at all.  No change events are ever
 *     delivered except while running trash_watcher_rescan().
 *
 *     This should be used for filesystems where change notification
 *     is unsupported or is supported, but buggy enough to cause
 *     problems when using the other two options.
 */
static WatchType
decide_watch_type (GUnixMountEntry *mount,
                   gboolean         is_home_trash)
{
  const gchar *fs_type;
  const gchar *mount_path;

  /* Let's assume that home trash is trusted if mount wasn't found.
   * https://bugzilla.gnome.org/show_bug.cgi?id=747540
   */
  if (mount == NULL)
    return TRASH_WATCHER_TRUSTED;

  mount_path = g_unix_mount_entry_get_mount_path (mount);

  /* Do not care about mount points without read access to avoid polling, see:
   * https://bugzilla.gnome.org/show_bug.cgi?id=522314
   */
  if (access (mount_path, R_OK) != 0)
    return TRASH_WATCHER_NO_WATCH;

  fs_type = g_unix_mount_entry_get_fs_type (mount);

  if (g_strcmp0 (fs_type, "nfs") == 0 ||
      g_strcmp0 (fs_type, "nfs4") == 0 ||
      g_strcmp0 (fs_type, "cifs") == 0)
    return TRASH_WATCHER_WATCH;
  else
    return TRASH_WATCHER_TRUSTED;
}

/* find the mount entry for the directory containing 'file'.
 * used to figure out what sort of filesystem the home trash
 * folder is sitting on.
 */
static GUnixMountEntry *
find_mount_entry_for_file (GFile *file)
{
  GUnixMountEntry *entry;
  char *pathname;

  pathname = g_file_get_path (file);
  do
    {
      char *slash;

      slash = strrchr (pathname, '/');

      /* leave the leading '/' in place */
      if (slash == pathname)
        slash++;

      *slash = '\0';

      entry = g_unix_mount_entry_for (pathname, NULL);
    }
  while (entry == NULL && pathname[1]);

  g_free (pathname);

  /* Entry might not be found e.g. for bind mounts, btrfs subvolumes...
   * https://bugzilla.gnome.org/show_bug.cgi?id=747540
   */
  if (entry == NULL)
    {
      pathname = g_file_get_path (file);
      g_warning ("Mount entry was not found for %s", pathname);
      g_free (pathname);
    }

  return entry;
}

typedef struct _TrashMount TrashMount;

struct OPAQUE_TYPE__TrashWatcher
{
  TrashRoot *root;

  GUnixMountMonitor *mount_monitor;
  TrashMount *mounts;

  TrashDir *homedir_trashdir;
  WatchType homedir_type;

  gboolean watching;
};

struct _TrashMount
{
  GUnixMountEntry *mount_entry;
  TrashDir *dirs[2];
  WatchType type;

  TrashMount *next;
};

static void
trash_mount_insert (TrashWatcher      *watcher,
                    TrashMount      ***mount_ptr_ptr,
                    GUnixMountEntry   *mount_entry)
{
  const char *mountpoint;
  gboolean watching;
  TrashMount *mount;

  mountpoint = g_unix_mount_entry_get_mount_path (mount_entry);

  mount = g_slice_new (TrashMount);
  mount->mount_entry = mount_entry;
  mount->type = decide_watch_type (mount_entry, FALSE);

  watching = watcher->watching && mount->type != TRASH_WATCHER_NO_WATCH;

  /* """
   *   For showing trashed files, implementations SHOULD support (1) and
   *   (2) at the same time (i.e. if both $topdir/.Trash/$uid and
   *   $topdir/.Trash-$uid are present, it should list trashed files
   *   from both of them).
   * """
   */

  /* (1) */
  mount->dirs[0] = trash_dir_new (watcher->root, watching, FALSE, mountpoint,
                                  ".Trash/%d/files", (int) getuid ());

  /* (2) */
  mount->dirs[1] = trash_dir_new (watcher->root, watching, FALSE, mountpoint,
                                  ".Trash-%d/files", (int) getuid ());

  mount->next = **mount_ptr_ptr;

  **mount_ptr_ptr = mount;
  *mount_ptr_ptr = &mount->next;
}

static void
trash_mount_remove (TrashMount **mount_ptr)
{
  TrashMount *mount = *mount_ptr;

  /* first, the dirs */
  trash_dir_free (mount->dirs[0]);
  trash_dir_free (mount->dirs[1]);

  /* detach from list */
  *mount_ptr = mount->next;

  g_unix_mount_entry_free (mount->mount_entry);
  g_slice_free (TrashMount, mount);
}

static gboolean
ignore_trash_mount (GUnixMountEntry *mount)
{
  GUnixMountPoint *mount_point = NULL;
  const gchar *mount_options;

  mount_options = g_unix_mount_entry_get_options (mount);
  if (mount_options == NULL)
    {
      const gchar *mount_path = g_unix_mount_entry_get_mount_path (mount);

      mount_point = g_unix_mount_point_at (mount_path, NULL);
      if (mount_point != NULL)
        mount_options = g_unix_mount_point_get_options (mount_point);

      g_clear_pointer (&mount_point, g_unix_mount_point_free);
    }

  if (mount_options != NULL)
    {
      if (strstr (mount_options, "x-gvfs-trash") != NULL)
        return FALSE;

      if (strstr (mount_options, "x-gvfs-notrash") != NULL)
        return TRUE;
    }

  if (g_unix_mount_entry_is_system_internal (mount))
    return TRUE;

  return FALSE;
}

static void
trash_watcher_remount (TrashWatcher *watcher)
{
  TrashMount **old;
  GList *mounts;
  GList *new;

  mounts = g_unix_mount_entries_get (NULL);
  mounts = g_list_sort (mounts, (GCompareFunc) g_unix_mount_entry_compare);

  old = &watcher->mounts;
  new = mounts;

  /* synchronise the two lists */
  while (*old || new)
    {
      int result;

      if (new && ignore_trash_mount (new->data))
        {
          g_unix_mount_entry_free (new->data);
          new = new->next;
          continue;
        }

      if ((result = (new == NULL) - (*old == NULL)) == 0)
        result = g_unix_mount_entry_compare (new->data, (*old)->mount_entry);

      if (result < 0)
        {
          /* new entry.  add it. */
          trash_mount_insert (watcher, &old, new->data);
          new = new->next;
        }
      else if (result > 0)
        {
          /* old entry.  remove it. */
          trash_mount_remove (old);
        }
      else
        {
          /* match.  no change. */
          g_unix_mount_entry_free (new->data);

          old = &(*old)->next;
          new = new->next;
        }
    }

  g_list_free (mounts);
}

TrashWatcher *
trash_watcher_new (TrashRoot *root)
{
  GUnixMountEntry *homedir_mount;
  GFile *homedir_trashdir;
  TrashWatcher *watcher;
  GFile *user_datadir;

  watcher = g_slice_new (TrashWatcher);
  watcher->root = root;
  watcher->mounts = NULL;
  watcher->watching = FALSE;
  watcher->mount_monitor = g_unix_mount_monitor_get ();
  g_signal_connect_swapped (watcher->mount_monitor, "mounts_changed",
                            G_CALLBACK (trash_watcher_remount), watcher);

  user_datadir = g_file_new_for_path (g_get_user_data_dir ());
  homedir_trashdir = g_file_get_child (user_datadir, "Trash/files");
  homedir_mount = find_mount_entry_for_file (homedir_trashdir);
  watcher->homedir_type = decide_watch_type (homedir_mount, TRUE);
  watcher->homedir_trashdir = trash_dir_new (watcher->root,
                                             FALSE, TRUE,
                                             g_get_user_data_dir (),
                                             "Trash/files");

  if (homedir_mount)
    g_unix_mount_entry_free (homedir_mount);
  g_object_unref (homedir_trashdir);
  g_object_unref (user_datadir);

  trash_watcher_remount (watcher);

  return watcher;
}

void
trash_watcher_free (TrashWatcher *watcher)
{
  /* We just leak everything here, as this is not normally hit.
     This used to be a g_assert_not_reached(), and that got hit when
     mounting the trash backend failed due to the trash already being
     mounted. */
}

void
trash_watcher_watch (TrashWatcher *watcher)
{
  TrashMount *mount;

  g_assert (!watcher->watching);

  if (watcher->homedir_type != TRASH_WATCHER_NO_WATCH)
    trash_dir_watch (watcher->homedir_trashdir);

  for (mount = watcher->mounts; mount; mount = mount->next)
    if (mount->type != TRASH_WATCHER_NO_WATCH)
      {
        trash_dir_watch (mount->dirs[0]);
        trash_dir_watch (mount->dirs[1]);
      }

  watcher->watching = TRUE;
}

void
trash_watcher_unwatch (TrashWatcher *watcher)
{
  TrashMount *mount;

  g_assert (watcher->watching);

  if (watcher->homedir_type != TRASH_WATCHER_NO_WATCH)
    trash_dir_unwatch (watcher->homedir_trashdir);

  for (mount = watcher->mounts; mount; mount = mount->next)
    if (mount->type != TRASH_WATCHER_NO_WATCH)
      {
        trash_dir_unwatch (mount->dirs[0]);
        trash_dir_unwatch (mount->dirs[1]);
      }

  watcher->watching = FALSE;
}

void
trash_watcher_rescan (TrashWatcher *watcher)
{
  TrashMount *mount;

  if (!watcher->watching || watcher->homedir_type != TRASH_WATCHER_TRUSTED)
    trash_dir_rescan (watcher->homedir_trashdir);

  for (mount = watcher->mounts; mount; mount = mount->next)
    if (!watcher->watching || mount->type != TRASH_WATCHER_TRUSTED)
      {
        trash_dir_rescan (mount->dirs[0]);
        trash_dir_rescan (mount->dirs[1]);
      }
}
