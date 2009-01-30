/*
 * Copyright © 2008 Ryan Lortie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.   
 */

#include "trashexpunge.h"

static gsize trash_expunge_initialised;
static GHashTable *trash_expunge_queue;
static gboolean trash_expunge_alive;
static GMutex *trash_expunge_lock;
static GCond *trash_expunge_wait;

static void
trash_expunge_delete_everything_under (GFile *directory)
{
  GFileEnumerator *enumerator;

  g_file_set_attribute_uint32 (directory,
                               G_FILE_ATTRIBUTE_UNIX_MODE, 0700,
                               G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                               NULL, NULL);

  enumerator = g_file_enumerate_children (directory,
                                          G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                          G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          NULL, NULL);

  if (enumerator)
    {
      GFileInfo *info;

      while ((info = g_file_enumerator_next_file (enumerator, NULL, NULL)))
        {
          const gchar *basename;
          GFile *sub;
          
          basename = g_file_info_get_name (info);
          sub = g_file_get_child (directory, basename);

          if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
            trash_expunge_delete_everything_under (sub);

          /* do the delete here */
          g_file_delete (sub, NULL, NULL);

          g_object_unref (info);
          g_object_unref (sub);
        }
      g_object_unref (enumerator);
    }
}

static gboolean
just_return_true (gpointer a,
                  gpointer b,
                  gpointer c)
{
  return TRUE;
}

static gpointer
trash_expunge_thread (gpointer data)
{
  GTimeVal timeval;

  g_mutex_lock (trash_expunge_lock);

  do
    {
      while (g_hash_table_size (trash_expunge_queue))
        {
          GFile *directory;
         
          directory = g_hash_table_find (trash_expunge_queue,
                                         just_return_true, NULL);
          g_hash_table_remove (trash_expunge_queue, directory);

          g_mutex_unlock (trash_expunge_lock);
          trash_expunge_delete_everything_under (directory);
          g_mutex_lock (trash_expunge_lock);

          g_object_unref (directory);
        }

      g_get_current_time (&timeval);
      g_time_val_add (&timeval, 60 * 1000000); /* 1min */
    }
  while (g_cond_timed_wait (trash_expunge_wait,
                            trash_expunge_lock,
                            &timeval));

  trash_expunge_alive = FALSE;

  g_mutex_unlock (trash_expunge_lock);

  return NULL;
}

void
trash_expunge (GFile *directory)
{
  if G_UNLIKELY (g_once_init_enter (&trash_expunge_initialised))
    {
      trash_expunge_queue = g_hash_table_new (g_file_hash,
                                              (GEqualFunc) g_file_equal);
      trash_expunge_lock = g_mutex_new ();
      trash_expunge_wait = g_cond_new ();

      g_once_init_leave (&trash_expunge_initialised, 1);
    }

  g_mutex_lock (trash_expunge_lock);

  if (!g_hash_table_lookup (trash_expunge_queue, directory))
    g_hash_table_insert (trash_expunge_queue,
                         g_object_ref (directory),
                         directory);

  if (trash_expunge_alive == FALSE)
    {
      GThread *thread;

      thread = g_thread_create (trash_expunge_thread, NULL, FALSE, NULL);
      g_assert (thread != NULL);
      trash_expunge_alive = TRUE;
    }
  else
    g_cond_signal (trash_expunge_wait);

  g_mutex_unlock (trash_expunge_lock);
}
