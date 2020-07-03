/*
 * Copyright © 2012 Red Hat, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * See the included COPYING file for more information.
 */

#include "gvfsbackendrecent.h"

#include <glib/gi18n.h> /* _() */
#include <string.h>

#include "gvfsjobcreatemonitor.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobseekread.h"
#include "gvfsjobqueryinforead.h"
#include "gvfsjobread.h"

typedef GVfsBackendClass GVfsBackendRecentClass;

typedef struct {
  char *guid;
  char *uri;
  char *display_name;
  GFile *file;
  GDateTime *modified;
} RecentItem;

struct OPAQUE_TYPE__GVfsBackendRecent
{
  GVfsBackend parent_instance;

  GBookmarkFile *bookmarks;
  gchar *filename;
  GFileMonitor *monitor;
  GHashTable *uri_map;
  GHashTable *items;

  GVfsMonitor *file_monitor;
  GVfsMonitor *dir_monitor;
};

G_DEFINE_TYPE (GVfsBackendRecent, g_vfs_backend_recent, G_VFS_TYPE_BACKEND);

#define RECENTLY_USED_FILE "recently-used.xbel"

static GVfsMonitor *
recent_backend_get_file_monitor (GVfsBackendRecent *backend,
                                 gboolean           create)
{
  if (backend->file_monitor == NULL && create == FALSE)
    return NULL;

  else if (backend->file_monitor == NULL)
    {
      /* 'create' is only ever set in the main thread, so we will have
       * no possibility here for creating more than one new monitor.
       */
      /* FIXME */

      backend->file_monitor = g_vfs_monitor_new (G_VFS_BACKEND (backend));
    }

  return g_object_ref (backend->file_monitor);
}

static GVfsMonitor *
recent_backend_get_dir_monitor (GVfsBackendRecent *backend,
                                gboolean           create)
{
  if (backend->dir_monitor == NULL && create == FALSE)
    return NULL;

  else if (backend->dir_monitor == NULL)
    {
      backend->dir_monitor = g_vfs_monitor_new (G_VFS_BACKEND (backend));
    }

  return g_object_ref (backend->dir_monitor);
}

static GFile *
recent_backend_get_file (GVfsBackendRecent *backend,
                         const char        *filename,
                         RecentItem       **item_ret,
                         GError           **error)
{
  GFile *file = NULL;
  RecentItem *item;

  filename++;

  item = g_hash_table_lookup (backend->items, filename);
  if (item)
    {
      file = g_object_ref (item->file);
      if (item_ret)
        *item_ret = item;
    }

  if (file == NULL)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                         _("No such file or directory"));

  return file;
}

/* ======================= method implementations ======================= */
static gboolean
recent_backend_open_for_read (GVfsBackend        *vfs_backend,
                              GVfsJobOpenForRead *job,
                              const char         *filename)
{
  GVfsBackendRecent *backend = G_VFS_BACKEND_RECENT (vfs_backend);
  GError *error = NULL;

  if (filename[1] == '\0')
    g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
                         _("Can’t open directory"));

  else
    {
      GFile *real;

      real = recent_backend_get_file (backend, filename, NULL, &error);

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

              return TRUE;
            }
        }
    }

  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);

  return TRUE;
}

static gboolean
recent_backend_read (GVfsBackend       *vfs_backend,
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

      return TRUE;
    }

  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);

  return TRUE;
}

static gboolean
recent_backend_seek_on_read (GVfsBackend       *vfs_backend,
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

      return TRUE;
    }

  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);

  return TRUE;
}

static void
recent_backend_query_info_on_read (GVfsBackend           *backend,
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

static gboolean
recent_backend_close_read (GVfsBackend       *vfs_backend,
                           GVfsJobCloseRead  *job,
                           GVfsBackendHandle  handle)
{
  GError *error = NULL;

  if (g_input_stream_close (handle, G_VFS_JOB (job)->cancellable, &error))
    {
      g_vfs_job_succeeded (G_VFS_JOB (job));
      g_object_unref (handle);

      return TRUE;
    }

  g_object_unref (handle);

  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);

  return TRUE;
}

static gboolean
recent_backend_delete (GVfsBackend   *vfs_backend,
                       GVfsJobDelete *job,
                       const char    *filename)
{
  GVfsBackendRecent *backend = G_VFS_BACKEND_RECENT (vfs_backend);
  GError *error = NULL;
  g_debug ("before job: %d\n", G_OBJECT(job)->ref_count);

  if (filename[1] == '\0')
    {
      g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           _("The recent folder may not be deleted"));
    }
  else
    {
      RecentItem *item;

      item = g_hash_table_lookup (backend->items, filename + 1);
      if (item)
        {
          gboolean res;

          res = g_bookmark_file_remove_item (backend->bookmarks, item->uri, &error);
          if (res)
            res = g_bookmark_file_to_file (backend->bookmarks, backend->filename, &error);

          if (res)
            {
              g_vfs_job_succeeded (G_VFS_JOB (job));
              return TRUE;
            }
        }
      else
        g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             _("No such file or directory"));
    }

  g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
  g_error_free (error);

  return TRUE;
}

static void
recent_backend_add_info (RecentItem *item,
                         GFileInfo  *info)
{
  g_assert (item != NULL);

  g_file_info_set_name (info, item->guid);
  g_file_info_set_display_name (info, item->display_name);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, item->uri);

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
                                     TRUE);

  /* G_FILE_ATTRIBUTE_RECENT_MODIFIED */
  g_file_info_set_attribute_int64 (info, "recent::modified",
                                   g_date_time_to_unix (item->modified));
}

static gboolean
recent_backend_enumerate (GVfsBackend           *vfs_backend,
                          GVfsJobEnumerate      *job,
                          const char            *filename,
                          GFileAttributeMatcher *attribute_matcher,
                          GFileQueryInfoFlags    flags)
{
  GVfsBackendRecent *backend = G_VFS_BACKEND_RECENT (vfs_backend);
  GHashTableIter iter;
  gpointer key, value;

  g_assert (filename[0] == '/');

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_iter_init (&iter, backend->items);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      RecentItem *item = value;
      GFileInfo *info;

      info = g_file_query_info (item->file,
                                job->attributes,
                                flags,
                                G_VFS_JOB (job)->cancellable,
                                NULL);
      if (info)
        {
          g_file_info_set_attribute_mask (info, attribute_matcher);
          recent_backend_add_info (item, info);
          g_vfs_job_enumerate_add_info (job, info);
          g_object_unref (info);
        }
    }
  g_vfs_job_enumerate_done (job);

  return TRUE;
}

static void
recent_item_free (RecentItem *item)
{
  g_free (item->uri);
  g_free (item->display_name);
  g_free (item->guid);
  g_clear_object (&item->file);
  g_date_time_unref (item->modified);
  g_free (item);
}

static gboolean
recent_item_update (RecentItem  *item,
                    const gchar *uri,
                    const gchar *display_name,
                    GDateTime   *modified)
{
  gboolean changed = FALSE;

  if (g_strcmp0 (item->uri, uri) != 0)
    {
      changed = TRUE;
      g_free (item->uri);
      item->uri = g_strdup (uri);

      g_clear_object (&item->file);
      item->file = g_file_new_for_uri (item->uri);
    }

  if (g_strcmp0 (item->display_name, display_name) != 0)
    {
      changed = TRUE;
      g_free (item->display_name);
      item->display_name = g_strdup (display_name);
    }

  if (!g_date_time_equal (item->modified, modified))
    {
      changed = TRUE;
      g_date_time_unref (item->modified);
      item->modified = g_date_time_ref (modified);
    }

  return changed;
}

static RecentItem *
recent_item_new (const gchar *uri,
                 const gchar *display_name,
                 GDateTime   *modified)
{
  RecentItem *item;
  item = g_new0 (RecentItem, 1);
  item->guid = g_dbus_generate_guid ();
  item->modified = g_date_time_ref (modified);

  recent_item_update (item, uri, display_name, modified);

  return item;
}

static gboolean
should_include (GBookmarkFile *bookmarks,
                const gchar   *uri)
{
  gchar *mimetype, *filename;

  /* Is public */
  if (g_bookmark_file_get_is_private (bookmarks, uri, NULL))
    return FALSE;

  /* Is local */
  if (g_ascii_strncasecmp (uri, "file:/", 6) != 0)
    return FALSE;

  /* Is not dir */
  mimetype = g_bookmark_file_get_mime_type (bookmarks, uri, NULL);
  if (g_strcmp0 (mimetype, "inode/directory") == 0)
    {
      g_free (mimetype);
      return FALSE;
    }
  g_free (mimetype);

  /* Exists */
  filename = g_filename_from_uri (uri, NULL, NULL);
  if (!g_file_test (filename, G_FILE_TEST_EXISTS))
    {
      g_free (filename);
      return FALSE;
    }
  g_free (filename);

  return TRUE;
}

static char *
get_display_name (GBookmarkFile *bookmarks,
                  const gchar   *uri)
{
  gchar *filename, *display_name;

  display_name = g_bookmark_file_get_title (bookmarks, uri, NULL);
  if (display_name == NULL)
    {
      filename = g_filename_from_uri (uri, NULL, NULL);
      display_name = g_filename_display_basename (filename);
      g_free (filename);
    }

  return display_name;
}

static void
reload_recent_items (GVfsBackendRecent *backend)
{
  GVfsMonitor *monitor;
  GList *added = NULL;
  GList *changed = NULL;
  GList *not_seen_items = NULL;
  GList *l;
  GError *error = NULL;
  gchar **uris;
  gsize uris_len, i;

  g_debug ("reloading recent items\n");

  g_bookmark_file_load_from_file (backend->bookmarks, backend->filename, &error);
  if (error != NULL)
    {
      g_warning ("Unable to load %s: %s", backend->filename, error->message);
      g_clear_error (&error);
      g_bookmark_file_free (backend->bookmarks);
      backend->bookmarks = g_bookmark_file_new ();
    }

  not_seen_items = g_hash_table_get_values (backend->items);
  uris = g_bookmark_file_get_uris (backend->bookmarks, &uris_len);
  for (i = 0; i < uris_len; i++)
    {
      const char *uri = uris[i];
      const char *guid;
      char *display_name;
      GDateTime *modified;

      if (should_include (backend->bookmarks, uri))
        {
          display_name = get_display_name (backend->bookmarks, uri);
          modified = g_bookmark_file_get_modified_date_time (backend->bookmarks, uri, NULL);
          guid = g_hash_table_lookup (backend->uri_map, uri);
          if (guid)
            {
              RecentItem *item;
              item = g_hash_table_lookup (backend->items, guid);
              if (recent_item_update (item, uri, display_name, modified))
                changed = g_list_prepend (changed, item->guid);
              not_seen_items = g_list_remove (not_seen_items, item);
            }
          else
            {
              RecentItem *item;
              item = recent_item_new (uri, display_name, modified);
              added = g_list_prepend (added, item->guid);
              g_hash_table_insert (backend->items, item->guid, item);
              g_hash_table_insert (backend->uri_map, item->uri, item->guid);
            }

          g_free (display_name);
        }
    }

  g_strfreev (uris);

  monitor = recent_backend_get_dir_monitor (backend, FALSE);

  /* process removals */
  for (l = not_seen_items; l; l = l->next)
    {
      RecentItem *item = l->data;
      g_hash_table_remove (backend->uri_map, item->uri);
      g_hash_table_steal (backend->items, item->guid);
      if (monitor)
        g_vfs_monitor_emit_event (monitor, G_FILE_MONITOR_EVENT_DELETED, item->guid, NULL);
      recent_item_free (item);
    }
  g_list_free (not_seen_items);

  /* process additions */
  if (monitor)
    {
      for (l = added; l; l = l->next)
        g_vfs_monitor_emit_event (monitor, G_FILE_MONITOR_EVENT_CREATED, l->data, NULL);
    }
  g_list_free (added);

  /* process changes */
  if (monitor)
    {
      for (l = changed; l; l = l->next)
        g_vfs_monitor_emit_event (monitor, G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED, l->data, NULL);
    }
  g_list_free (changed);

  if (monitor)
    g_object_unref (monitor);
}

static void
bookmarks_changed (GFileMonitor      *monitor,
                   GFile             *file,
                   GFile             *other_file,
                   GFileMonitorEvent  event_type,
                   gpointer           user_data)
{
  GVfsBackendRecent *backend = G_VFS_BACKEND_RECENT (user_data);
  gchar *filename;

  switch (event_type)
    {
      case G_FILE_MONITOR_EVENT_CREATED:
      case G_FILE_MONITOR_EVENT_DELETED:
        filename = g_file_get_path (file);
        if (g_strcmp0 (filename, backend->filename) != 0)
          {
            g_free (filename);
            break;
          }
        g_free (filename);
      case G_FILE_MONITOR_EVENT_CHANGED:
        reload_recent_items (backend);
        break;

      default:
        break;
    }
}

static gboolean
recent_backend_mount (GVfsBackend  *vfs_backend,
                      GVfsJobMount *job,
                      GMountSpec   *mount_spec,
                      GMountSource *mount_source,
                      gboolean      is_automount)
{
  GVfsBackendRecent *backend = G_VFS_BACKEND_RECENT (vfs_backend);
  GError *error = NULL;
  GFile *file;

  backend->bookmarks = g_bookmark_file_new ();
  backend->filename = g_build_filename (g_get_user_data_dir (), RECENTLY_USED_FILE, NULL);

  file = g_file_new_for_path (backend->filename);
  backend->monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, &error);
  g_object_unref (file);
  if (error != NULL)
    {
      g_warning ("Unable to monitor %s: %s", backend->filename, error->message);
      g_clear_error (&error);
    }
  else
    g_signal_connect (backend->monitor, "changed", G_CALLBACK (bookmarks_changed), backend);

  reload_recent_items (backend);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static gboolean
recent_backend_query_info (GVfsBackend           *vfs_backend,
                           GVfsJobQueryInfo      *job,
                           const char            *filename,
                           GFileQueryInfoFlags    flags,
                           GFileInfo             *info,
                           GFileAttributeMatcher *matcher)
{
  GVfsBackendRecent *backend = G_VFS_BACKEND_RECENT (vfs_backend);

  g_assert (filename[0] == '/');

  if (filename[1])
    {
      GError *error = NULL;
      RecentItem *item = NULL;
      GFile *real;

      real = recent_backend_get_file (backend, filename, &item, &error);

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
              recent_backend_add_info (item, info);
              g_vfs_job_succeeded (G_VFS_JOB (job));
              g_object_unref (real_info);

              return TRUE;
            }
        }

      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
    }
  else
    {
      GIcon *icon;

      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_name (info, "/");
      /* Translators: this is the display name of the backend */
      g_file_info_set_display_name (info, _("Recent"));
      g_file_info_set_content_type (info, "inode/directory");
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, TRUE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);

      icon = g_themed_icon_new ("document-open-recent");
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
      icon = g_themed_icon_new ("document-open-recent-symbolic");
      g_file_info_set_symbolic_icon (info, icon);
      g_object_unref (icon);

      g_vfs_job_succeeded (G_VFS_JOB (job));
    }

  return TRUE;
}

static gboolean
recent_backend_query_fs_info (GVfsBackend           *vfs_backend,
                              GVfsJobQueryFsInfo    *job,
                              const char            *filename,
                              GFileInfo             *info,
                              GFileAttributeMatcher *matcher)
{
  g_file_info_set_attribute_string (info,
                                    G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
                                    "recent");
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
recent_backend_create_dir_monitor (GVfsBackend          *vfs_backend,
                                   GVfsJobCreateMonitor *job,
                                   const char           *filename,
                                   GFileMonitorFlags     flags)
{
  GVfsBackendRecent *backend = G_VFS_BACKEND_RECENT (vfs_backend);
  GVfsMonitor *monitor;

  if (filename[1])
    monitor = g_vfs_monitor_new (vfs_backend);
  else
    monitor = recent_backend_get_dir_monitor (backend, TRUE);

  g_vfs_job_create_monitor_set_monitor (job, monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_object_unref (monitor);

  return TRUE;
}

static gboolean
recent_backend_create_file_monitor (GVfsBackend          *vfs_backend,
                                    GVfsJobCreateMonitor *job,
                                    const char           *filename,
                                    GFileMonitorFlags     flags)
{
  GVfsBackendRecent *backend = G_VFS_BACKEND_RECENT (vfs_backend);
  GVfsMonitor *monitor;

  if (filename[1])
    monitor = g_vfs_monitor_new (vfs_backend);
  else
    monitor = recent_backend_get_file_monitor (backend, TRUE);

  g_vfs_job_create_monitor_set_monitor (job, monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_object_unref (monitor);

  return TRUE;
}

static void
recent_backend_finalize (GObject *object)
{
  GVfsBackendRecent *backend = G_VFS_BACKEND_RECENT (object);

  g_clear_object (&backend->dir_monitor);
  g_clear_object (&backend->file_monitor);

  g_hash_table_destroy (backend->items);
  g_hash_table_destroy (backend->uri_map);

  g_clear_pointer (&backend->filename, g_free);
  g_clear_pointer (&backend->bookmarks, g_bookmark_file_free);
  if (backend->monitor)
    {
      g_signal_handlers_disconnect_by_func (backend->monitor, reload_recent_items, backend);
      g_clear_object (&backend->monitor);
    }

  if (G_OBJECT_CLASS (g_vfs_backend_recent_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_recent_parent_class)->finalize) (object);
}

static void
g_vfs_backend_recent_init (GVfsBackendRecent *backend)
{
  GVfsBackend *vfs_backend = G_VFS_BACKEND (backend);
  GMountSpec *mount_spec;

  backend->items = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify)recent_item_free);
  backend->uri_map = g_hash_table_new (g_str_hash, g_str_equal);

  /* translators: This is the name of the backend */
  g_vfs_backend_set_display_name (vfs_backend, _("Recent"));
  g_vfs_backend_set_icon_name (vfs_backend, "document-open-recent");
  g_vfs_backend_set_symbolic_icon_name (vfs_backend, "document-open-recent-symbolic");
  g_vfs_backend_set_user_visible (vfs_backend, FALSE);

  mount_spec = g_mount_spec_new ("recent");
  g_vfs_backend_set_mount_spec (vfs_backend, mount_spec);
  g_mount_spec_unref (mount_spec);
}

static void
g_vfs_backend_recent_class_init (GVfsBackendRecentClass *class)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (class);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (class);

  gobject_class->finalize = recent_backend_finalize;

  backend_class->try_mount = recent_backend_mount;
  backend_class->try_open_for_read = recent_backend_open_for_read;
  backend_class->try_read = recent_backend_read;
  backend_class->try_seek_on_read = recent_backend_seek_on_read;
  backend_class->query_info_on_read = recent_backend_query_info_on_read;
  backend_class->try_close_read = recent_backend_close_read;
  backend_class->try_query_info = recent_backend_query_info;
  backend_class->try_query_fs_info = recent_backend_query_fs_info;
  backend_class->try_enumerate = recent_backend_enumerate;
  backend_class->try_delete = recent_backend_delete;
  backend_class->try_create_dir_monitor = recent_backend_create_dir_monitor;
  backend_class->try_create_file_monitor = recent_backend_create_file_monitor;
}
