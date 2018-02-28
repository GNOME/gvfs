/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2009 Thibault Saunier
 * Copyright (C) 2014, 2015 Red Hat, Inc.
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
 * Author: Debarshi Ray <debarshir@gnome.org>
 *         Thibault Saunier <saunierthibault@gmail.com>
 */

#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#define GOA_API_IS_SUBJECT_TO_CHANGE
#include <gdata/gdata.h>
#include <goa/goa.h>

#include "gvfsbackendgoogle.h"
#include "gvfsicon.h"
#include "gvfsjobcloseread.h"
#include "gvfsjobcopy.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobwrite.h"
#include "gvfsmonitor.h"

struct _GVfsBackendGoogle
{
  GVfsBackend parent;
  GDataDocumentsService *service;
  GDataEntry *root;
  GHashTable *entries;
  GHashTable *dir_entries;
  GHashTable *monitors;
  GList *dir_collisions;
  GRecMutex mutex; /* guards cache */
  GoaClient *client;
  gboolean entries_stale;
  gchar *account_identity;
  guint entries_stale_timeout;
};

struct _GVfsBackendGoogleClass
{
  GVfsBackendClass parent_class;
};

G_DEFINE_TYPE(GVfsBackendGoogle, g_vfs_backend_google, G_VFS_TYPE_BACKEND)

/* ---------------------------------------------------------------------------------------------------- */

#define CATEGORY_SCHEMA_KIND "http://schemas.google.com/g/2005#kind"
#define CATEGORY_SCHEMA_KIND_FILE "http://schemas.google.com/docs/2007#file"

#define CONTENT_TYPE_PREFIX_GOOGLE "application/vnd.google-apps"

#define MAX_RESULTS 50

#define REBUILD_ENTRIES_TIMEOUT 60 /* s */

#define URI_PREFIX "https://www.googleapis.com/drive/v2/files/"

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  gchar *title_or_id;
  gchar *parent_id;
} DirEntriesKey;

typedef struct
{
  GDataEntry *document;
  GDataUploadStream *stream;
  gchar *filename;
} WriteHandle;

static GDataEntry *resolve_dir (GVfsBackendGoogle  *self,
                                const gchar        *filename,
                                gchar             **out_basename,
                                GError            **error);

/* ---------------------------------------------------------------------------------------------------- */

static DirEntriesKey *
dir_entries_key_new (const gchar *title_or_id, const gchar *parent_id)
{
  DirEntriesKey *k;

  k = g_slice_new0 (DirEntriesKey);
  k->title_or_id = g_strdup (title_or_id);
  k->parent_id = g_strdup (parent_id);
  return k;
}

static void
dir_entries_key_free (gpointer data)
{
  DirEntriesKey *k = (DirEntriesKey *) data;

  if (k == NULL)
    return;

  g_free (k->title_or_id);
  g_free (k->parent_id);
  g_slice_free (DirEntriesKey, k);
}

static guint
entries_in_folder_hash (gconstpointer key)
{
  DirEntriesKey *k = (DirEntriesKey *) key;
  guint hash1;
  guint hash2;

  hash1 = g_str_hash (k->title_or_id);
  hash2 = g_str_hash (k->parent_id);
  return hash1 ^ hash2;
}

static gboolean
entries_in_folder_equal (gconstpointer a, gconstpointer b)
{
  DirEntriesKey *k_a = (DirEntriesKey *) a;
  DirEntriesKey *k_b = (DirEntriesKey *) b;

  if (g_strcmp0 (k_a->title_or_id, k_b->title_or_id) == 0 &&
      g_strcmp0 (k_a->parent_id, k_b->parent_id) == 0)
    return TRUE;

  return FALSE;
}

/* ---------------------------------------------------------------------------------------------------- */

static WriteHandle *
write_handle_new (GDataEntry *document, GDataUploadStream *stream, const gchar *filename)
{
  WriteHandle *handle;

  handle = g_slice_new0 (WriteHandle);

  if (document != NULL)
    handle->document = g_object_ref (document);

  if (stream != NULL)
    {
      handle->stream = g_object_ref (stream);
      if (handle->document == NULL)
        handle->document = g_object_ref (gdata_upload_stream_get_entry (stream));
    }

  handle->filename = g_strdup (filename);

  return handle;
}

static void
write_handle_free (gpointer data)
{
  WriteHandle *handle = (WriteHandle *) data;

  if (handle == NULL)
    return;

  g_clear_object (&handle->document);
  g_clear_object (&handle->stream);
  g_free (handle->filename);
  g_slice_free (WriteHandle, handle);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
sanitize_error (GError **error)
{
  if (g_error_matches (*error, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_AUTHENTICATION_REQUIRED) ||
      g_error_matches (*error, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_FORBIDDEN))
    {
      g_warning ("%s", (*error)->message);
      g_clear_error (error);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, _("Permission denied"));
    }
  else if (g_error_matches (*error, GDATA_SERVICE_ERROR, GDATA_SERVICE_ERROR_NOT_FOUND))
    {
      g_warning ("%s", (*error)->message);
      g_clear_error (error);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("Target object doesn’t exist"));
    }
}

/* ---------------------------------------------------------------------------------------------------- */

static void
emit_event_internal (GVfsMonitor       *monitor,
                     const gchar       *entry_path,
                     GFileMonitorEvent  event)
{
  const gchar *monitored_path;
  gchar *parent_path;

  if (entry_path == NULL)
    return;

  monitored_path = g_object_get_data (G_OBJECT (monitor), "g-vfs-backend-google-path");
  parent_path = g_path_get_dirname (entry_path);

  if (g_strcmp0 (parent_path, monitored_path) == 0)
    {
      g_debug ("  emit event %d on parent directory for %s\n", event, entry_path);
      g_vfs_monitor_emit_event (monitor, event, entry_path, NULL);
    }
  else if (g_strcmp0 (entry_path, monitored_path) == 0)
    {
      g_debug ("  emit event %d on file %s\n", event, entry_path);
      g_vfs_monitor_emit_event (monitor, event, entry_path, NULL);
    }

  g_free (parent_path);
}

static void
emit_attribute_changed_event (gpointer monitor,
                              gpointer unused,
                              gpointer entry_path)
{
  emit_event_internal (G_VFS_MONITOR (monitor), entry_path, G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED);
}

static void
emit_changed_event (gpointer monitor,
                    gpointer unused,
                    gpointer entry_path)
{
  emit_event_internal (G_VFS_MONITOR (monitor), entry_path, G_FILE_MONITOR_EVENT_CHANGED);
}

static void
emit_changes_done_event (gpointer monitor,
                         gpointer unused,
                         gpointer entry_path)
{
  emit_event_internal (G_VFS_MONITOR (monitor), entry_path, G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT);
}

static void
emit_create_event (gpointer monitor,
                   gpointer unused,
                   gpointer entry_path)
{
  emit_event_internal (G_VFS_MONITOR (monitor), entry_path, G_FILE_MONITOR_EVENT_CREATED);
}

static void
emit_delete_event (gpointer monitor,
                   gpointer unused,
                   gpointer entry_path)
{
  emit_event_internal (G_VFS_MONITOR (monitor), entry_path, G_FILE_MONITOR_EVENT_DELETED);
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
get_content_type_from_entry (GDataEntry *entry)
{
  GList *categories;
  GList *l;
  gchar *ret_val = NULL;

  categories = gdata_entry_get_categories (entry);
  for (l = categories; l != NULL; l = l->next)
    {
      GDataCategory *category = GDATA_CATEGORY (l->data);
      const gchar *scheme;

      scheme = gdata_category_get_scheme (category);
      if (g_strcmp0 (scheme, CATEGORY_SCHEMA_KIND) == 0)
        {
          ret_val = g_strdup (gdata_category_get_label (category));
          break;
        }
    }

  return ret_val;
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
get_parent_id (GVfsBackendGoogle *self,
               GDataEntry        *entry)
{
  GList *l;
  GList *links = NULL;
  gchar *ret_val = NULL;

  links = gdata_entry_look_up_links (entry, GDATA_LINK_PARENT);
  for (l = links; l != NULL; l = l->next)
    {
      GDataLink *link = GDATA_LINK (l->data);
      const gchar *uri;

      /* HACK: GDataLink does not have the ID, only the URI. Extract
       * the ID from the GDataLink:uri by removing the prefix. Ignore
       * links which don't have the prefix.
       */
      uri = gdata_link_get_uri (link);
      if (g_str_has_prefix (uri, URI_PREFIX))
        {
          const gchar *id;
          gsize uri_prefix_len;

          uri_prefix_len = strlen (URI_PREFIX);
          id = uri + uri_prefix_len;
          if (id[0] != '\0')
            {
              ret_val = g_strdup (uri + uri_prefix_len);
              break;
            }
        }
    }

  if (ret_val == NULL)
    {
      const gchar *root_id;

      root_id = gdata_entry_get_id (self->root);
      ret_val = g_strdup (root_id);
    }

  g_list_free (links);
  return ret_val;
}

static gchar *
get_entry_path (GVfsBackendGoogle *self, GDataEntry *entry)
{
  GString *path = NULL;
  const gchar *base_id;
  const gchar *root_id;
  gchar *id = NULL;
  gchar *ret_val = NULL;

  if (entry == self->root)
    {
      ret_val = g_strdup ("/");
      goto out;
    }

  base_id = gdata_entry_get_id (entry);
  path = g_string_new (base_id);
  g_string_prepend_c (path, '/');

  id = get_parent_id (self, entry);
  root_id = gdata_entry_get_id (self->root);

  while (id != NULL)
    {
      GDataEntry *parent_entry;

      /* The root folder itself has an ID, so path can become
       * /root/folder1/folder2/file. Instead, we want it to be
       * /folder1/folder2/file.
       */

      if (g_strcmp0 (id, root_id) == 0)
        break;

      parent_entry = g_hash_table_lookup (self->entries, id);
      if (parent_entry == NULL)
        goto out;

      g_string_prepend (path, id);
      g_string_prepend_c (path, '/');

      g_free (id);
      id = get_parent_id (self, parent_entry);
    }

  ret_val = g_strdup (path->str);

 out:
  g_free (id);
  if (path != NULL)
    g_string_free (path, TRUE);
  return ret_val;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
insert_entry_full (GVfsBackendGoogle *self,
                   GDataEntry        *entry,
                   gboolean           track_dir_collisions)
{
  DirEntriesKey *k;
  GDataEntry *old_entry;
  gboolean insert_title = TRUE;
  const gchar *id;
  const gchar *old_id;
  const gchar *title;
  gchar *parent_id;

  id = gdata_entry_get_id (entry);
  title = gdata_entry_get_title (entry);

  g_hash_table_insert (self->entries, g_strdup (id), g_object_ref (entry));

  parent_id = get_parent_id (self, entry);

  k = dir_entries_key_new (id, parent_id);
  g_hash_table_insert (self->dir_entries, k, g_object_ref (entry));
  g_debug ("  insert_entry: Inserted (%s, %s) -> %p\n", id, parent_id, entry);

  k = dir_entries_key_new (title, parent_id);
  old_entry = g_hash_table_lookup (self->dir_entries, k);
  if (old_entry != NULL)
    {
      old_id = gdata_entry_get_id (old_entry);
      if (g_strcmp0 (old_id, title) == 0)
        {
          insert_title = FALSE;
        }
      else
        {
          /* If the collision is not due to the title matching the ID
           * of an earlier GDataEntry, then it is due to duplicate
           * titles.
           */
          if (g_strcmp0 (old_id, id) < 0)
            insert_title = FALSE;
        }
    }

  if (insert_title)
    {
      if (old_entry != NULL && track_dir_collisions)
        {
          self->dir_collisions = g_list_prepend (self->dir_collisions, g_object_ref (old_entry));
          g_debug ("  insert_entry: Ejected (%s, %s, %s) -> %p\n", old_id, title, parent_id, old_entry);
        }

      g_hash_table_insert (self->dir_entries, k, g_object_ref (entry));
      g_debug ("  insert_entry: Inserted (%s, %s) -> %p\n", title, parent_id, entry);
    }
  else
    {
      if (track_dir_collisions)
        {
          self->dir_collisions = g_list_prepend (self->dir_collisions, g_object_ref (entry));
          g_debug ("  insert_entry: Skipped (%s, %s, %s) -> %p\n", id, title, parent_id, entry);
        }

      dir_entries_key_free (k);
    }

  g_free (parent_id);
  return insert_title;
}

static void
insert_entry (GVfsBackendGoogle *self,
              GDataEntry        *entry)
{
  insert_entry_full (self, entry, TRUE);
}

static void
remove_entry (GVfsBackendGoogle *self,
              GDataEntry        *entry)
{
  DirEntriesKey *k;
  GList *l;
  const gchar *id;
  const gchar *title;
  gchar *parent_id;

  id = gdata_entry_get_id (entry);
  title = gdata_entry_get_title (entry);

  g_hash_table_remove (self->entries, id);

  parent_id = get_parent_id (self, entry);

  k = dir_entries_key_new (id, parent_id);
  g_hash_table_remove (self->dir_entries, k);
  g_debug ("  remove_entry: Removed (%s, %s) -> %p\n", id, parent_id, entry);
  dir_entries_key_free (k);

  k = dir_entries_key_new (title, parent_id);
  g_hash_table_remove (self->dir_entries, k);
  g_debug ("  remove_entry: Removed (%s, %s) -> %p\n", title, parent_id, entry);
  dir_entries_key_free (k);

  for (l = self->dir_collisions; l != NULL; l = l->next)
    {
      GDataEntry *colliding_entry = GDATA_ENTRY (l->data);

      if (insert_entry_full (self, colliding_entry, FALSE))
        {
          self->dir_collisions = g_list_remove_link (self->dir_collisions, l);
          g_debug ("  remove_entry: Restored %p\n", colliding_entry);
          g_list_free (l);
          g_object_unref (colliding_entry);
          break;
        }
    }

  g_free (parent_id);
}

static void
rebuild_entries (GVfsBackendGoogle  *self,
                 GCancellable       *cancellable,
                 GError            **error)
{
  GDataDocumentsFeed *feed = NULL;
  GDataDocumentsQuery *query = NULL;
  gboolean succeeded_once = FALSE;

  query = gdata_documents_query_new_with_limits (NULL, 1, MAX_RESULTS);
  gdata_documents_query_set_show_folders (query, TRUE);

  while (TRUE)
    {
      GError *local_error;
      GList *entries;
      GList *l;

      local_error = NULL;
      feed = gdata_documents_service_query_documents (self->service, query, cancellable, NULL, NULL, &local_error);
      if (local_error != NULL)
        {
          sanitize_error (&local_error);
          g_propagate_error (error, local_error);
          self->entries_stale = TRUE;

          goto out;
        }

      if (!succeeded_once)
        {
          g_hash_table_remove_all (self->entries);
          g_hash_table_remove_all (self->dir_entries);

          g_list_free_full (self->dir_collisions, g_object_unref);
          self->dir_collisions = NULL;

          succeeded_once = TRUE;
        }

      entries = gdata_feed_get_entries (GDATA_FEED (feed));
      if (entries == NULL)
        break;

      for (l = entries; l != NULL; l = l->next)
        {
          GDataEntry *entry = GDATA_ENTRY (l->data);
          insert_entry (self, entry);
        }

      gdata_query_next_page (GDATA_QUERY (query));
      g_clear_object (&feed);
    }

  self->entries_stale = FALSE;

 out:
  g_clear_object (&feed);
  g_clear_object (&query);
}

/* ---------------------------------------------------------------------------------------------------- */

static GDataEntry *
resolve_child (GVfsBackendGoogle *self,
               GDataEntry        *parent,
               const gchar       *basename)
{
  DirEntriesKey *k;
  GDataEntry *entry;
  const gchar *parent_id;

  parent_id = gdata_entry_get_id (parent);
  k = dir_entries_key_new (basename, parent_id);
  entry = g_hash_table_lookup (self->dir_entries, k);
  dir_entries_key_free (k);
  return entry;
}

static GDataEntry *
resolve (GVfsBackendGoogle  *self,
         const gchar        *filename,
         GError            **error)
{
  GDataEntry *parent;
  GDataEntry *ret_val = NULL;
  GError *local_error;
  gchar *basename = NULL;

  if (g_strcmp0 (filename, "/") == 0)
    {
      ret_val = self->root;
      goto out;
    }

  local_error = NULL;
  parent = resolve_dir (self, filename, &basename, &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, local_error);
      goto out;
    }

  ret_val = resolve_child (self, parent, basename);
  if (ret_val == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("No such file or directory"));
      goto out;
    }

 out:
  g_free (basename);
  return ret_val;
}

static GDataEntry *
resolve_dir (GVfsBackendGoogle  *self,
             const gchar        *filename,
             gchar             **out_basename,
             GError            **error)
{
  GDataEntry *parent;
  GDataEntry *ret_val = NULL;
  GError *local_error;
  gchar *basename = NULL;
  gchar *parent_path = NULL;

  basename = g_path_get_basename (filename);
  parent_path = g_path_get_dirname (filename);

  local_error = NULL;
  parent = resolve (self, parent_path, &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, local_error);
      goto out;
    }

  if (!GDATA_IS_DOCUMENTS_FOLDER (parent))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, _("The file is not a directory"));
      goto out;
    }

  if (out_basename != NULL)
    {
      *out_basename = basename;
      basename = NULL;
    }

  ret_val = parent;

 out:
  g_free (basename);
  g_free (parent_path);
  return ret_val;
}

/* ---------------------------------------------------------------------------------------------------- */

static GDataEntry *
resolve_and_rebuild (GVfsBackendGoogle  *self,
                     const gchar        *filename,
                     GCancellable       *cancellable,
                     GError            **error)
{
  GDataEntry *entry;
  GDataEntry *ret_val = NULL;

  entry = resolve (self, filename, NULL);
  if (entry == NULL)
    {
      GError *local_error;

      local_error = NULL;
      rebuild_entries (self, cancellable, &local_error);
      if (local_error != NULL)
        {
          g_propagate_error (error, local_error);
          goto out;
        }

      local_error = NULL;
      entry = resolve (self, filename, &local_error);
      if (local_error != NULL)
        {
          g_propagate_error (error, local_error);
          goto out;
        }
    }

  ret_val = entry;

 out:
  return ret_val;
}

static GDataEntry *
resolve_dir_and_rebuild (GVfsBackendGoogle  *self,
                         const gchar        *filename,
                         GCancellable       *cancellable,
                         gchar             **out_basename,
                         GError            **error)
{
  GDataEntry *parent;
  GDataEntry *ret_val = NULL;
  GError *local_error;
  gchar *basename = NULL;

  local_error = NULL;
  parent = resolve_dir (self, filename, &basename, &local_error);
  if (local_error != NULL)
    {
      if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY))
        {
          g_propagate_error (error, local_error);
          goto out;
        }
      else
        {
          g_error_free (local_error);
        }

      local_error = NULL;
      rebuild_entries (self, cancellable, &local_error);
      if (local_error != NULL)
        {
          g_propagate_error (error, local_error);
          goto out;
        }

      local_error = NULL;
      parent = resolve_dir (self, filename, &basename, &local_error);
      if (local_error != NULL)
        {
          g_propagate_error (error, local_error);
          goto out;
        }
    }

  if (out_basename != NULL)
    {
      *out_basename = basename;
      basename = NULL;
    }

  ret_val = parent;

 out:
  g_free (basename);
  return ret_val;
}

/* ---------------------------------------------------------------------------------------------------- */

static char *
get_extension_offset (const char *title)
{
  gchar *end;
  gchar *end2;

  end = strrchr (title, '.');

  if (end != NULL && end != title)
    {
      if (g_strcmp0 (end, ".gz") == 0 ||
          g_strcmp0 (end, ".bz2") == 0 ||
          g_strcmp0 (end, ".sit") == 0 ||
          g_strcmp0 (end, ".zip") == 0 ||
          g_strcmp0 (end, ".Z") == 0)
        {
          end2 = end - 1;
          while (end2 > title && *end2 != '.')
            end2--;

          if (end2 != title)
            end = end2;
        }
    }

  return end;
}

static gchar *
generate_copy_name (GVfsBackendGoogle *self, GDataEntry *entry)
{
  GDataEntry *existing_entry;
  GDataEntry *parent;
  const gchar *id;
  const gchar *title;
  gchar *entry_path = NULL;
  gchar *extension = NULL;
  gchar *extension_offset;
  gchar *ret_val = NULL;
  gchar *title_without_extension = NULL;

  title = gdata_entry_get_title (entry);

  entry_path = get_entry_path (self, entry);
  if (entry_path == NULL)
    goto out;

  parent = resolve_dir (self, entry_path, NULL, NULL);
  if (parent == NULL)
    goto out;

  existing_entry = resolve_child (self, parent, title);
  if (existing_entry == entry)
    goto out;

  title_without_extension = g_strdup (title);
  extension_offset = get_extension_offset (title_without_extension);
  if (extension_offset != NULL && extension_offset != title_without_extension)
    {
      extension = g_strdup (extension_offset);
      *extension_offset = '\0';
    }

  id = gdata_entry_get_id (entry);
  ret_val = g_strdup_printf ("%s - %s%s", title_without_extension, id, (extension == NULL) ? "" : extension);

 out:
  if (ret_val == NULL)
    ret_val = g_strdup (title);
  g_free (entry_path);
  g_free (extension);
  g_free (title_without_extension);
  return ret_val;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
is_native_file (GDataEntry *entry)
{
  gchar *content_type;
  gboolean ret = FALSE;

  content_type = get_content_type_from_entry (entry);
  if (content_type != NULL && g_str_has_prefix (content_type, CONTENT_TYPE_PREFIX_GOOGLE))
    ret = TRUE;

  g_free (content_type);

  return ret;
}

static void
build_file_info (GVfsBackendGoogle      *self,
                 GDataEntry             *entry,
                 GFileQueryInfoFlags     flags,
                 GFileInfo              *info,
                 GFileAttributeMatcher  *matcher,
                 gboolean                is_symlink,
                 const gchar            *symlink_name,
                 const gchar            *symlink_target,
                 GError                **error)
{
  GFileType file_type;
  GList *authors;
  GList *links;
  gboolean is_folder = FALSE;
  gboolean is_root = FALSE;
  gboolean has_parent = FALSE;
  const gchar *etag;
  const gchar *id;
  const gchar *name;
  const gchar *title;
  gchar *escaped_name = NULL;
  gchar *content_type = NULL;
  gchar *copy_name = NULL;
  gchar *generated_copy_name = NULL;
  gint64 atime;
  gint64 ctime;
  gint64 mtime;
  gsize i;

  if (GDATA_IS_DOCUMENTS_FOLDER (entry))
    is_folder = TRUE;

  if (entry == self->root)
    is_root = TRUE;

  links = gdata_entry_look_up_links (entry, GDATA_LINK_PARENT);
  has_parent = (links != NULL);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, !is_root);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, is_folder);

  g_file_info_set_is_symlink (info, is_symlink);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_VOLATILE, is_symlink);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, !is_root && has_parent);

  g_file_info_set_is_hidden (info, !has_parent);

  if (is_folder)
    {
      content_type = g_strdup ("inode/directory");
      file_type = G_FILE_TYPE_DIRECTORY;
    }
  else
    {
      content_type = get_content_type_from_entry (entry);
      file_type = G_FILE_TYPE_REGULAR;

      /* We want native Drive content to open in the browser. */
      if (is_native_file (entry))
        {
          GDataLink *alternate;
          const gchar *uri;

          file_type = G_FILE_TYPE_SHORTCUT;
          alternate = gdata_entry_look_up_link (entry, GDATA_LINK_ALTERNATE);
          uri = gdata_link_get_uri (alternate);
          g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, uri);
        }
      else
        {
          goffset size;

#if HAVE_LIBGDATA_0_17_7
          size = gdata_documents_entry_get_file_size (GDATA_DOCUMENTS_ENTRY (entry));
#else
          size = gdata_documents_entry_get_quota_used (GDATA_DOCUMENTS_ENTRY (entry));
#endif
          g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_SIZE, (guint64) size);
        }
    }

  if (is_symlink)
    {
      if (flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS)
        {
          g_free (content_type);
          content_type = g_strdup ("inode/symlink");
          file_type = G_FILE_TYPE_SYMBOLIC_LINK;
        }

      g_file_info_set_symlink_target (info, symlink_target);
    }

  if (content_type != NULL)
    {
      GIcon *icon;
      GIcon *symbolic_icon;

      g_file_info_set_content_type (info, content_type);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, content_type);

      if (file_type == G_FILE_TYPE_DIRECTORY)
        {
          icon = g_themed_icon_new ("folder");
          symbolic_icon = g_themed_icon_new ("folder-symbolic");
        }
      else
        {
          icon = g_content_type_get_icon (content_type);
          symbolic_icon = g_content_type_get_symbolic_icon (content_type);
        }

      g_file_info_set_icon (info, icon);
      g_file_info_set_symbolic_icon (info, symbolic_icon);

      g_object_unref (icon);
      g_object_unref (symbolic_icon);
    }

  g_file_info_set_file_type (info, file_type);

  if (is_root)
    goto out;

  id = gdata_entry_get_id (entry);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ID_FILE, id);

  if (is_symlink)
    name = symlink_name;
  else
    name = id;

  g_file_info_set_name (info, name);

  title = gdata_entry_get_title (entry);
  g_file_info_set_display_name (info, title);
  g_file_info_set_edit_name (info, title);

  generated_copy_name = generate_copy_name (self, entry);

  /* While copying remote Drive content to local storage, we want to
   * create Link-type desktop files because GLocalFile doesn't know
   * about shortcuts. That might change in future.
   */
  if (file_type == G_FILE_TYPE_SHORTCUT)
    {
      copy_name = g_strconcat (generated_copy_name, ".desktop", NULL);
    }
  else
    {
      copy_name = generated_copy_name;
      generated_copy_name = NULL;
    }

  /* Sanitize copy-name by replacing slashes with dashes. This is
   * what nautilus does (for desktop files).
   */
  for (i = 0; copy_name[i] != '\0'; i++)
    {
      if (copy_name[i] == '/')
        copy_name[i] = '-';
    }

  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_COPY_NAME, copy_name);

  atime = gdata_documents_entry_get_last_viewed (GDATA_DOCUMENTS_ENTRY (entry));
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_ACCESS, (guint64) atime);

  ctime = gdata_entry_get_published (entry);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CREATED, (guint64) ctime);

  mtime = gdata_entry_get_updated (entry);
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, (guint64) mtime);

  authors = gdata_entry_get_authors (entry);
  if (authors != NULL)
    {
      GDataAuthor *author = GDATA_AUTHOR (authors->data);
      const gchar *author_name;
      const gchar *email_address;

      author_name = gdata_author_get_name (author);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_OWNER_USER_REAL, author_name);

      email_address = gdata_author_get_email_address (author);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_OWNER_USER, email_address);
    }

  etag = gdata_entry_get_etag (entry);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE, etag);

  if (!is_folder)
    {
      const gchar *thumbnail_uri;

      thumbnail_uri = gdata_documents_document_get_thumbnail_uri (GDATA_DOCUMENTS_DOCUMENT (entry));
      if (thumbnail_uri != NULL && thumbnail_uri[0] != '\0')
        {
          GIcon *preview;
          GMountSpec *spec;

          spec = g_vfs_backend_get_mount_spec (G_VFS_BACKEND (self));
          preview = g_vfs_icon_new (spec, thumbnail_uri);
          g_file_info_set_attribute_object (info, G_FILE_ATTRIBUTE_PREVIEW_ICON, G_OBJECT (preview));
          g_object_unref (preview);
        }
    }

 out:
  g_free (copy_name);
  g_free (generated_copy_name);
  g_free (escaped_name);
  g_free (content_type);
  g_list_free (links);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
remove_monitor_weak_ref (gpointer monitor,
                         gpointer unused,
                         gpointer monitors)
{
  g_object_weak_unref (G_OBJECT (monitor), (GWeakNotify) g_hash_table_remove, monitors);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_copy (GVfsBackend           *_self,
                           GVfsJobCopy           *job,
                           const gchar           *source,
                           const gchar           *destination,
                           GFileCopyFlags         flags,
                           GFileProgressCallback  progress_callback,
                           gpointer               progress_callback_data)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataDocumentsEntry *dummy_source_entry = NULL;
  GDataDocumentsEntry *new_entry = NULL;
  GDataEntry *destination_parent;
  GDataEntry *existing_entry;
  GDataEntry *source_entry;
  GError *error;
  GType source_entry_type;
  gboolean needs_rebuild = FALSE;
  gboolean destination_not_directory = FALSE;
  const gchar *etag;
  const gchar *id;
  const gchar *summary;
  gchar *destination_basename = NULL;
  gchar *entry_path = NULL;
  goffset size;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ copy: %s -> %s, %d\n", source, destination, flags);

  if (flags & G_FILE_COPY_BACKUP)
    {
      /* Return G_IO_ERROR_NOT_SUPPORTED instead of
       * G_IO_ERROR_CANT_CREATE_BACKUP to proceed with the GIO
       * fallback copy.
       */
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation unsupported"));
      goto out;
    }

  source_entry = resolve (self, source, NULL);
  if (source_entry == NULL)
    needs_rebuild = TRUE;

  error = NULL;
  destination_parent = resolve_dir (self, destination, &destination_basename, &error);
  if (error != NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY))
        destination_not_directory = TRUE;
      else
        needs_rebuild = TRUE;

      g_error_free (error);
    }

  if (needs_rebuild)
    {
      error = NULL;
      rebuild_entries (self, cancellable, &error);
      if (error != NULL)
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }

      error = NULL;
      source_entry = resolve (self, source, &error);
      if (error != NULL)
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }

      if (!destination_not_directory)
        {
          g_free (destination_basename);
          destination_basename = NULL;

          error = NULL;
          destination_parent = resolve_dir (self, destination, &destination_basename, &error);
          if (error != NULL)
            {
              g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
              g_error_free (error);
              goto out;
            }
        }
    }

  etag = gdata_entry_get_etag (source_entry);
  id = gdata_entry_get_id (source_entry);
  summary = gdata_entry_get_summary (source_entry);

  /* Fail the job if copy/move operation leads to display name loss.
   * Use G_IO_ERROR_FAILED instead of _NOT_SUPPORTED to avoid r/w fallback.
   * See: https://bugzilla.gnome.org/show_bug.cgi?id=755701 */
  if (g_strcmp0 (id, destination_basename) == 0)
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job),
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                _("Operation unsupported"));
      goto out;
    }

  if (destination_not_directory)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, _("The file is not a directory"));
      goto out;
    }

  existing_entry = resolve_child (self, destination_parent, destination_basename);
  if (existing_entry != NULL)
    {
      if (flags & G_FILE_COPY_OVERWRITE)
        {
          /* We don't support overwrites, so we don't need to care
           * about G_IO_ERROR_IS_DIRECTORY and G_IO_ERROR_WOULD_MERGE.
           */
          g_vfs_job_failed_literal (G_VFS_JOB (job),
                                    G_IO_ERROR,
                                    G_IO_ERROR_NOT_SUPPORTED,
                                    _("Operation unsupported"));
          goto out;
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file already exists"));
          goto out;
        }
    }
  else
    {
      if (GDATA_IS_DOCUMENTS_FOLDER (source_entry))
        {
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR,
                            G_IO_ERROR_WOULD_RECURSE,
                            _("Can’t recursively copy directory"));
          goto out;
        }
    }

  source_entry_type = G_OBJECT_TYPE (source_entry);
  dummy_source_entry = g_object_new (source_entry_type,
                                     "etag", etag,
                                     "id", id,
                                     "summary", summary,
                                     "title", destination_basename,
                                     NULL);

  error = NULL;
  new_entry = gdata_documents_service_add_entry_to_folder (self->service,
                                                           dummy_source_entry,
                                                           GDATA_DOCUMENTS_FOLDER (destination_parent),
                                                           cancellable,
                                                           &error);
  if (error != NULL)
    {
      sanitize_error (&error);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  entry_path = get_entry_path (self, GDATA_ENTRY (new_entry));
  g_debug ("  new entry path: %s\n", entry_path);

  insert_entry (self, GDATA_ENTRY (new_entry));
  g_hash_table_foreach (self->monitors, emit_create_event, entry_path);

#if HAVE_LIBGDATA_0_17_7
  size = gdata_documents_entry_get_file_size (new_entry);
#else
  size = gdata_documents_entry_get_quota_used (new_entry);
#endif
  g_vfs_job_progress_callback (size, size, job);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_clear_object (&dummy_source_entry);
  g_clear_object (&new_entry);
  g_free (destination_basename);
  g_free (entry_path);
  g_debug ("- copy\n");
  g_rec_mutex_unlock (&self->mutex);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
g_vfs_backend_google_create_dir_monitor (GVfsBackend          *_self,
                                         GVfsJobCreateMonitor *job,
                                         const gchar          *filename,
                                         GFileMonitorFlags     flags)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataEntry *entry;
  GError *error;
  GVfsMonitor *monitor = NULL;
  gchar *entry_path = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ create_dir_monitor: %s, %d\n", filename, flags);

  if (flags & G_FILE_MONITOR_SEND_MOVED)
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation unsupported"));
      goto out;
    }

  error = NULL;
  entry = resolve_and_rebuild (self, filename, cancellable, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  entry_path = get_entry_path (self, entry);
  g_debug ("  entry path: %s\n", entry_path);

  if (!GDATA_IS_DOCUMENTS_FOLDER (entry))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, _("The file is not a directory"));
      goto out;
    }

  monitor = g_vfs_monitor_new (_self);
  g_object_set_data_full (G_OBJECT (monitor), "g-vfs-backend-google-path", g_strdup (entry_path), g_free);
  g_hash_table_add (self->monitors, monitor);
  g_object_weak_ref (G_OBJECT (monitor), (GWeakNotify) g_hash_table_remove, self->monitors);
  g_vfs_job_create_monitor_set_monitor (job, monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_clear_object (&monitor);
  g_free (entry_path);
  g_debug ("- create_dir_monitor\n");
  g_rec_mutex_unlock (&self->mutex);
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_delete (GVfsBackend   *_self,
                             GVfsJobDelete *job,
                             const gchar   *filename)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataAuthorizationDomain *auth_domain;
  GDataEntry *entry;
  GError *error;
  gchar *entry_path = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ delete: %s\n", filename);

  error = NULL;
  entry = resolve_and_rebuild (self, filename, cancellable, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  entry_path = get_entry_path (self, entry);
  g_debug ("  entry path: %s\n", entry_path);

  if (entry == self->root)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation unsupported"));
      goto out;
    }

  auth_domain = gdata_documents_service_get_primary_authorization_domain ();

  error = NULL;
  gdata_service_delete_entry (GDATA_SERVICE (self->service), auth_domain, entry, cancellable, &error);
  if (error != NULL)
    {
      sanitize_error (&error);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  remove_entry (self, entry);
  g_hash_table_foreach (self->monitors, emit_delete_event, entry_path);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_free (entry_path);
  g_debug ("- delete\n");
  g_rec_mutex_unlock (&self->mutex);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
rebuild_entries_timeout_cb (gpointer user_data)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (user_data);

  self->entries_stale = TRUE;
  self->entries_stale_timeout = 0;

  return G_SOURCE_REMOVE;
}

static void
g_vfs_backend_google_enumerate (GVfsBackend           *_self,
                                GVfsJobEnumerate      *job,
                                const gchar           *filename,
                                GFileAttributeMatcher *matcher,
                                GFileQueryInfoFlags    flags)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataEntry *entry;
  GError *error;
  GHashTableIter iter;
  gchar *entry_path = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ enumerate: %s\n", filename);

  if (self->entries_stale_timeout == 0)
    {
      self->entries_stale_timeout = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                                                REBUILD_ENTRIES_TIMEOUT,
                                                                rebuild_entries_timeout_cb,
                                                                g_object_ref (self),
                                                                g_object_unref);
    }

  if (self->entries_stale)
    {
      error = NULL;
      rebuild_entries (self, cancellable, &error);
      if (error != NULL)
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }
    }

  error = NULL;
  entry = resolve (self, filename, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  entry_path = get_entry_path (self, entry);
  g_debug ("  entry path: %s\n", entry_path);

  if (!GDATA_IS_DOCUMENTS_FOLDER (entry))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,_("The file is not a directory"));
      goto out;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_hash_table_iter_init (&iter, self->entries);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &entry))
    {
      gchar *path;

      path = get_entry_path (self, entry);
      g_debug ("  found entry: %s\n", path);
      if (path != NULL)
        {
          gchar *parent_path;

          parent_path = g_path_get_dirname (path);
          if (g_strcmp0 (entry_path, parent_path) == 0)
            {
              GFileInfo *info;

              info = g_file_info_new ();
              build_file_info (self, entry, flags, info, matcher, FALSE, NULL, NULL, NULL);
              g_vfs_job_enumerate_add_info (job, info);
              g_object_unref (info);
            }

          g_free (parent_path);
        }

      g_free (path);
    }

  g_vfs_job_enumerate_done (job);

 out:
  g_free (entry_path);
  g_debug ("- enumerate\n");
  g_rec_mutex_unlock (&self->mutex);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_make_directory (GVfsBackend          *_self,
                                     GVfsJobMakeDirectory *job,
                                     const gchar          *filename)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataDocumentsEntry *new_folder = NULL;
  GDataDocumentsFolder *folder = NULL;
  GDataEntry *existing_entry;
  GDataEntry *parent;
  GDataEntry *summary_entry;
  GError *error;
  const gchar *summary;
  gchar *entry_path = NULL;
  gchar *basename = NULL;
  gchar *parent_path = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ make_directory: %s\n", filename);

  if (g_strcmp0 (filename, "/") == 0)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation unsupported"));
      goto out;
    }

  error = NULL;
  parent = resolve_dir_and_rebuild (self, filename, cancellable, &basename, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  parent_path = get_entry_path (self, parent);
  g_debug ("  parent path: %s\n", parent_path);

  summary_entry = g_hash_table_lookup (self->entries, basename);
  if (summary_entry == NULL)
    summary = NULL;
  else
    summary = gdata_entry_get_summary (summary_entry);

  existing_entry = resolve_child (self, parent, basename);
  if (existing_entry != NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file already exists"));
      goto out;
    }

  folder = gdata_documents_folder_new (NULL);
  gdata_entry_set_title (GDATA_ENTRY (folder), basename);
  gdata_entry_set_summary (GDATA_ENTRY (folder), summary);

  error = NULL;
  new_folder = gdata_documents_service_add_entry_to_folder (self->service,
                                                            GDATA_DOCUMENTS_ENTRY (folder),
                                                            GDATA_DOCUMENTS_FOLDER (parent),
                                                            cancellable,
                                                            &error);
  if (error != NULL)
    {
      sanitize_error (&error);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  entry_path = get_entry_path (self, GDATA_ENTRY (new_folder));
  g_debug ("  new entry path: %s\n", entry_path);

  insert_entry (self, GDATA_ENTRY (new_folder));
  g_hash_table_foreach (self->monitors, emit_create_event, entry_path);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_clear_object (&folder);
  g_clear_object (&new_folder);
  g_free (basename);
  g_free (entry_path);
  g_free (parent_path);
  g_debug ("- make_directory\n");
  g_rec_mutex_unlock (&self->mutex);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_mount (GVfsBackend  *_self,
                            GVfsJobMount *job,
                            GMountSpec   *spec,
                            GMountSource *source,
                            gboolean      is_automount)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataAuthorizationDomain *auth_domain;
  GError *error;
  GList *accounts = NULL;
  GList *l;
  gboolean account_found = FALSE;
  const gchar *host;
  const gchar *user;

  g_debug ("+ mount\n");

  error = NULL;
  self->client = goa_client_new_sync (cancellable, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  host = g_mount_spec_get (spec, "host");
  user = g_mount_spec_get (spec, "user");
  self->account_identity = g_strconcat (user, "@", host, NULL);

  accounts = goa_client_get_accounts (self->client);
  for (l = accounts; l != NULL && !account_found; l = l->next)
    {
      GoaAccount *account;
      GoaObject *object = GOA_OBJECT (l->data);
      gchar *account_identity;
      gchar *provider_type;

      account = goa_object_get_account (object);
      account_identity = goa_account_dup_identity (account);
      provider_type = goa_account_dup_provider_type (account);

      if (g_strcmp0 (provider_type, "google") == 0 &&
          g_strcmp0 (account_identity, self->account_identity) == 0)
        {
          GDataGoaAuthorizer *authorizer;

          authorizer = gdata_goa_authorizer_new (object);
          self->service = gdata_documents_service_new (GDATA_AUTHORIZER (authorizer));
          account_found = TRUE;
          g_object_unref (authorizer);
        }

      g_free (provider_type);
      g_free (account_identity);
      g_object_unref (account);
    }

  if (!account_found)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("Invalid mount spec"));
      goto out;
    }

  auth_domain = gdata_documents_service_get_primary_authorization_domain ();

  error = NULL;
  self->root = gdata_service_query_single_entry (GDATA_SERVICE (self->service),
                                                 auth_domain,
                                                 "root",
                                                 NULL,
                                                 GDATA_TYPE_DOCUMENTS_FOLDER,
                                                 cancellable,
                                                 &error);
  if (error != NULL)
    {
      sanitize_error (&error);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_vfs_backend_set_mount_spec (_self, spec);
  g_vfs_backend_set_display_name (_self, self->account_identity);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_list_free_full (accounts, g_object_unref);
  g_debug ("- mount\n");
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_open_icon_for_read (GVfsBackend            *_self,
                                         GVfsJobOpenIconForRead *job,
                                         const gchar            *icon_id)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataAuthorizationDomain *auth_domain;
  GInputStream *stream;

  g_debug ("+ open_icon_for_read: %s\n", icon_id);

  auth_domain = gdata_documents_service_get_primary_authorization_domain ();
  stream = gdata_download_stream_new (GDATA_SERVICE (self->service), auth_domain, icon_id, cancellable);
  if (stream == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED, _("Error getting data from file"));
      goto out;
    }

  g_vfs_job_open_for_read_set_handle (G_VFS_JOB_OPEN_FOR_READ (job), stream);
  g_vfs_job_open_for_read_set_can_seek (G_VFS_JOB_OPEN_FOR_READ (job), TRUE);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_debug ("- open_icon_for_read\n");
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_push (GVfsBackend           *_self,
                           GVfsJobPush           *job,
                           const gchar           *destination,
                           const gchar           *local_path,
                           GFileCopyFlags         flags,
                           gboolean               remove_source,
                           GFileProgressCallback  progress_callback,
                           gpointer               progress_callback_data)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataDocumentsDocument *document = NULL;
  GDataDocumentsDocument *new_document = NULL;
  GDataEntry *destination_parent;
  GDataEntry *existing_entry;
  GDataUploadStream *ostream = NULL;
  GError *error;
  GFile *local_file = NULL;
  GFileInputStream *istream = NULL;
  GFileInfo *info = NULL;
  gboolean needs_overwrite = FALSE;
  const gchar *content_type;
  const gchar *title;
  gchar *destination_basename = NULL;
  gchar *entry_path = NULL;
  goffset size;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ push: %s -> %s, %d\n", local_path, destination, flags);

  if (flags & G_FILE_COPY_BACKUP)
    {
      /* Return G_IO_ERROR_NOT_SUPPORTED instead of
       * G_IO_ERROR_CANT_CREATE_BACKUP to proceed with the GIO
       * fallback copy.
       */
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation unsupported"));
      goto out;
    }

  local_file = g_file_new_for_path (local_path);

  error = NULL;
  info = g_file_query_info (local_file,
                            G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE","
                            G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME","
                            G_FILE_ATTRIBUTE_STANDARD_TYPE,
                            G_FILE_QUERY_INFO_NONE,
                            cancellable,
                            &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  error = NULL;
  destination_parent = resolve_dir_and_rebuild (self, destination, cancellable, &destination_basename, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  existing_entry = resolve_child (self, destination_parent, destination_basename);
  if (existing_entry != NULL)
    {
      if (flags & G_FILE_COPY_OVERWRITE)
        {
          if (GDATA_IS_DOCUMENTS_FOLDER (existing_entry))
            {
              if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
                {
                  g_vfs_job_failed (G_VFS_JOB (job),
                                    G_IO_ERROR,
                                    G_IO_ERROR_WOULD_MERGE,
                                    _("Can’t copy directory over directory"));
                  goto out;
                }
              else
                {
                  g_vfs_job_failed (G_VFS_JOB (job),
                                    G_IO_ERROR,
                                    G_IO_ERROR_IS_DIRECTORY,
                                    _("Can’t copy file over directory"));
                  goto out;
                }
            }
          else if (is_native_file (existing_entry))
            {
              g_vfs_job_failed (G_VFS_JOB (job),
                                G_IO_ERROR,
                                G_IO_ERROR_NOT_REGULAR_FILE,
                                _("Target file is not a regular file"));
              goto out;
            }
          else
            {
              if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
                {
                  g_vfs_job_failed (G_VFS_JOB (job),
                                    G_IO_ERROR,
                                    G_IO_ERROR_WOULD_RECURSE,
                                    _("Can’t recursively copy directory"));
                  goto out;
                }
            }

          needs_overwrite = TRUE;
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file already exists"));
          goto out;
        }
    }
  else
    {
      if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY)
        {
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR,
                            G_IO_ERROR_WOULD_RECURSE,
                            _("Can’t recursively copy directory"));
          goto out;
        }
    }

  g_debug ("  will overwrite: %d\n", needs_overwrite);

  error = NULL;
  istream = g_file_read (local_file, cancellable, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  content_type = g_file_info_get_content_type (info);

  if (needs_overwrite)
    {
      document = GDATA_DOCUMENTS_DOCUMENT (g_object_ref (existing_entry));
      title = gdata_entry_get_title (existing_entry);

      error = NULL;
      ostream = gdata_documents_service_update_document (self->service,
                                                         document,
                                                         title,
                                                         content_type,
                                                         cancellable,
                                                         &error);
    }
  else
    {
      document = gdata_documents_document_new (NULL);
      title = destination_basename;
      gdata_entry_set_title (GDATA_ENTRY (document), title);

      error = NULL;
      ostream = gdata_documents_service_upload_document (self->service,
                                                         document,
                                                         title,
                                                         content_type,
                                                         GDATA_DOCUMENTS_FOLDER (destination_parent),
                                                         cancellable,
                                                         &error);
    }

  if (error != NULL)
    {
      sanitize_error (&error);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  error = NULL;
  g_output_stream_splice (G_OUTPUT_STREAM (ostream),
                          G_INPUT_STREAM (istream),
                          G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                          cancellable,
                          &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  error = NULL;
  new_document = gdata_documents_service_finish_upload (self->service, ostream, &error);
  if (error != NULL)
    {
      sanitize_error (&error);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  entry_path = get_entry_path (self, GDATA_ENTRY (new_document));
  g_debug ("  new entry path: %s\n", entry_path);

  if (needs_overwrite)
    remove_entry (self, existing_entry);

  insert_entry (self, GDATA_ENTRY (new_document));
  g_hash_table_foreach (self->monitors, emit_create_event, entry_path);

  if (remove_source)
    {
      error = NULL;
      g_file_delete (local_file, cancellable, &error);
      if (error != NULL)
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }
    }

#if HAVE_LIBGDATA_0_17_7
  size = gdata_documents_entry_get_file_size (GDATA_DOCUMENTS_ENTRY (new_document));
#else
  size = gdata_documents_entry_get_quota_used (GDATA_DOCUMENTS_ENTRY (new_document));
#endif
  g_vfs_job_progress_callback (size, size, job);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_clear_object (&document);
  g_clear_object (&info);
  g_clear_object (&istream);
  g_clear_object (&local_file);
  g_clear_object (&new_document);
  g_clear_object (&ostream);
  g_free (destination_basename);
  g_free (entry_path);
  g_debug ("- push\n");
  g_rec_mutex_unlock (&self->mutex);
}

/* ---------------------------------------------------------------------------------------------------- */

#if HAVE_LIBGDATA_0_17_9
static void
fs_info_cb (GObject      *source_object,
            GAsyncResult *res,
            gpointer      user_data)
{
  GDataDocumentsService *service = GDATA_DOCUMENTS_SERVICE (source_object);
  GVfsJobQueryFsInfo *job = G_VFS_JOB_QUERY_FS_INFO (user_data);
  GError *error = NULL;
  GDataDocumentsMetadata *metadata;
  goffset total, used;

  metadata = gdata_documents_service_get_metadata_finish (service, res, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  total = gdata_documents_metadata_get_quota_total (metadata);
  used = gdata_documents_metadata_get_quota_used (metadata);
  g_object_unref (metadata);

  if (used >= 0) /* sanity check */
    g_file_info_set_attribute_uint64 (job->file_info, G_FILE_ATTRIBUTE_FILESYSTEM_USED, used);

  if (total >= 0) /* -1 'total' means unlimited quota, just don't report size in that case */
    g_file_info_set_attribute_uint64 (job->file_info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, total);

  if (total >= 0 && used >= 0)
    g_file_info_set_attribute_uint64 (job->file_info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, total - used);

  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_debug ("- query_fs_info\n");
}
#endif

static gboolean
g_vfs_backend_google_query_fs_info (GVfsBackend           *_self,
                                    GVfsJobQueryFsInfo    *job,
                                    const gchar           *filename,
                                    GFileInfo             *info,
                                    GFileAttributeMatcher *matcher)
{
  GMountSpec *spec;
  const gchar *type;

  g_debug ("+ query_fs_info: %s\n", filename);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, FALSE);

  spec = g_vfs_backend_get_mount_spec (_self);
  type = g_mount_spec_get_type (spec);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, type);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, TRUE);

#if HAVE_LIBGDATA_0_17_9
  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_FILESYSTEM_FREE) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_FILESYSTEM_USED))
    {
      GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
      GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
      gdata_documents_service_get_metadata_async (self->service, cancellable, fs_info_cb, job);
      return TRUE;
    }
#endif

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_debug ("- query_fs_info\n");
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_query_info (GVfsBackend           *_self,
                                 GVfsJobQueryInfo      *job,
                                 const gchar           *filename,
                                 GFileQueryInfoFlags    flags,
                                 GFileInfo             *info,
                                 GFileAttributeMatcher *matcher)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataEntry *entry;
  GError *error;
  gboolean is_symlink = FALSE;
  gchar *entry_path = NULL;
  gchar *symlink_name = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ query_info: %s, %d\n", filename, flags);

  error = NULL;
  entry = resolve_and_rebuild (self, filename, cancellable, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  entry_path = get_entry_path (self, entry);
  if (g_strcmp0 (entry_path, filename) != 0) /* volatile */
    {
      is_symlink = TRUE;
      symlink_name = g_path_get_basename (filename);
    }

  g_debug ("  entry path: %s (%d)\n", entry_path, is_symlink);

  error = NULL;
  build_file_info (self, entry, flags, info, matcher, is_symlink, symlink_name, entry_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_free (entry_path);
  g_free (symlink_name);
  g_debug ("- query_info\n");
  g_rec_mutex_unlock (&self->mutex);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_query_info_on_read (GVfsBackend           *_self,
                                         GVfsJobQueryInfoRead  *job,
                                         GVfsBackendHandle      handle,
                                         GFileInfo             *info,
                                         GFileAttributeMatcher *matcher)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GDataEntry *entry;
  GError *error;
  GInputStream *stream = G_INPUT_STREAM (handle);
  gboolean is_symlink = FALSE;
  const gchar *filename;
  gchar *entry_path = NULL;
  gchar *symlink_name = NULL;

  g_debug ("+ query_info_on_read: %p\n", handle);

  entry = g_object_get_data (G_OBJECT (stream), "g-vfs-backend-google-entry");
  filename = g_object_get_data (G_OBJECT (stream), "g-vfs-backend-google-filename");

  entry_path = get_entry_path (self, entry);
  if (g_strcmp0 (entry_path, filename) != 0) /* volatile */
    {
      is_symlink = TRUE;
      symlink_name = g_path_get_basename (filename);
    }

  g_debug ("  entry path: %s (%d)\n", entry_path, is_symlink);

  error = NULL;
  build_file_info (self,
                   entry,
                   G_FILE_QUERY_INFO_NONE,
                   info,
                   matcher,
                   is_symlink,
                   symlink_name,
                   entry_path,
                   &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_free (entry_path);
  g_free (symlink_name);
  g_debug ("- query_info_on_read\n");
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
g_vfs_backend_google_query_info_on_write (GVfsBackend           *_self,
                                          GVfsJobQueryInfoWrite *job,
                                          GVfsBackendHandle      handle,
                                          GFileInfo             *info,
                                          GFileAttributeMatcher *matcher)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GError *error;
  WriteHandle *wh = (WriteHandle *) handle;
  gboolean is_symlink = FALSE;
  gchar *entry_path = NULL;
  gchar *symlink_name = NULL;

  g_debug ("+ query_info_on_write: %p\n", handle);

  entry_path = get_entry_path (self, wh->document);
  if (g_strcmp0 (entry_path, wh->filename) != 0) /* volatile */
    {
      is_symlink = TRUE;
      symlink_name = g_path_get_basename (wh->filename);
    }

  g_debug ("  entry path: %s (%d)\n", entry_path, is_symlink);

  error = NULL;
  build_file_info (self,
                   wh->document,
                   G_FILE_QUERY_INFO_NONE,
                   info,
                   matcher,
                   is_symlink,
                   symlink_name,
                   entry_path,
                   &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_free (entry_path);
  g_free (symlink_name);
  g_debug ("- query_info_on_write\n");
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_open_for_read (GVfsBackend        *_self,
                                    GVfsJobOpenForRead *job,
                                    const gchar        *filename)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataEntry *entry;
  GInputStream *stream;
  GError *error;
  gchar *content_type = NULL;
  gchar *entry_path = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ open_for_read: %s\n", filename);

  error = NULL;
  entry = resolve_and_rebuild (self, filename, cancellable, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  entry_path = get_entry_path (self, entry);
  g_debug ("  entry path: %s\n", entry_path);

  if (GDATA_IS_DOCUMENTS_FOLDER (entry))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY, _("Can’t open directory"));
      goto out;
    }

  content_type = get_content_type_from_entry (entry);
  if (content_type == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED, _("Invalid reply received"));
      goto out;
    }

  /* While copying remote Drive content to local storage, we want to
   * create Link-type desktop files because GLocalFile doesn't know
   * about shortcuts. That might change in future.
   */
  if (g_str_has_prefix (content_type, CONTENT_TYPE_PREFIX_GOOGLE))
    {
      GDataLink *alternate;
      GKeyFile *file;
      const gchar *title;
      const gchar *uri;
      gchar *data;
      gsize length;

      file = g_key_file_new ();

      title = gdata_entry_get_title (entry);
      g_key_file_set_string (file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_NAME, title);
      g_key_file_set_string (file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_TYPE, "Link");

      alternate = gdata_entry_look_up_link (entry, GDATA_LINK_ALTERNATE);
      uri = gdata_link_get_uri (alternate);
      g_key_file_set_string (file, G_KEY_FILE_DESKTOP_GROUP, G_KEY_FILE_DESKTOP_KEY_URL, uri);

      data = g_key_file_to_data (file, &length, NULL);
      stream = g_memory_input_stream_new_from_data (data, (gssize) length, g_free);

      g_key_file_free (file);
    }
  else
    {
      GDataAuthorizationDomain *auth_domain;
      const gchar *uri;

      auth_domain = gdata_documents_service_get_primary_authorization_domain ();
      uri = gdata_entry_get_content_uri (entry);
      stream = gdata_download_stream_new (GDATA_SERVICE (self->service), auth_domain, uri, cancellable);
      if (stream == NULL)
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED, _("Error getting data from file"));
          goto out;
        }
    }

  g_object_set_data_full (G_OBJECT (stream), "g-vfs-backend-google-entry", g_object_ref (entry), g_object_unref);
  g_object_set_data_full (G_OBJECT (stream), "g-vfs-backend-google-filename", g_strdup (filename), g_free);
  g_vfs_job_open_for_read_set_handle (job, stream);
  g_vfs_job_open_for_read_set_can_seek (job, TRUE);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_free (content_type);
  g_free (entry_path);
  g_debug ("- open_for_read\n");
  g_rec_mutex_unlock (&self->mutex);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
read_cb (GObject      *source_object,
         GAsyncResult *res,
         gpointer      user_data)
{
  GError *error = NULL;
  GInputStream *stream = G_INPUT_STREAM (source_object);
  GVfsJobRead *job = G_VFS_JOB_READ (user_data);
  gssize nread;

  nread = g_input_stream_read_finish (stream, res, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_vfs_job_read_set_size (job, (gsize) nread);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_debug ("- read\n");
}

static gboolean
g_vfs_backend_google_read (GVfsBackend       *_self,
                           GVfsJobRead       *job,
                           GVfsBackendHandle  handle,
                           gchar             *buffer,
                           gsize              bytes_requested)
{
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GInputStream *stream = G_INPUT_STREAM (handle);

  g_debug ("+ read: %p\n", handle);
  g_input_stream_read_async (stream, buffer, bytes_requested, G_PRIORITY_DEFAULT, cancellable, read_cb, job);
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_seek_on_read (GVfsBackend       *_self,
                                   GVfsJobSeekRead   *job,
                                   GVfsBackendHandle  handle,
                                   goffset            offset,
                                   GSeekType          type)
{
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GError *error;
  GInputStream *stream = G_INPUT_STREAM (handle);
  goffset cur_offset;

  g_debug ("+ seek_on_read: %p\n", handle);

  error = NULL;
  g_seekable_seek (G_SEEKABLE (stream), offset, type, cancellable, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  cur_offset = g_seekable_tell (G_SEEKABLE (stream));
  g_vfs_job_seek_read_set_offset (job, cur_offset);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_debug ("- seek_on_read\n");
}

/* ---------------------------------------------------------------------------------------------------- */

static void
close_read_cb (GObject      *source_object,
               GAsyncResult *res,
               gpointer      user_data)
{
  GError *error = NULL;
  GInputStream *stream = G_INPUT_STREAM (source_object);
  GVfsJobCloseRead *job = G_VFS_JOB_CLOSE_READ (user_data);

  g_input_stream_close_finish (stream, res, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_object_unref (stream);
  g_debug ("- close_read\n");
}

static gboolean
g_vfs_backend_google_close_read (GVfsBackend       *_self,
                                 GVfsJobCloseRead  *job,
                                 GVfsBackendHandle  handle)
{
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GInputStream *stream = G_INPUT_STREAM (handle);

  g_debug ("+ close_read: %p\n", handle);

  g_input_stream_close_async (stream, G_PRIORITY_DEFAULT, cancellable, close_read_cb, job);

  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_set_display_name (GVfsBackend           *_self,
                                       GVfsJobSetDisplayName *job,
                                       const gchar           *filename,
                                       const gchar           *display_name)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataAuthorizationDomain *auth_domain;
  GDataEntry *entry;
  GDataEntry *new_entry = NULL;
  GError *error;
  gchar *entry_path = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ set_display_name: %s, %s\n", filename, display_name);

  error = NULL;
  entry = resolve_and_rebuild (self, filename, cancellable, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  entry_path = get_entry_path (self, entry);
  g_debug ("  entry path: %s\n", entry_path);

  if (entry == self->root)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation unsupported"));
      goto out;
    }

  gdata_entry_set_title (entry, display_name);
  auth_domain = gdata_documents_service_get_primary_authorization_domain ();

  error = NULL;
  new_entry = gdata_service_update_entry (GDATA_SERVICE (self->service), auth_domain, entry, cancellable, &error);
  if (error != NULL)
    {
      sanitize_error (&error);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  remove_entry (self, entry);
  insert_entry (self, new_entry);
  g_hash_table_foreach (self->monitors, emit_attribute_changed_event, entry_path);
  g_vfs_job_set_display_name_set_new_path (job, entry_path);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_clear_object (&new_entry);
  g_free (entry_path);
  g_debug ("- set_display_name\n");
  g_rec_mutex_unlock (&self->mutex);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_create (GVfsBackend         *_self,
                             GVfsJobOpenForWrite *job,
                             const gchar         *filename,
                             GFileCreateFlags     flags)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataDocumentsDocument *document = NULL;
  GDataDocumentsEntry *new_document = NULL;
  GDataEntry *existing_entry;
  GDataEntry *parent;
  GError *error;
  WriteHandle *handle;
  gchar *basename = NULL;
  gchar *entry_path = NULL;
  gchar *parent_path = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ create: %s, %d\n", filename, flags);

  if (g_strcmp0 (filename, "/") == 0)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation unsupported"));
      goto out;
    }

  error = NULL;
  parent = resolve_dir_and_rebuild (self, filename, cancellable, &basename, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  parent_path = get_entry_path (self, parent);
  g_debug ("  parent path: %s\n", parent_path);

  existing_entry = resolve_child (self, parent, basename);
  if (existing_entry != NULL)
    {
      if (flags & G_FILE_CREATE_REPLACE_DESTINATION)
        {
          g_vfs_job_failed_literal (G_VFS_JOB (job),
                                    G_IO_ERROR,
                                    G_IO_ERROR_NOT_SUPPORTED,
                                    _("Operation unsupported"));
          goto out;
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file already exists"));
          goto out;
        }
    }

  document = gdata_documents_document_new (NULL);
  gdata_entry_set_title (GDATA_ENTRY (document), basename);

  error = NULL;
  new_document = gdata_documents_service_add_entry_to_folder (self->service,
                                                              GDATA_DOCUMENTS_ENTRY (document),
                                                              GDATA_DOCUMENTS_FOLDER (parent),
                                                              cancellable,
                                                              &error);
  if (error != NULL)
    {
      sanitize_error (&error);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  entry_path = get_entry_path (self, GDATA_ENTRY (new_document));
  g_debug ("  new entry path: %s\n", entry_path);

  insert_entry (self, GDATA_ENTRY (new_document));
  g_hash_table_foreach (self->monitors, emit_create_event, entry_path);

  handle = write_handle_new (GDATA_ENTRY (new_document), NULL, filename);
  g_vfs_job_open_for_write_set_handle (job, handle);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_clear_object (&document);
  g_clear_object (&new_document);
  g_free (basename);
  g_free (entry_path);
  g_free (parent_path);
  g_debug ("- create\n");
  g_rec_mutex_unlock (&self->mutex);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_replace (GVfsBackend         *_self,
                              GVfsJobOpenForWrite *job,
                              const gchar         *filename,
                              const gchar         *etag,
                              gboolean             make_backup,
                              GFileCreateFlags     flags)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataDocumentsDocument *document = NULL;
  GDataDocumentsEntry *new_document = NULL;
  GDataEntry *existing_entry;
  GDataEntry *parent;
  GDataUploadStream *stream = NULL;
  GError *error;
  WriteHandle *handle;
  gboolean needs_overwrite = FALSE;
  gchar *basename = NULL;
  gchar *content_type = NULL;
  gchar *entry_path = NULL;
  gchar *parent_path = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ replace: %s, %s, %d, %d\n", filename, etag, make_backup, flags);

  if (make_backup)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_CANT_CREATE_BACKUP,
                        _("Backup file creation failed"));
      goto out;
    }

  if (g_strcmp0 (filename, "/") == 0)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation unsupported"));
      goto out;
    }

  error = NULL;
  parent = resolve_dir_and_rebuild (self, filename, cancellable, &basename, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  parent_path = get_entry_path (self, parent);
  g_debug ("  parent path: %s\n", parent_path);

  existing_entry = resolve_child (self, parent, basename);
  if (existing_entry != NULL)
    {
      if (GDATA_IS_DOCUMENTS_FOLDER (existing_entry))
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY, _("Target file is a directory"));
          goto out;
        }
      else if (is_native_file (existing_entry))
        {
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_REGULAR_FILE,
                            _("Target file is not a regular file"));
          goto out;
        }

      needs_overwrite = TRUE;
    }

  g_debug ("  will overwrite: %d\n", needs_overwrite);

  if (needs_overwrite)
    {
      const gchar *title;

      entry_path = get_entry_path (self, existing_entry);
      g_debug ("  existing entry path: %s\n", entry_path);

      title = gdata_entry_get_title (existing_entry);
      content_type = get_content_type_from_entry (existing_entry);

      error = NULL;
      stream = gdata_documents_service_update_document (self->service,
                                                        GDATA_DOCUMENTS_DOCUMENT (existing_entry),
                                                        title,
                                                        content_type,
                                                        cancellable,
                                                        &error);
      if (error != NULL)
        {
          sanitize_error (&error);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }

      handle = write_handle_new (NULL, stream, filename);
    }
  else
    {
      document = gdata_documents_document_new (NULL);
      gdata_entry_set_title (GDATA_ENTRY (document), basename);

      error = NULL;
      new_document = gdata_documents_service_add_entry_to_folder (self->service,
                                                                  GDATA_DOCUMENTS_ENTRY (document),
                                                                  GDATA_DOCUMENTS_FOLDER (parent),
                                                                  cancellable,
                                                                  &error);
      if (error != NULL)
        {
          sanitize_error (&error);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }

      entry_path = get_entry_path (self, GDATA_ENTRY (new_document));
      g_debug ("  new entry path: %s\n", entry_path);

      insert_entry (self, GDATA_ENTRY (new_document));
      g_hash_table_foreach (self->monitors, emit_create_event, entry_path);

      handle = write_handle_new (GDATA_ENTRY (new_document), NULL, filename);
    }

  g_vfs_job_open_for_write_set_handle (job, handle);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_clear_object (&document);
  g_clear_object (&new_document);
  g_clear_object (&stream);
  g_free (basename);
  g_free (content_type);
  g_free (entry_path);
  g_free (parent_path);
  g_debug ("- replace\n");
  g_rec_mutex_unlock (&self->mutex);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_write (GVfsBackend       *_self,
                            GVfsJobWrite      *job,
                            GVfsBackendHandle  handle,
                            gchar             *buffer,
                            gsize              buffer_size)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GError *error;
  WriteHandle *wh = (WriteHandle *) handle;
  gchar *entry_path = NULL;
  gssize nwrite;

  g_debug ("+ write: %p\n", handle);

  if (wh->stream == NULL)
    {
      const gchar *title;
      gchar *content_type = NULL;

      title = gdata_entry_get_title (wh->document);
      content_type = g_content_type_guess (title, (const guchar *) buffer, buffer_size, NULL);
      g_debug ("  content-type: %s\n", content_type);

      error = NULL;
      wh->stream = gdata_documents_service_update_document (self->service,
                                                            GDATA_DOCUMENTS_DOCUMENT (wh->document),
                                                            title,
                                                            content_type,
                                                            cancellable,
                                                            &error);
      g_free (content_type);

      if (error != NULL)
        {
          sanitize_error (&error);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }
    }

  g_debug ("  writing to stream: %p\n", wh->stream);

  entry_path = get_entry_path (self, wh->document);
  g_debug ("  entry path: %s\n", entry_path);

  error = NULL;
  nwrite = g_output_stream_write (G_OUTPUT_STREAM (wh->stream),
                                  buffer,
                                  buffer_size,
                                  cancellable,
                                  &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_hash_table_foreach (self->monitors, emit_changed_event, entry_path);
  g_vfs_job_write_set_written_size (job, (gsize) nwrite);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_free (entry_path);
  g_debug ("- write\n");
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_close_write (GVfsBackend       *_self,
                                  GVfsJobCloseWrite *job,
                                  GVfsBackendHandle  handle)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataDocumentsDocument *new_document = NULL;
  GError *error;
  WriteHandle *wh = (WriteHandle *) handle;
  gchar *entry_path = NULL;

  g_debug ("+ close_write: %p\n", handle);

  if (!g_output_stream_is_closed (G_OUTPUT_STREAM (wh->stream)))
    {
      error = NULL;
      g_output_stream_close (G_OUTPUT_STREAM (wh->stream), cancellable, &error);
      if (error != NULL)
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }
    }

  error = NULL;
  new_document = gdata_documents_service_finish_upload (self->service, wh->stream, &error);
  if (error != NULL)
    {
      sanitize_error (&error);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }
  else if (new_document == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED, _("Error writing file"));
      goto out;
    }

  entry_path = get_entry_path (self, GDATA_ENTRY (new_document));
  g_debug ("  new entry path: %s\n", entry_path);

  remove_entry (self, wh->document);
  insert_entry (self, GDATA_ENTRY (new_document));
  g_hash_table_foreach (self->monitors, emit_changes_done_event, entry_path);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_clear_object (&new_document);
  write_handle_free (wh);
  g_free (entry_path);
  g_debug ("- close_write\n");
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_dispose (GObject *_self)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);

  if (self->entries_stale_timeout != 0)
    {
      g_source_remove (self->entries_stale_timeout);
      self->entries_stale_timeout = 0;
    }

  if (self->dir_collisions != NULL)
    {
      g_list_free_full (self->dir_collisions, g_object_unref);
      self->dir_collisions = NULL;
    }

  g_clear_object (&self->service);
  g_clear_object (&self->root);
  g_clear_object (&self->client);
  g_clear_pointer (&self->entries, (GDestroyNotify) g_hash_table_unref);
  g_clear_pointer (&self->dir_entries, (GDestroyNotify) g_hash_table_unref);

  G_OBJECT_CLASS (g_vfs_backend_google_parent_class)->dispose (_self);
}

static void
g_vfs_backend_google_finalize (GObject *_self)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);

  g_hash_table_foreach (self->monitors, remove_monitor_weak_ref, self->monitors);
  g_hash_table_unref (self->monitors);
  g_free (self->account_identity);

  g_rec_mutex_clear (&self->mutex);

  G_OBJECT_CLASS (g_vfs_backend_google_parent_class)->finalize (_self);
}

static void
g_vfs_backend_google_class_init (GVfsBackendGoogleClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

  gobject_class->dispose = g_vfs_backend_google_dispose;
  gobject_class->finalize = g_vfs_backend_google_finalize;

  backend_class->try_close_read = g_vfs_backend_google_close_read;
  backend_class->close_write = g_vfs_backend_google_close_write;
  backend_class->copy = g_vfs_backend_google_copy;
  backend_class->create = g_vfs_backend_google_create;
  backend_class->try_create_dir_monitor = g_vfs_backend_google_create_dir_monitor;
  backend_class->delete = g_vfs_backend_google_delete;
  backend_class->enumerate = g_vfs_backend_google_enumerate;
  backend_class->make_directory = g_vfs_backend_google_make_directory;
  backend_class->mount = g_vfs_backend_google_mount;
  backend_class->open_for_read = g_vfs_backend_google_open_for_read;
  backend_class->open_icon_for_read = g_vfs_backend_google_open_icon_for_read;
  backend_class->push = g_vfs_backend_google_push;
  backend_class->try_query_fs_info = g_vfs_backend_google_query_fs_info;
  backend_class->query_info = g_vfs_backend_google_query_info;
  backend_class->query_info_on_read = g_vfs_backend_google_query_info_on_read;
  backend_class->try_query_info_on_write = g_vfs_backend_google_query_info_on_write;
  backend_class->seek_on_read = g_vfs_backend_google_seek_on_read;
  backend_class->set_display_name = g_vfs_backend_google_set_display_name;
  backend_class->try_read = g_vfs_backend_google_read;
  backend_class->replace = g_vfs_backend_google_replace;
  backend_class->write = g_vfs_backend_google_write;
}

static void
g_vfs_backend_google_init (GVfsBackendGoogle *self)
{
  self->entries = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  self->dir_entries = g_hash_table_new_full (entries_in_folder_hash,
                                             entries_in_folder_equal,
                                             dir_entries_key_free,
                                             g_object_unref);
  self->monitors = g_hash_table_new (NULL, NULL);
  g_rec_mutex_init (&self->mutex);
  self->entries_stale = TRUE;
}
