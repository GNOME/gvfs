/*
 * Copyright © 2008 Ryan Lortie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.   
 */

#include "trashexpunge.h"
#include "trashitem.h"

#include <glib/gstdio.h>

typedef struct
{
  trash_item_notify func;
  TrashItem *item;
  gpointer user_data;
} NotifyClosure;

struct OPAQUE_TYPE__TrashRoot
{
  GStaticRWLock lock;
  GQueue *notifications;

  trash_item_notify create_notify;
  trash_item_notify delete_notify;
  trash_size_change size_change;
  gpointer user_data;

  GHashTable *item_table;
  gboolean is_homedir;
  int old_size;
};

struct OPAQUE_TYPE__TrashItem
{
  TrashRoot *root;
  gint ref_count;

  char *escaped_name;
  GFile *file;

  GFile *original;
  char *delete_date;
};

static char *
trash_item_escape_name (GFile    *file,
                        gboolean  in_homedir)
{
  /*
   * make unique names as follows:
   *
   * - items in home directory use their basename (never starts with '/')
   *
   *     - if the basename starts with '\' then it is changed to '`\'
   *
   *     - if the basename starts with '`' then it is changed to '``'
   *
   *     - this means that home directory items never start with '\'
   *
   * - items in others use full path name (always starts with '/')
   *
   *     - each '/' (including the first) is changed to '\'
   *
   *     - this means that all of these items start with '\'
   *
   *     - each '\' is changed to '`\'
   *
   *     - each '`' is changed to '``'
   */
#define ESCAPE_SYMBOL1 '\\'
#define ESCAPE_SYMBOL2 '`'

  if (in_homedir)
    {
      char *basename;
      char *escaped;

      basename = g_file_get_basename (file);

      if (basename[0] != ESCAPE_SYMBOL1 && basename[0] != ESCAPE_SYMBOL2)
        return basename;

      escaped = g_strdup_printf ("%c%s", ESCAPE_SYMBOL2, basename);
      g_free (basename);

      return escaped;
    }
  else
    {
      char *uri, *src, *dest, *escaped;
      int need_bytes = 0;

      uri = g_file_get_uri (file);
      g_assert (g_str_has_prefix (uri, "file:///"));

      src = uri + 7; /* keep the first '/' */
      while (*src)
        {
          if (*src == ESCAPE_SYMBOL1 || *src == ESCAPE_SYMBOL2)
            need_bytes += 2;
          else
            need_bytes++;

          src++;
        }

      escaped = g_malloc (need_bytes + 1);

      dest = escaped;
      src = uri + 7;
      while (*src)
        {
          if (*src == ESCAPE_SYMBOL1 || *src == ESCAPE_SYMBOL2)
            {
              *dest++ = ESCAPE_SYMBOL2;
              *dest++ = *src;
            }
          else if (*src == '/')
            *dest++ = ESCAPE_SYMBOL1;
          else
            *dest++ = *src;

          src++;
        }

      g_free (uri);
      *dest = '\0';

      return escaped;
    }
}

static void
trash_item_get_trashinfo (GFile  *path,
                          GFile **original,
                          char  **date)
{
  GFile *files, *trashdir;
  GKeyFile *keyfile;
  char *trashpath;
  char *trashinfo;
  char *basename;

  files = g_file_get_parent (path);
  trashdir = g_file_get_parent (files);
  trashpath = g_file_get_path (trashdir);
  g_object_unref (files);

  basename = g_file_get_basename (path);

  trashinfo = g_strdup_printf ("%s/info/%s.trashinfo", trashpath, basename);
  g_free (trashpath);
  g_free (basename);

  keyfile = g_key_file_new ();

  *original = NULL;
  *date = NULL;

  if (g_key_file_load_from_file (keyfile, trashinfo, 0, NULL))
    {
      char *orig, *decoded;

      decoded = NULL;
      orig = g_key_file_get_string (keyfile,
                                    "Trash Info", "Path",
                                    NULL);

      if (orig == NULL)
        *original = NULL;
      else
	{
	  decoded = g_uri_unescape_string (orig, NULL);

	  if (g_path_is_absolute (decoded))
	    *original = g_file_new_for_path (decoded);
	  else
	    {
	      GFile *rootdir;
	      
	      rootdir = g_file_get_parent (trashdir);
	      *original = g_file_get_child (rootdir, decoded);
	      g_object_unref (rootdir);
	    }
	  g_free (decoded);
	}

      g_free (orig);

      *date = g_key_file_get_string (keyfile,
                                     "Trash Info", "DeletionDate",
                                     NULL);
    }

  g_object_unref (trashdir);
  g_key_file_free (keyfile);
  g_free (trashinfo);
}

static TrashItem *
trash_item_new (TrashRoot *root,
                GFile         *file,
                gboolean       in_homedir)
{
  TrashItem *item;

  item = g_slice_new (TrashItem);
  item->root = root;
  item->ref_count = 1;
  item->file = g_object_ref (file);
  item->escaped_name = trash_item_escape_name (file, in_homedir);
  trash_item_get_trashinfo (item->file, &item->original, &item->delete_date);

  return item;
}

static TrashItem *
trash_item_ref (TrashItem *item)
{
  g_atomic_int_inc (&item->ref_count);
  return item;
}

void
trash_item_unref (TrashItem *item)
{
  if (g_atomic_int_dec_and_test (&item->ref_count))
    {
      g_object_unref (item->file);

      if (item->original)
        g_object_unref (item->original);

      g_free (item->delete_date);
      g_free (item->escaped_name);

      g_slice_free (TrashItem, item);
    }
}

const char *
trash_item_get_escaped_name (TrashItem *item)
{
  return item->escaped_name;
}

const char *
trash_item_get_delete_date (TrashItem *item)
{
  return item->delete_date;
}

GFile *
trash_item_get_original (TrashItem *item)
{
  return item->original;
}

GFile *
trash_item_get_file (TrashItem *item)
{
  return item->file;
}

static void
trash_item_queue_notify (TrashItem         *item,
                         trash_item_notify  func)
{
  NotifyClosure *closure;

  closure = g_slice_new (NotifyClosure);
  closure->func = func;
  closure->item = trash_item_ref (item);
  closure->user_data = item->root->user_data;

  g_queue_push_tail (item->root->notifications, closure);
}

static void
trash_item_invoke_closure (NotifyClosure *closure)
{
  closure->func (closure->item, closure->user_data);
  trash_item_unref (closure->item);
  g_slice_free (NotifyClosure, closure);
}

void
trash_root_thaw (TrashRoot *root)
{
  NotifyClosure *closure;
  gboolean size_changed;
  int size;

  /* send notifications until we have none */
  while (TRUE)
    {
      g_static_rw_lock_writer_lock (&root->lock);
      if (g_queue_is_empty (root->notifications))
        break;

      closure = g_queue_pop_head (root->notifications);
      g_static_rw_lock_writer_unlock (&root->lock);

      trash_item_invoke_closure (closure);
    }

  /* still holding lock... */
  size = g_hash_table_size (root->item_table);
  size_changed = root->old_size != size;
  root->old_size = size;

  g_static_rw_lock_writer_unlock (&root->lock);

  if (size_changed)
    root->size_change (root->user_data);
}

static void
trash_item_removed (gpointer data)
{
  TrashItem *item = data;

  trash_item_queue_notify (item, item->root->delete_notify);
  trash_item_unref (item);
}

TrashRoot *
trash_root_new (trash_item_notify create,
                trash_item_notify delete,
                trash_size_change size_change,
                gpointer          user_data)
{
  TrashRoot *root;

  root = g_slice_new (TrashRoot);
  g_static_rw_lock_init (&root->lock);
  root->create_notify = create;
  root->delete_notify = delete;
  root->size_change = size_change;
  root->user_data = user_data;
  root->notifications = g_queue_new ();
  root->item_table = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            NULL, trash_item_removed);
  root->old_size = 0;

  return root;
}

void
trash_root_free (TrashRoot *root)
{
  g_hash_table_destroy (root->item_table);

  while (!g_queue_is_empty (root->notifications))
    {
      NotifyClosure *closure;
      
      closure = g_queue_pop_head (root->notifications);
      trash_item_unref (closure->item);
      g_slice_free (NotifyClosure, closure);
    }
  g_queue_free (root->notifications);

  g_slice_free (TrashRoot, root);
}

void
trash_root_add_item (TrashRoot *list,
                     GFile     *file,
                     gboolean   in_homedir)
{
  TrashItem *item;

  item = trash_item_new (list, file, in_homedir);

  g_static_rw_lock_writer_lock (&list->lock);

  if (g_hash_table_lookup (list->item_table, item->escaped_name))
    {
      g_static_rw_lock_writer_unlock (&list->lock);

      /* already exists... */
      trash_item_unref (item);
      return;
    }

  g_hash_table_insert (list->item_table, item->escaped_name, item);
  trash_item_queue_notify (item, item->root->create_notify);

  g_static_rw_lock_writer_unlock (&list->lock);
}

void
trash_root_remove_item (TrashRoot *list,
                        GFile     *file,
                        gboolean   in_homedir)
{
  char *escaped;

  escaped = trash_item_escape_name (file, in_homedir);

  g_static_rw_lock_writer_lock (&list->lock);
  g_hash_table_remove (list->item_table, escaped);
  g_static_rw_lock_writer_unlock (&list->lock);

  g_free (escaped);
}

GList *
trash_root_get_items (TrashRoot *root)
{
  GList *items, *node;

  g_static_rw_lock_reader_lock (&root->lock);

  items = g_hash_table_get_values (root->item_table);
  for (node = items; node; node = node->next)
    trash_item_ref (node->data);

  g_static_rw_lock_reader_unlock (&root->lock);

  return items;
}

void
trash_item_list_free (GList *list)
{
  GList *node;

  for (node = list; node; node = node->next)
    trash_item_unref (node->data);
  g_list_free (list);
}

TrashItem *
trash_root_lookup_item (TrashRoot  *root,
                        const char *escaped)
{
  TrashItem *item;

  g_static_rw_lock_reader_lock (&root->lock);

  if ((item = g_hash_table_lookup (root->item_table, escaped)))
    trash_item_ref (item);

  g_static_rw_lock_reader_unlock (&root->lock);

  return item;
}

int
trash_root_get_n_items (TrashRoot *root)
{
  int size;

  g_static_rw_lock_reader_lock (&root->lock);
  size = g_hash_table_size (root->item_table);
  g_static_rw_lock_reader_unlock (&root->lock);

  return size;
}

gboolean
trash_item_delete (TrashItem  *item,
                   GError    **error)
{
  gboolean success;
  GFile *expunged;
  guint unique;
  guint i;

  expunged = g_file_resolve_relative_path (item->file,
                                           "../../expunged");
  g_file_make_directory_with_parents (expunged, NULL, NULL);
  unique = g_random_int ();

  for (success = FALSE, i = 0; !success && i < 1000; i++)
    {
      GFile *temp_name;
      char buffer[16];

      g_sprintf (buffer, "%u", unique + i);
      temp_name = g_file_get_child (expunged, buffer);

      /* "restore" the item into the expunged folder */
      if (trash_item_restore (item, temp_name,
			      G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS,
			      NULL))
        {
          trash_expunge (expunged);
          success = TRUE;
        }

      g_object_unref (temp_name);
    }

  g_object_unref (expunged);

  if (!success)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "Failed to delete the item from the trash");

  return success;
}

gboolean
trash_item_restore (TrashItem  *item,
                    GFile      *dest,
		    GFileCopyFlags flags,
                    GError    **error)
{
  if (g_file_move (item->file, dest,
		   flags |
                   G_FILE_COPY_NO_FALLBACK_FOR_MOVE,
                   NULL, NULL, NULL, error))
    {
      g_static_rw_lock_writer_lock (&item->root->lock);
      g_hash_table_remove (item->root->item_table, item->escaped_name);
      g_static_rw_lock_writer_unlock (&item->root->lock);

      {
        GFile *trashinfo;
        gchar *basename;
        gchar *relname;

        basename = g_file_get_basename (item->file);
        relname = g_strdup_printf ("../../info/%s.trashinfo", basename);
        trashinfo = g_file_resolve_relative_path (item->file, relname);
        g_free (basename);
        g_free (relname);

        g_file_delete (trashinfo, NULL, NULL);
        g_object_unref (trashinfo);
      }

      return TRUE;
    }

  return FALSE;
}
