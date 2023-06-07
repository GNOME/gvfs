/*
 * Copyright © 2008 Ryan Lortie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * See the included COPYING file for more information.
 */

#include "gvfsbackendtrash.h"

#include <glib/gi18n.h> /* _() */
#include <string.h>

#include "trashlib/trashwatcher.h"
#include "trashlib/trashitem.h"

#include "gvfsjobcreatemonitor.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobseekread.h"
#include "gvfsjobqueryinforead.h"
#include "gvfsjobread.h"
#include "gvfsdaemonutils.h"

typedef GVfsBackendClass GVfsBackendTrashClass;

struct OPAQUE_TYPE__GVfsBackendTrash
{
  GVfsBackend parent_instance;

  GVfsMonitor *file_monitor;
  GVfsMonitor *dir_monitor;

  GMainContext *worker_context;
  GMainLoop *worker_loop;
  GThread *worker_thread;

  TrashWatcher *watcher;
  TrashRoot *root;

  guint thaw_timeout_id;
};

G_DEFINE_TYPE (GVfsBackendTrash, g_vfs_backend_trash, G_VFS_TYPE_BACKEND);

typedef struct
{
  GSourceFunc source_func;
  gpointer user_data;
  GMutex mutex;
  GCond cond;
  gboolean completed;
} ContextInvokeData;

static gboolean
source_func_wrapper (gpointer user_data)
{
  ContextInvokeData *data = user_data;

  g_mutex_lock (&data->mutex);

  while (data->source_func (data->user_data));
  data->completed = TRUE;

  g_cond_signal (&data->cond);
  g_mutex_unlock (&data->mutex);

  return G_SOURCE_REMOVE;
}

static void
trash_backend_worker_thread_queue_and_wait (GVfsBackendTrash *backend,
                                            GSourceFunc       source_func)
{
  ContextInvokeData data;

  data.source_func = source_func;
  data.user_data = backend;

  g_mutex_init (&data.mutex);
  g_cond_init (&data.cond);
  data.completed = FALSE;

  g_mutex_lock (&data.mutex);

  g_main_context_invoke (backend->worker_context,
                         source_func_wrapper,
                         &data);

  while (!data.completed)
    {
      g_cond_wait (&data.cond, &data.mutex);
    }

  g_mutex_unlock (&data.mutex);

  g_mutex_clear (&data.mutex);
  g_cond_clear (&data.cond);
}

static void
trash_backend_worker_thread_queue (GVfsBackendTrash *backend,
                                   GSourceFunc       source_func)
{
  g_main_context_invoke (backend->worker_context, source_func, backend);
}

static gboolean
watch_func (gpointer user_data)
{
  GVfsBackendTrash *backend = G_VFS_BACKEND_TRASH (user_data);

  trash_watcher_watch (backend->watcher);

  return G_SOURCE_REMOVE;
}

static gboolean
rescan_func (gpointer user_data)
{
  GVfsBackendTrash *backend = G_VFS_BACKEND_TRASH (user_data);

  trash_watcher_rescan (backend->watcher);

  return G_SOURCE_REMOVE;
}

static gboolean
ready_func (gpointer user_data)
{
  return G_SOURCE_REMOVE;
}

static gboolean
is_root (const char *filename)
{
  return (filename[0] == '/' && filename[1] == '\0');
}

static GVfsMonitor *
trash_backend_get_file_monitor (GVfsBackendTrash  *backend,
                                gboolean           create)
{
  if (backend->file_monitor == NULL && create == FALSE)
    return NULL;

  else if (backend->file_monitor == NULL)
    {
      /* 'create' is only ever set in the main thread, so we will have
       * no possibility here for creating more than one new monitor.
       */
      if (backend->dir_monitor == NULL)
        trash_backend_worker_thread_queue (backend, watch_func);

      backend->file_monitor = g_vfs_monitor_new (G_VFS_BACKEND (backend));
    }

  return g_object_ref (backend->file_monitor);
}

static GVfsMonitor *
trash_backend_get_dir_monitor (GVfsBackendTrash *backend,
                               gboolean          create)
{
  if (backend->dir_monitor == NULL && create == FALSE)
    return NULL;

  else if (backend->dir_monitor == NULL)
    {
      /* 'create' is only ever set in the main thread, so we will have
       * no possibility here for creating more than one new monitor.
       */
      if (backend->file_monitor == NULL)
        trash_backend_worker_thread_queue (backend, watch_func);

      backend->dir_monitor = g_vfs_monitor_new (G_VFS_BACKEND (backend));
    }

  return g_object_ref (backend->dir_monitor);
}

static void
trash_backend_item_created (TrashItem *item,
                            gpointer   user_data)
{
  GVfsBackendTrash *backend = user_data;
  GVfsMonitor *monitor;

  monitor = trash_backend_get_dir_monitor (backend, FALSE);

  if (monitor)
    {
      char *slashname;

      slashname = g_strconcat ("/", trash_item_get_escaped_name (item), NULL);

      g_vfs_monitor_emit_event (monitor, G_FILE_MONITOR_EVENT_CREATED,
                                slashname, NULL);
      g_object_unref (monitor);
      g_free (slashname);
    }
}

static void
trash_backend_item_deleted (TrashItem *item,
                            gpointer   user_data)
{
  GVfsBackendTrash *backend = user_data;
  GVfsMonitor *monitor;

  monitor = trash_backend_get_dir_monitor (backend, FALSE);

  if (monitor)
    {
      char *slashname;

      slashname = g_strconcat ("/", trash_item_get_escaped_name (item), NULL);
      g_vfs_monitor_emit_event (monitor, G_FILE_MONITOR_EVENT_DELETED,
                                slashname, NULL);
      g_object_unref (monitor);
      g_free (slashname);
    }
}

static void
trash_backend_item_count_changed (gpointer user_data)
{
  GVfsBackendTrash *backend = user_data;
  GVfsMonitor *file_monitor;
  GVfsMonitor *dir_monitor;

  file_monitor = trash_backend_get_file_monitor (backend, FALSE);
  dir_monitor = trash_backend_get_dir_monitor (backend, FALSE);

  if (file_monitor)
    {
      g_vfs_monitor_emit_event (file_monitor,
                                G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED,
                                "/", NULL);

      g_object_unref (file_monitor);
    }

  if (dir_monitor)
    {
      g_vfs_monitor_emit_event (dir_monitor,
                                G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED,
                                "/", NULL);

      g_object_unref (dir_monitor);
    }
}

static GFile *
trash_backend_get_file (GVfsBackendTrash  *backend,
                        const char        *filename,
                        TrashItem        **item_ret,
                        gboolean          *is_toplevel,
                        GError           **error)
{
  const char *slash;
  gboolean is_top;
  TrashItem *item;
  GFile *file;

  trash_backend_worker_thread_queue_and_wait (backend, rescan_func);

  file = NULL;
  filename++;

  slash = strchr (filename, '/');
  is_top = slash == NULL;

  if (is_toplevel)
    *is_toplevel = is_top;

  if (!is_top)
    {
      char *toplevel;

      g_assert (slash[1]);

      toplevel = g_strndup (filename, slash - filename);
      if ((item = trash_root_lookup_item (backend->root, toplevel)))
        {
          file = trash_item_get_file (item);
          file = g_file_get_child (file, slash + 1);

          if (item_ret)
            *item_ret = item;
          else
            trash_item_unref (item);
        }

      g_free (toplevel);
    }
  else
    {
      if ((item = trash_root_lookup_item (backend->root, filename)))
        {
          file = g_object_ref (trash_item_get_file (item));

          if (item_ret)
            *item_ret = item;
          else
            trash_item_unref (item);
        }
    }

  if (file == NULL)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                         _("No such file or directory"));

  return file;
}

/* ======================= method implementations ======================= */
static void
trash_backend_open_for_read (GVfsBackend        *vfs_backend,
                             GVfsJobOpenForRead *job,
                             const char         *filename)
{
  GVfsBackendTrash *backend = G_VFS_BACKEND_TRASH (vfs_backend);
  GError *error = NULL;

  if (is_root (filename))
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                         _("Can’t open directory"));

  else
    {
      GFile *real;

      real = trash_backend_get_file (backend, filename, NULL, NULL, &error);

      if (real)
        {
          GFileInputStream *stream;

          stream = g_file_read (real, G_VFS_JOB (job)->cancellable, &error);
          g_object_unref (real);
      
          if (stream)
            {
              g_vfs_job_open_for_read_set_handle (job, stream);
              g_vfs_job_open_for_read_set_can_seek (job, TRUE);
              g_vfs_job_succeeded (G_VFS_JOB (job));

              return;
            }
        }
    }

  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
trash_backend_read (GVfsBackend       *backend,
                    GVfsJobRead       *job,
                    GVfsBackendHandle  handle,
                    char              *buffer,
                    gsize              bytes_requested)
{
  GError *error = NULL;
  gssize bytes;

  bytes = g_input_stream_read (handle, buffer, bytes_requested,
                               G_VFS_JOB (job)->cancellable, &error);

  if (bytes >= 0)
    {
      g_vfs_job_read_set_size (job, bytes);
      g_vfs_job_succeeded (G_VFS_JOB (job));

      return;
    }

  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
trash_backend_seek_on_read (GVfsBackend       *backend,
                            GVfsJobSeekRead   *job,
                            GVfsBackendHandle  handle,
                            goffset            offset,
                            GSeekType          type) 
{
  GError *error = NULL;

  if (g_seekable_seek (handle, offset, type, NULL, &error))
    {
      g_vfs_job_seek_read_set_offset (job, g_seekable_tell (handle));
      g_vfs_job_succeeded (G_VFS_JOB (job));

      return;
    }

  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
trash_backend_query_info_on_read (GVfsBackend           *backend,
                                  GVfsJobQueryInfoRead  *job,
                                  GVfsBackendHandle      handle,
                                  GFileInfo             *info,
                                  GFileAttributeMatcher *matcher)
{
  GError *error = NULL;
  GFileInfo *real_info;

  real_info = g_file_input_stream_query_info (handle,
                                              job->attributes,
                                              G_VFS_JOB (job)->cancellable,
                                              &error);
  if (real_info)
    {
      g_file_info_copy_into (real_info, info);
      g_vfs_job_succeeded (G_VFS_JOB (job));
      g_object_unref (real_info);
    }
  else
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static void
trash_backend_close_read (GVfsBackend       *backend,
                          GVfsJobCloseRead  *job,
                          GVfsBackendHandle  handle)
{
  GError *error = NULL;

  if (g_input_stream_close (handle, G_VFS_JOB (job)->cancellable, &error))
    {
      g_vfs_job_succeeded (G_VFS_JOB (job));
      g_object_unref (handle);

      return;
    }

  g_object_unref (handle);

  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static gboolean
trash_backend_thaw_callback (gpointer user_data)
{
  GVfsBackendTrash *backend = user_data;

  trash_root_thaw (backend->root);

  backend->thaw_timeout_id = 0;
  return FALSE;
}

static void
trash_backend_schedule_thaw (GVfsBackendTrash *backend)
{
  if (backend->thaw_timeout_id)
    g_source_remove (backend->thaw_timeout_id);

  backend->thaw_timeout_id = g_timeout_add (200,
                                            trash_backend_thaw_callback,
                                            backend);
}

static void
trash_backend_delete (GVfsBackend   *vfs_backend,
                      GVfsJobDelete *job,
                      const char    *filename)
{
  GVfsBackendTrash *backend = G_VFS_BACKEND_TRASH (vfs_backend);
  GError *error = NULL;
  g_debug ("before job: %d\n", G_OBJECT(job)->ref_count);

  if (is_root (filename))
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                         _("The trash folder may not be deleted"));
  else
    {
      gboolean is_toplevel;
      TrashItem *item;
      GFile *real;

      real = trash_backend_get_file (backend, filename,
                                     &item, &is_toplevel, &error);

      if (real)
        {
          /* not interested in the 'real', but the item */
          g_object_unref (real);

          if (!is_toplevel)
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                 _("Items in the trash may not be modified"));

          else
            {
              if (trash_item_delete (item, &error))
                {
                  trash_backend_schedule_thaw (backend);
                  g_vfs_job_succeeded (G_VFS_JOB (job));
                  trash_item_unref (item);

                  return;
                }
            }

          trash_item_unref (item);
        }
    }

  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
trash_backend_pull (GVfsBackend           *vfs_backend,
                    GVfsJobPull           *job,
                    const gchar           *source,
                    const gchar           *local_path,
                    GFileCopyFlags         flags,
                    gboolean               remove_source,
                    GFileProgressCallback  progress_callback,
                    gpointer               progress_callback_data)
{
  GVfsBackendTrash *backend = G_VFS_BACKEND_TRASH (vfs_backend);
  GError *error = NULL;

  if (is_root (source))
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                         _("The trash folder may not be deleted"));
  else
    {
      gboolean is_toplevel;
      TrashItem *item;
      GFile *real;

      real = trash_backend_get_file (backend, source, &item,
                                     &is_toplevel, &error);

      if (real)
        {
          if (remove_source && !is_toplevel)
            g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                 _("Items in the trash may not be modified"));

          else
            {
              GFile *destination;
              gboolean it_worked;

              destination = g_file_new_for_path (local_path);

              if (remove_source)
                it_worked = trash_item_restore (item, destination, flags, &error);
              else
                it_worked = g_file_copy (real, destination, flags, 
                                         G_VFS_JOB (job)->cancellable, 
                                         progress_callback, progress_callback_data, &error);

              g_object_unref (destination);

              if (it_worked)
                {
                  g_vfs_job_succeeded (G_VFS_JOB (job));
                  trash_item_unref (item);
                  g_object_unref (real);

                  return;
                }
            }

          trash_item_unref (item);
          g_object_unref (real);
        }
 
    }

  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
trash_backend_add_info (TrashItem *item,
                        GFileInfo *info,
                        gboolean   is_toplevel)
{
  if (is_toplevel)
    {
      const gchar *delete_date;
      GFile *original, *real;

      g_assert (item != NULL);

      original = trash_item_get_original (item);

      if (original)
        {
          gchar *edit_name, *path;

          path = g_file_get_path (original);
          edit_name = gvfs_file_info_populate_names_as_local (info, path);

          g_file_info_set_attribute_byte_string (info,
                                                 G_FILE_ATTRIBUTE_TRASH_ORIG_PATH,
                                                 path);
          g_free (edit_name);
          g_free (path);
        }

      real = trash_item_get_file (item);

      if (real)
        {
          char *uri;

          uri = g_file_get_uri (real);
          g_file_info_set_attribute_string (info,
                                            G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,
                                            uri);
          g_free (uri);
        }

      delete_date = trash_item_get_delete_date (item);

      if (delete_date)
        g_file_info_set_attribute_string (info,
                                          G_FILE_ATTRIBUTE_TRASH_DELETION_DATE,
                                          delete_date);
    }

  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
                                     FALSE);
  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
                                     FALSE);
  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME,
                                     FALSE);
  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH,
                                     FALSE);
  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE,
                                     is_toplevel);
}

static void
trash_backend_enumerate_root (GVfsBackendTrash      *backend,
                              GVfsJobEnumerate      *job,
                              GFileAttributeMatcher *attribute_matcher,
                              GFileQueryInfoFlags    flags)
{
  GList *items, *node;

  g_vfs_job_succeeded (G_VFS_JOB (job));

  trash_backend_worker_thread_queue_and_wait (backend, rescan_func);

  items = trash_root_get_items (backend->root);

  for (node = items; node; node = node->next)
    {
      TrashItem *item = node->data;
      GFileInfo *info;

      info = g_file_query_info (trash_item_get_file (item),
                                job->attributes, flags,
                                G_VFS_JOB (job)->cancellable, NULL);

      if (info)
        {
          g_file_info_set_attribute_mask (info, attribute_matcher);

          g_file_info_set_name (info, trash_item_get_escaped_name (item));
          trash_backend_add_info (item, info, TRUE);

          g_vfs_job_enumerate_add_info (job, info);
          g_object_unref (info);
        }

      trash_item_unref (item);
    }

  g_vfs_job_enumerate_done (job);
  g_list_free (items);
}

static void
trash_backend_enumerate_non_root (GVfsBackendTrash      *backend,
                                  GVfsJobEnumerate      *job,
                                  const gchar           *filename,
                                  GFileAttributeMatcher *attribute_matcher,
                                  GFileQueryInfoFlags    flags)
{
  GError *error = NULL;
  GFile *real;

  real = trash_backend_get_file (backend, filename, NULL, NULL, &error);

  if (real)
    {
      GFileEnumerator *enumerator;

      enumerator = g_file_enumerate_children (real, job->attributes,
                                              job->flags, 
                                              G_VFS_JOB (job)->cancellable, &error);
      g_object_unref (real);

      if (enumerator)
        {
          GFileInfo *info;

          g_vfs_job_succeeded (G_VFS_JOB (job));

          while ((info = g_file_enumerator_next_file (enumerator,
                                                      G_VFS_JOB (job)->cancellable,
                                                      &error)))
            {
              trash_backend_add_info (NULL, info, FALSE);
              g_vfs_job_enumerate_add_info (job, info);
              g_object_unref (info);
            }

          /* error from next_file?  ignore. */
          if (error)
            g_error_free (error);

          g_vfs_job_enumerate_done (job);
          g_object_unref (enumerator);
          return;
        }
    }

  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);
}

static void
trash_backend_enumerate (GVfsBackend           *vfs_backend,
                         GVfsJobEnumerate      *job,
                         const char            *filename,
                         GFileAttributeMatcher *attribute_matcher,
                         GFileQueryInfoFlags    flags)
{
  GVfsBackendTrash *backend = G_VFS_BACKEND_TRASH (vfs_backend);

  g_assert (filename[0] == '/');

  if (!is_root (filename))
    trash_backend_enumerate_non_root (backend, job, filename,
                                      attribute_matcher, flags);
  else
    trash_backend_enumerate_root (backend, job, attribute_matcher, flags);
}

static gpointer
thread_func (gpointer user_data)
{
  GVfsBackendTrash *backend = G_VFS_BACKEND_TRASH (user_data);

  g_main_context_push_thread_default (backend->worker_context);
  backend->worker_loop = g_main_loop_new (backend->worker_context, FALSE);

  backend->root = trash_root_new (trash_backend_item_created,
                                  trash_backend_item_deleted,
                                  trash_backend_item_count_changed,
                                  backend);
  backend->watcher = trash_watcher_new (backend->root);

  g_main_loop_run (backend->worker_loop);

  trash_watcher_free (backend->watcher);
  trash_root_free (backend->root);

  g_main_context_pop_thread_default (backend->worker_context);
  g_main_loop_unref (backend->worker_loop);

  return NULL;
}

static void
trash_backend_mount (GVfsBackend  *vfs_backend,
                     GVfsJobMount *job,
                     GMountSpec   *mount_spec,
                     GMountSource *mount_source,
                     gboolean      is_automount)
{
  GVfsBackendTrash *backend = G_VFS_BACKEND_TRASH (vfs_backend);

  backend->file_monitor = NULL;
  backend->dir_monitor = NULL;

  backend->worker_context = g_main_context_new ();
  backend->worker_thread = g_thread_new ("Trash Worker Thread",
                                         thread_func,
                                         backend);
  trash_backend_worker_thread_queue_and_wait (backend, ready_func);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
trash_backend_query_info (GVfsBackend           *vfs_backend,
                          GVfsJobQueryInfo      *job,
                          const char            *filename,
                          GFileQueryInfoFlags    flags,
                          GFileInfo             *info,
                          GFileAttributeMatcher *matcher)
{
  GVfsBackendTrash *backend = G_VFS_BACKEND_TRASH (vfs_backend);

  g_assert (filename[0] == '/');

  if (!is_root (filename))
    {
      GError *error = NULL;
      gboolean is_toplevel;
      TrashItem *item;
      GFile *real;

      real = trash_backend_get_file (backend, filename,
                                     &item, &is_toplevel, &error);

      if (real)
        {
          GFileInfo *real_info;

          real_info = g_file_query_info (real, 
                                         job->attributes,
                                         flags,
                                         G_VFS_JOB (job)->cancellable,
                                         &error);
          g_object_unref (real);

          if (real_info)
            {
              g_file_info_copy_into (real_info, info);
              trash_backend_add_info (item, info, is_toplevel);
              g_vfs_job_succeeded (G_VFS_JOB (job));
              trash_item_unref (item);
              g_object_unref (real_info);

              return;
            }

          trash_item_unref (item);
        }

      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
  else
    {
      GIcon *icon;
      int n_items;

      trash_backend_worker_thread_queue_and_wait (backend, rescan_func);

      n_items = trash_root_get_n_items (backend->root);

      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_name (info, "/");
      /* Translators: this is the display name of the backend */
      g_file_info_set_display_name (info, _("Trash"));
      g_file_info_set_content_type (info, "inode/directory");

      icon = g_themed_icon_new (n_items ? "user-trash-full" : "user-trash");
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);

      icon = g_themed_icon_new (n_items ? "user-trash-full-symbolic" : "user-trash-symbolic");
      g_file_info_set_symbolic_icon (info, icon);
      g_object_unref (icon);

      g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_TRASH_ITEM_COUNT, n_items);

      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
}

static gboolean
trash_backend_query_fs_info (GVfsBackend           *vfs_backend,
                             GVfsJobQueryFsInfo    *job,
                             const char            *filename,
                             GFileInfo             *info,
                             GFileAttributeMatcher *matcher)
{
  g_file_info_set_attribute_string (info,
                                    G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
                                    "trash");
  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE,
                                     FALSE);

  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_FILESYSTEM_READONLY,
                                     FALSE);

  g_file_info_set_attribute_uint32 (info,
                                    G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW,
                                    G_FILESYSTEM_PREVIEW_TYPE_IF_LOCAL);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static gboolean
trash_backend_create_dir_monitor (GVfsBackend          *vfs_backend,
                                  GVfsJobCreateMonitor *job,
                                  const char           *filename,
                                  GFileMonitorFlags     flags)
{
  GVfsBackendTrash *backend = G_VFS_BACKEND_TRASH (vfs_backend);
  GVfsMonitor *monitor;

  if (!is_root (filename))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Operation not supported"));
      return TRUE;
    }

  monitor = trash_backend_get_dir_monitor (backend, TRUE);
  g_vfs_job_create_monitor_set_monitor (job, monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_object_unref (monitor);

  return TRUE;
}

static gboolean
trash_backend_create_file_monitor (GVfsBackend          *vfs_backend,
                                   GVfsJobCreateMonitor *job,
                                   const char           *filename,
                                   GFileMonitorFlags     flags)
{
  GVfsBackendTrash *backend = G_VFS_BACKEND_TRASH (vfs_backend);
  GVfsMonitor *monitor;

  if (!is_root (filename))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Operation not supported"));
      return TRUE;
    }

  monitor = trash_backend_get_file_monitor (backend, TRUE);
  g_vfs_job_create_monitor_set_monitor (job, monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_object_unref (monitor);

  return TRUE;
}

static void
trash_backend_finalize (GObject *object)
{
  GVfsBackendTrash *backend = G_VFS_BACKEND_TRASH (object);

  /* get rid of these first to stop a flood of event notifications
   * from being emitted while we're tearing down the TrashWatcher
   */
  if (backend->file_monitor)
    g_object_unref (backend->file_monitor);
  backend->file_monitor = NULL;

  if (backend->dir_monitor)
    g_object_unref (backend->dir_monitor);
  backend->dir_monitor = NULL;
}

static void
g_vfs_backend_trash_init (GVfsBackendTrash *backend)
{
  GVfsBackend *vfs_backend = G_VFS_BACKEND (backend);
  GMountSpec *mount_spec;

  /* translators: This is the name of the backend */
  g_vfs_backend_set_display_name (vfs_backend, _("Trash"));
  g_vfs_backend_set_icon_name (vfs_backend, "user-trash");
  g_vfs_backend_set_symbolic_icon_name (vfs_backend, "user-trash-symbolic");
  g_vfs_backend_set_user_visible (vfs_backend, FALSE);

  mount_spec = g_mount_spec_new ("trash");
  g_vfs_backend_set_mount_spec (vfs_backend, mount_spec);
  g_mount_spec_unref (mount_spec);
}

static void
g_vfs_backend_trash_class_init (GVfsBackendTrashClass *class)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (class);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (class);

  gobject_class->finalize = trash_backend_finalize;

  backend_class->mount = trash_backend_mount;
  backend_class->open_for_read = trash_backend_open_for_read;
  backend_class->read = trash_backend_read;
  backend_class->seek_on_read = trash_backend_seek_on_read;
  backend_class->query_info_on_read = trash_backend_query_info_on_read;
  backend_class->close_read = trash_backend_close_read;
  backend_class->query_info = trash_backend_query_info;
  backend_class->try_query_fs_info = trash_backend_query_fs_info;
  backend_class->enumerate = trash_backend_enumerate;
  backend_class->delete = trash_backend_delete;
  backend_class->pull = trash_backend_pull;
  backend_class->try_create_dir_monitor = trash_backend_create_dir_monitor;
  backend_class->try_create_file_monitor = trash_backend_create_file_monitor;
}
