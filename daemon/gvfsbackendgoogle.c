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
#include "gvfsdaemonutils.h"

struct _GVfsBackendGoogle
{
  GVfsBackend parent;
  GDataDocumentsService *service;
  GDataEntry *root;
  GDataEntry *home;
  GDataEntry *shared_with_me_dir;
  GDataEntry *shared_drives_dir;
  GHashTable *entries; /* gchar *entry_id -> GDataEntry */
  GHashTable *dir_entries; /* DirEntriesKey -> GDataEntry */
  GHashTable *dir_timestamps; /* gchar *entry_id -> gint64 *timestamp */
  GHashTable *monitors;
  GList *dir_collisions;
  GList *shared_drives;
  GRecMutex mutex; /* guards cache */
  GoaClient *client;
  gchar *account_identity;
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

#define REBUILD_ENTRIES_TIMEOUT 60 /* s */

#define URI_PREFIX "https://www.googleapis.com/drive/v2/files/"

#define SOURCE_ID_PROPERTY_KEY "GVfsSourceID"
#define PARENT_ID_PROPERTY_KEY "GVfsParentID"

#define ROOT_ID "GVfsRoot"
#define SHARED_WITH_ME_ID "GVfsSharedWithMe"
#define SHARED_DRIVES_ID "GVfsSharedDrives"
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
  gchar *entry_path;
} WriteHandle;

static GDataEntry *resolve_dir (GVfsBackendGoogle  *self,
                                const gchar        *filename,
                                GCancellable       *cancellable,
                                gchar             **out_basename,
                                gchar             **out_path,
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

static void
log_dir_entries (GVfsBackendGoogle *self)
{
  GHashTableIter iter;
  GDataEntry *entry;
  DirEntriesKey *key;
  GList *l;

  if (!g_getenv ("GVFS_GOOGLE_DEBUG"))
    return;

  g_hash_table_iter_init (&iter, self->dir_entries);
  while (g_hash_table_iter_next (&iter, (gpointer *) &key, (gpointer *) &entry))
    {
      g_debug ("  Real ID = %s, (%s, %s) -> %p, %d\n",
               gdata_entry_get_id (GDATA_ENTRY (entry)),
               key->title_or_id,
               key->parent_id,
               entry,
               ((GObject *) entry)->ref_count);
    }

  for (l = self->dir_collisions; l != NULL; l = l->next)
    {
      entry = GDATA_ENTRY (l->data);
      g_debug ("* Real ID = %s, (%s) -> %p, %d\n",
               gdata_entry_get_id (entry),
               gdata_entry_get_title (entry),
               entry,
               ((GObject *) entry)->ref_count);
    }
}

/* ---------------------------------------------------------------------------------------------------- */

static WriteHandle *
write_handle_new (GDataEntry *document, GDataUploadStream *stream, const gchar *filename, const gchar *entry_path)
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
  handle->entry_path = g_strdup (entry_path);

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
  g_free (handle->entry_path);
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

static GList *
get_parent_ids (GVfsBackendGoogle *self,
                GDataEntry        *entry)
{
  GList *l;
  GList *links = NULL;
  GList *ids = NULL;

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
              ids = g_list_prepend (ids, g_strdup (id));
            }
        }
    }

  g_list_free (links);
  return ids;
}

/* ---------------------------------------------------------------------------------------------------- */

static gint64
count_files_in_directory_with_title (GVfsBackendGoogle *self,
                                     const gchar       *entry_title,
                                     GDataEntry        *directory)
{
  gint64 num_same_title_files = 0;
  GHashTableIter iter;
  GDataEntry *entry;
  DirEntriesKey *key;

  g_hash_table_iter_init (&iter, self->dir_entries);
  while (g_hash_table_iter_next (&iter, (gpointer *) &key, (gpointer *) &entry))
    {
      if (g_strcmp0 (entry_title, gdata_entry_get_title (entry)) == 0 &&
          g_strcmp0 (key->parent_id, gdata_entry_get_id (GDATA_ENTRY (directory))) == 0 &&
          g_strcmp0 (key->title_or_id, gdata_entry_get_id (entry)) == 0)
        num_same_title_files++;
    }

  return num_same_title_files;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
is_owner (GVfsBackendGoogle *self, GDataEntry *entry)
{
  GList *l;
  GDataGoaAuthorizer *goa_authorizer;
  GoaAccount *account;
  const gchar *account_identity;

  goa_authorizer = GDATA_GOA_AUTHORIZER (gdata_service_get_authorizer (GDATA_SERVICE (self->service)));
  account = goa_object_peek_account (gdata_goa_authorizer_get_goa_object (goa_authorizer));
  account_identity = goa_account_get_identity (account);

  for (l = gdata_entry_get_authors (entry); l != NULL; l = l->next)
    {
      GDataAuthor *author = GDATA_AUTHOR (l->data);

      if (g_strcmp0 (gdata_author_get_email_address (author), account_identity) == 0) {
          return TRUE;
      }
    }

  return FALSE;
}

static gboolean
is_shared_with_me (GDataEntry *entry)
{
  return gdata_documents_entry_get_shared_with_me_date (GDATA_DOCUMENTS_ENTRY (entry)) > 0;
}

/* ---------------------------------------------------------------------------------------------------- */

static const gchar *
_gdata_documents_entry_get_property_value (GDataDocumentsEntry *entry,
                                           const gchar         *key)
{
  GList *l = NULL;
  GList *document_properties;
  GDataDocumentsProperty *property;

  property = gdata_documents_property_new (key);
  gdata_documents_property_set_visibility (property, GDATA_DOCUMENTS_PROPERTY_VISIBILITY_PRIVATE);

  document_properties = gdata_documents_entry_get_document_properties (GDATA_DOCUMENTS_ENTRY (entry));
  l = g_list_find_custom (document_properties, property, (GCompareFunc) gdata_comparable_compare);

  g_clear_object (&property);
  if (l && l->data)
    return gdata_documents_property_get_value (GDATA_DOCUMENTS_PROPERTY (l->data));
  else
    return NULL;
}

/* ---------------------------------------------------------------------------------------------------- */

/* Since, we now store both the mappings (title, parent_id) -> GDataEntry
 * and (volatile_entry_id, parent_id) -> GDataEntry in the dir_entries hash
 * table, so a case may arise that some (entry_id, parent_id) matches some
 * other entry's (volatile_entry_id, parent_id) tuple. This means that we have
 * a collision on the volatile entry with some other entry's actual ID.
 *
 * The below helper function checks if dir_entries contains a value
 * corresponding to key specified by argument k. If such a value exists, it
 * then checks if both the entries returned have the same ID or not. In case
 * that they have same ID, both entries are guaranteed to be same whereas if the
 * two IDs differ, it means that a collision has ocurred. In this case, we return
 * the ID of the volatile_entry.
 *
 * Semantically, we may have returned a boolean value, but in the case of a
 * collision, we need the volatile entry's ID to log meaningful debug output.
 * So, we decide to return string or NULL. */
static const gchar *
get_volatile_entry_id (GVfsBackendGoogle *self, GDataEntry *entry, DirEntriesKey *k)
{
  GDataEntry *child = NULL;
  const gchar *child_id;

  if ((child = g_hash_table_lookup (self->dir_entries, k)) != NULL)
    {
      child_id = gdata_entry_get_id (child);

      /* Be sure that we are not matching title/id of volatile file. This
       * needs to specifically be taken care of while having files with
       * multiple titles, otherwise we may remove an entry which belonged
       * to some other file and errorneously decrement the ref count of
       * that entry (causing seg-faults later). */
      if (g_strcmp0 (gdata_entry_get_id (entry), child_id) == 0)
        return NULL;
      else
        return child_id;
    }

  return NULL;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
insert_entry_full (GVfsBackendGoogle *self,
                   GDataEntry        *entry,
                   gboolean           track_dir_collisions)
{
  DirEntriesKey *k;
  GDataEntry *old_entry;
  GDataEntry *volatile_entry = NULL;
  gboolean retval = TRUE;
  gboolean insert_title = TRUE;
  const gchar *id;
  const gchar *old_id;
  const gchar *title;
  const gchar *source_id_property;
  GList *parent_ids, *l, *ll;

  id = gdata_entry_get_id (entry);
  title = gdata_entry_get_title (entry);

  g_hash_table_insert (self->entries, g_strdup (id), g_object_ref (entry));

  source_id_property = _gdata_documents_entry_get_property_value (GDATA_DOCUMENTS_ENTRY (entry),
                                                                  SOURCE_ID_PROPERTY_KEY);

  parent_ids = get_parent_ids (self, entry);
  for (l = parent_ids; l != NULL; l = l->next)
    {
      const gchar *parent_id_property;
      gchar *parent_id = l->data;

      k = dir_entries_key_new (id, parent_id);
      old_entry = g_hash_table_lookup (self->dir_entries, k);

      /* Below case happens when I have an entry (say "entry1") which has a GVfsSourceID
       * property (because of a copy operation performed earlier) and the entry
       * corresponding to value of GVfsSourceID property (say "entry2") is moved to the same
       * parent (i.e. the parent of "entry1"), then the entries will collide on
       * the title as well as there will be a contention for inserting real ID
       * of "entry2" vs volatile entry of "entry1".
       *
       * So, we give priority to real ID of "entry1" to maintain the condition that each
       * entry's real ID must be in dir_entries hash table. */
      if (old_entry != NULL && g_strcmp0 (gdata_entry_get_id (old_entry), id) != 0)
        {
          if ((ll = g_list_find (self->dir_collisions, old_entry)) == NULL)
            {
              self->dir_collisions = g_list_prepend (self->dir_collisions, g_object_ref (old_entry));
              g_debug ("  insert_entry: Ejected volatile  (colliding real)  (%s, (%s, %s)) -> %p, %d\n", id, k->title_or_id, k->parent_id, old_entry, ((GObject *)old_entry)->ref_count);
            }
        }

      g_hash_table_insert (self->dir_entries, k, g_object_ref (entry));
      g_debug ("  insert_entry: Inserted real     (%s, %s) -> %p\n", id, parent_id, entry);

      k = dir_entries_key_new (title, parent_id);
      old_entry = g_hash_table_lookup (self->dir_entries, k);
      if (old_entry != NULL)
        {
          old_id = gdata_entry_get_id (old_entry);
          g_debug ("  title collision of %s with: (%s, %s) -> %p\n", id, title, parent_id, old_entry);
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
              if ((ll = g_list_find (self->dir_collisions, old_entry)) == NULL)
                {
                  self->dir_collisions = g_list_prepend (self->dir_collisions, g_object_ref (old_entry));

                  g_debug ("  insert_entry: Ejected title    (%s, (%s, %s)) -> %p, %d\n", old_id, title, parent_id, old_entry, ((GObject *)old_entry)->ref_count);
                }
            }

          g_hash_table_insert (self->dir_entries, k, g_object_ref (entry));
          g_debug ("  insert_entry: Inserted title    (%s, %s) -> %p\n", title, parent_id, entry);
        }
      else
        {
          if (track_dir_collisions)
            {
              if ((ll = g_list_find (self->dir_collisions, entry)) == NULL)
                {
                  self->dir_collisions = g_list_prepend (self->dir_collisions, g_object_ref (entry));
                  g_debug ("  insert_entry: Skipped title    (%s, %s, %s) -> %p, %d\n", id, title, parent_id, entry, ((GObject *)entry)->ref_count);
                }
            }

          dir_entries_key_free (k);
        }

      parent_id_property = _gdata_documents_entry_get_property_value (GDATA_DOCUMENTS_ENTRY (entry),
                                                                      PARENT_ID_PROPERTY_KEY);

      if (source_id_property && (g_strcmp0 (parent_id_property, parent_id) == 0))
        {
          k = dir_entries_key_new (source_id_property, parent_id);
          volatile_entry = g_hash_table_lookup (self->dir_entries, k);

          if (volatile_entry == NULL)
            {
              g_hash_table_insert (self->dir_entries, k, g_object_ref (entry));
              g_debug ("  insert_entry: Inserted volatile (%s, %s) -> %p\n", source_id_property, parent_id, entry);
            }
          else
            {
              /* This case only occurs in the following scenario:
               * Suppose I have a file with title "t1" and ID "id1". Suppose we
               * wish to copy this file "t1" over to some folder "folder1".
               * Now, firstly I touch (create) a new file in "folder1" and I
               * set the title of this new file to "id1". So, this file has its
               * ID (say "id2") and title "id1".
               *
               * Now, we copy the file "id1" into "folder1". When we're trying
               * to do so, the volatile path entry which needs to be inserted
               * consists of mapping `("id1", "folder1_ID") -> GDataEntry` but
               * since we had already created the file with title "id1" inside
               * of "folder1", we get a collision of a volatile entry with some
               * file's actual title. If we ignore this case here, the code
               * will definitely work and the copy operation will succeed but
               * when `query_info` is performed on the entry
               * `("id1", "folder1_ID") -> GDataEntry`, we will get the older
               * file and not the newer one (which nautilus tries to find). So,
               * we treat this case just like that of a title collision.
               *
               * This case shouldn't normally happen since it entails that file
               * names be completely gibberish (like IDs), but for the sake of
               * completeness, we handle it here. */
              if (track_dir_collisions)
                {
                  /* The below string comparison is used to check if the
                   * (id, parent_id) tuple corresponds to some other entry's real ID or
                   * of volatile entry. It may so happen that some file which is to
                   * be moved/copied will have a volatile entry kept inside
                   * GVfsSourceID GDataDocumentsProperty, and that this ID equals
                   * to some other file's real ID. The highest precedence is given
                   * to real ID, hence we put the volatile entry (here, `id`) into
                   * the dir_collisions list. The inherent knowledge here is that
                   * two different entries can never have same real ID. */
                  if (g_strcmp0 (k->title_or_id, gdata_entry_get_id (volatile_entry)) == 0)
                    {
                      if ((ll = g_list_find (self->dir_collisions, entry)) == NULL)
                        {
                          self->dir_collisions = g_list_prepend (self->dir_collisions, g_object_ref (entry));
                          g_debug ("  insert_entry: Inserted volatile to dir_collisions (%s, (%s, %s)) -> %p, %d\n",
                                   gdata_entry_get_id (volatile_entry),
                                   source_id_property,
                                   parent_id,
                                   volatile_entry,
                                   ((GObject *)entry)->ref_count);
                        }
                    }
                  else
                    {
                      /* (id, parent_id) tuple maps to some other entry (here volatile_entry's)
                       * real ID. So, we prepend `entry` to dir_collisions instead of
                       * ejecting volatile_entry. */

                      if ((ll = g_list_find (self->dir_collisions, entry)) == NULL)
                        {
                          self->dir_collisions = g_list_prepend (self->dir_collisions, g_object_ref (volatile_entry));
                          g_debug ("  insert_entry: Ejected volatile  (%s, (%s, %s)) -> %p, %d\n",
                                   gdata_entry_get_id (volatile_entry),
                                   source_id_property,
                                   parent_id,
                                   volatile_entry,
                                   ((GObject *)volatile_entry)->ref_count);
                        }

                      g_hash_table_insert (self->dir_entries, k, g_object_ref (entry));
                      g_debug ("  insert_entry: Inserted volatile (%s, %s) -> %p, %d\n", source_id_property, parent_id, entry, ((GObject *)entry)->ref_count);
                    }
                }
            }
        }

      retval &= insert_title && (!source_id_property ||  !volatile_entry);
    }
  g_list_free_full (parent_ids, g_free);

  return retval;
}

static void
insert_entry (GVfsBackendGoogle *self,
              GDataEntry        *entry)
{
  gint64 *timestamp;

  timestamp = g_new (gint64, 1);
  *timestamp =  g_get_real_time ();
  g_object_set_data_full (G_OBJECT (entry), "timestamp", timestamp, g_free);

  insert_entry_full (self, entry, TRUE);
}

static void
insert_custom_entry (GVfsBackendGoogle *self,
                     GDataEntry        *entry,
                     const gchar       *parent_id)
{
  DirEntriesKey *k;
  const gchar *id;
  const gchar *title;

  id = gdata_entry_get_id (entry);
  title = gdata_entry_get_title (entry);

  g_hash_table_insert (self->entries, g_strdup (id), g_object_ref (entry));

  k = dir_entries_key_new (id, parent_id);
  g_hash_table_insert (self->dir_entries, k, g_object_ref (entry));

  k = dir_entries_key_new (title, parent_id);
  g_hash_table_insert (self->dir_entries, k, g_object_ref (entry));
}

static void
remove_entry_full (GVfsBackendGoogle *self,
                   GDataEntry        *entry,
                   gboolean           restore_dir_collisions)
{
  DirEntriesKey *k;
  GList *l;
  const gchar *id;
  const gchar *title;
  GList *parent_ids, *ll;

  id = gdata_entry_get_id (entry);
  title = gdata_entry_get_title (entry);

  g_hash_table_remove (self->entries, id);

  if (is_shared_with_me (entry))
    g_hash_table_remove (self->dir_timestamps, SHARED_WITH_ME_ID);

  parent_ids = get_parent_ids (self, entry);
  for (ll = parent_ids; ll != NULL; ll = ll->next)
    {
      const gchar *source_id_property = NULL;
      const gchar *parent_id_property = NULL;
      const gchar *volatile_entry_id = NULL;
      gchar *parent_id = ll->data;

      source_id_property = _gdata_documents_entry_get_property_value (GDATA_DOCUMENTS_ENTRY (entry),
                                                                      SOURCE_ID_PROPERTY_KEY);

      g_hash_table_remove (self->dir_timestamps, parent_id);

      k = dir_entries_key_new (id, parent_id);
      if (g_hash_table_remove (self->dir_entries, k))
        g_debug ("  remove_entry: Removed real      (%s, %s) -> %p\n", id, parent_id, entry);
      dir_entries_key_free (k);

      k = dir_entries_key_new (title, parent_id);

      if ((volatile_entry_id = get_volatile_entry_id (self, entry, k)) != NULL)
        g_debug ("  skipping removal of %s as it is volatile path for %s\n", id, volatile_entry_id);
      else if (g_hash_table_remove (self->dir_entries, k))
        g_debug ("  remove_entry: Removed title     (%s, %s) -> %p\n", title, parent_id, entry);

      dir_entries_key_free (k);

      parent_id_property = _gdata_documents_entry_get_property_value (GDATA_DOCUMENTS_ENTRY (entry),
                                                                      PARENT_ID_PROPERTY_KEY);
      if (source_id_property && (g_strcmp0 (parent_id_property, parent_id) == 0))
        {
          k = dir_entries_key_new (source_id_property, parent_id);

          if ((volatile_entry_id = get_volatile_entry_id (self, entry, k)) != NULL)
            g_debug ("  skipping removal of %s as it is volatile path for %s\n", id, volatile_entry_id);
          else if (g_hash_table_remove (self->dir_entries, k))
            g_debug ("  remove_entry: Removed volatile  (%s, %s) -> %p\n", source_id_property, parent_id, entry);

          dir_entries_key_free (k);
        }

      /* Lets remove the entry from the dir_collisions list as well. It's
       * guaranteed that at a time, only 1 entry will exist in dir_collisions */
      l = g_list_find (self->dir_collisions, entry);
      if (l != NULL)
        {
          self->dir_collisions = g_list_remove_link (self->dir_collisions, l);
          g_object_unref (entry);
          g_list_free (l);
        }

      if (restore_dir_collisions)
        {
          for (l = self->dir_collisions; l != NULL; l = l->next)
            {
              GDataEntry *colliding_entry = GDATA_ENTRY (l->data);

              if (insert_entry_full (self, colliding_entry, FALSE))
                {
                  self->dir_collisions = g_list_remove_link (self->dir_collisions, l);
                  g_debug ("  remove_entry: Restored %p\n", colliding_entry);
                  g_list_free (l);
                  g_object_unref (colliding_entry);
                }
            }
        }
    }
  g_list_free_full (parent_ids, g_free);
}

static void
remove_entry (GVfsBackendGoogle *self,
              GDataEntry        *entry)
{
  remove_entry_full (self, entry, TRUE);
}

static void
remove_dir (GVfsBackendGoogle *self,
            GDataEntry        *parent)
{
  GHashTableIter iter;
  GDataEntry *entry;
  gchar *parent_id;
  GList *l;

  /* g_strdup() is necessary to prevent segfault because gdata_entry_get_id() calls g_free() */
  parent_id = g_strdup (gdata_entry_get_id (parent));

  g_hash_table_remove (self->dir_timestamps, parent_id);

  g_hash_table_iter_init (&iter, self->entries);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &entry))
    {
      DirEntriesKey *k;
      const gchar *volatile_entry_id;

      k = dir_entries_key_new (gdata_entry_get_id (entry), parent_id);

      if (g_hash_table_lookup (self->dir_entries, k) != NULL)
        {
          if ((volatile_entry_id = get_volatile_entry_id (self, entry, k)) != NULL)
            {
              g_debug ("Skipping removal of (%s, %s) as it is volatile path for %s\n",
                       k->title_or_id,
                       k->parent_id,
                       volatile_entry_id);
              dir_entries_key_free (k);
              continue;
            }

          g_object_ref (entry);
          g_hash_table_iter_remove (&iter);
          remove_entry_full (self, entry, FALSE);
          g_object_unref (entry);
        }

      dir_entries_key_free (k);
    }

  for (l = self->dir_collisions; l != NULL; l = l->next)
  {
    GDataEntry *colliding_entry = GDATA_ENTRY (l->data);

    if (insert_entry_full (self, colliding_entry, FALSE))
      {
        self->dir_collisions = g_list_remove_link (self->dir_collisions, l);
        g_debug ("  remove_entry: Restored %p\n", colliding_entry);
        g_list_free (l);
        g_object_unref (colliding_entry);
      }
  }

  g_free (parent_id);
}

static gboolean
is_entry_valid (GDataEntry *entry)
{
  gint64 *timestamp;

  timestamp = g_object_get_data (G_OBJECT (entry), "timestamp");
  if (timestamp == NULL)
    return TRUE;

  return (g_get_real_time () - *timestamp < REBUILD_ENTRIES_TIMEOUT * G_USEC_PER_SEC);
}

static gboolean
is_dir_listing_valid (GVfsBackendGoogle *self, GDataEntry *parent)
{
  gint64 *timestamp;

  if (parent == self->root)
    return TRUE;

  timestamp = g_hash_table_lookup (self->dir_timestamps, gdata_entry_get_id (parent));
  if (timestamp != NULL)
    return (g_get_real_time () - *timestamp < REBUILD_ENTRIES_TIMEOUT * G_USEC_PER_SEC);

  return FALSE;
}

static void
rebuild_shared_drives_dir (GVfsBackendGoogle  *self,
                           GCancellable       *cancellable,
                           GError            **error)

{
  GDataAuthorizationDomain *auth_domain;
  GList *l;
  gint64 *timestamp;

  auth_domain = gdata_documents_service_get_primary_authorization_domain ();

  remove_dir (self, self->shared_drives_dir);

  for (l = self->shared_drives; l != NULL; l = l->next)
    {
      GDataDocumentsDrive *drive = GDATA_DOCUMENTS_DRIVE (l->data);
      GDataEntry *entry = NULL;

      entry = gdata_service_query_single_entry (GDATA_SERVICE (self->service),
                                                auth_domain,
                                                gdata_entry_get_id (GDATA_ENTRY (drive)),
                                                NULL,
                                                GDATA_TYPE_DOCUMENTS_FOLDER,
                                                cancellable,
                                                error);
      if (entry == NULL)
        return;

      /* Replace "My Drive" title by the real name of the Drive. */
      gdata_entry_set_title (entry, gdata_documents_drive_get_name (drive));

      insert_custom_entry (self, entry, SHARED_DRIVES_ID);

      g_object_unref (entry);
    }

  timestamp = g_new (gint64, 1);
  *timestamp = g_get_real_time ();
  g_hash_table_insert (self->dir_timestamps, SHARED_DRIVES_ID, timestamp);
}

static void
rebuild_dir (GVfsBackendGoogle  *self,
             GDataEntry         *parent,
             GCancellable       *cancellable,
             GError            **error)
{
  GDataDocumentsFeed *feed = NULL;
  GDataDocumentsQuery *query = NULL;
  gboolean succeeded_once = FALSE;
  gchar *search;
  gchar *parent_id;

  if (parent == self->shared_drives_dir)
    {
      rebuild_shared_drives_dir (self, cancellable, error);
      return;
    }

  /* g_strdup() is necessary to prevent segfault because gdata_entry_get_id() calls g_free() */
  parent_id = g_strdup (gdata_entry_get_id (parent));

  if (parent == self->shared_with_me_dir)
    search = g_strdup ("sharedWithMe");
  else
    search = g_strdup_printf ("'%s' in parents", parent_id);
  query = gdata_documents_query_new_with_limits (search, 1, G_MAXUINT);
  gdata_documents_query_set_show_folders (query, TRUE);
  g_free (search);

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

          goto out;
        }

      if (!succeeded_once)
        {
          gint64 *timestamp;

          remove_dir (self, parent);

          timestamp = g_new (gint64, 1);
          *timestamp = g_get_real_time ();
          g_hash_table_insert (self->dir_timestamps, g_strdup (parent_id), timestamp);

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

 out:
  g_clear_object (&feed);
  g_clear_object (&query);
  g_free (parent_id);
}

/* ---------------------------------------------------------------------------------------------------- */

static GDataEntry *
resolve_child (GVfsBackendGoogle  *self,
               GDataEntry         *parent,
               const gchar        *basename,
               GCancellable       *cancellable,
               GError            **error)
{
  DirEntriesKey *k;
  GDataEntry *entry;
  const gchar *parent_id;
  GError *local_error = NULL;
  gboolean is_shared_with_me_dir = (parent == self->shared_with_me_dir);

  parent_id = gdata_entry_get_id (parent);
  k = dir_entries_key_new (basename, parent_id);

  if (is_shared_with_me_dir)
    entry = g_hash_table_lookup (self->entries, basename);
  else
    entry = g_hash_table_lookup (self->dir_entries, k);

  if ((entry == NULL && !is_dir_listing_valid (self, parent)) ||
      (entry != NULL && !is_entry_valid (entry)))
    {
      rebuild_dir (self, parent, cancellable, &local_error);
      if (local_error != NULL)
        {
          g_propagate_error (error, local_error);
          goto out;
        }

      if (is_shared_with_me_dir)
        entry = g_hash_table_lookup (self->entries, basename);
      else
        entry = g_hash_table_lookup (self->dir_entries, k);
    }

  if (entry == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("No such file or directory"));
      goto out;
    }

 out:
  dir_entries_key_free (k);

  return entry;
}

static GDataEntry *
resolve (GVfsBackendGoogle  *self,
         const gchar        *filename,
         GCancellable       *cancellable,
         gchar             **out_path,
         GError            **error)
{
  GDataEntry *parent;
  GDataEntry *ret_val = NULL;
  GError *local_error;
  gchar *basename = NULL;

  g_assert (filename && filename[0] == '/');

  if (g_strcmp0 (filename, "/") == 0)
    {
      ret_val = self->root;

      if (out_path != NULL)
        *out_path = g_strdup ("/");

      goto out;
    }

  local_error = NULL;
  parent = resolve_dir (self, filename, cancellable, &basename, out_path, &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, local_error);
      goto out;
    }

  ret_val = resolve_child (self, parent, basename, cancellable, &local_error);
  if (ret_val == NULL)
    {
      /* This fallback provides volatile entries for URIs which was used
       * before My Drive folder was added in the root. */
      if (parent == self->root)
        {
          g_clear_error (&local_error);
          ret_val = resolve_child (self, self->home, basename, cancellable, &local_error);
          if (ret_val != NULL && out_path != NULL)
            {
              gchar *tmp;
              tmp = g_build_path ("/", *out_path, gdata_entry_get_id (self->home), NULL);
              g_free (*out_path);
              *out_path = tmp;
            }
        }

      if (ret_val == NULL)
        {
          g_propagate_error (error, local_error);
          goto out;
        }
    }

  if (out_path != NULL)
    {
      gchar *tmp;
      tmp = g_build_path ("/", *out_path, gdata_entry_get_id (ret_val), NULL);
      g_free (*out_path);
      *out_path = tmp;
    }

 out:
  g_free (basename);
  return ret_val;
}

static GDataEntry *
resolve_dir (GVfsBackendGoogle  *self,
             const gchar        *filename,
             GCancellable       *cancellable,
             gchar             **out_basename,
             gchar             **out_path,
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
  parent = resolve (self, parent_path, cancellable, out_path, &local_error);
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
generate_copy_name (GVfsBackendGoogle *self, GDataEntry *entry, const gchar *entry_path)
{
  GDataEntry *existing_entry;
  GDataEntry *parent;
  const gchar *id;
  const gchar *title;
  gchar *extension = NULL;
  gchar *extension_offset;
  gchar *ret_val = NULL;
  gchar *title_without_extension = NULL;

  title = gdata_entry_get_title (entry);

  parent = resolve_dir (self, entry_path, NULL, NULL, NULL, NULL);
  if (parent == NULL)
    goto out;

  existing_entry = resolve_child (self, parent, title, NULL, NULL);
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
                 const gchar            *filename,
                 const gchar            *entry_path,
                 GError                **error)
{
  GFileType file_type;
  GList *authors;
  gboolean is_folder = FALSE;
  gboolean is_root = FALSE;
  gboolean is_symlink = FALSE;
  const gchar *etag;
  const gchar *id;
  const gchar *name;
  const gchar *title;
  gchar *escaped_name = NULL;
  gchar *content_type = NULL;
  gchar *copy_name = NULL;
  gchar *symlink_name = NULL;
  gint64 atime;
  gint64 ctime;
  gint64 mtime;
  gsize i;
  gboolean is_shared_with_me_dir = (entry == self->shared_with_me_dir);
  gboolean is_home = (entry == self->home);
  gboolean is_shared_drives_dir = (entry == self->shared_drives_dir);
  gboolean can_edit;

  if (GDATA_IS_DOCUMENTS_FOLDER (entry))
    is_folder = TRUE;

  if (entry == self->root)
    is_root = TRUE;

  if (filename != NULL && g_strcmp0 (entry_path, filename) != 0) /* volatile */
    {
      is_symlink = TRUE;
      symlink_name = g_path_get_basename (filename);
    }

  /* TODO: It is not always possible to rename, delete, or list children.
   * However, the proper implementation of gdata_documents_entry_can_rename/
   * _delete/_list_children would require port to Google Drive API v3. */
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME,
                                     !is_root && !is_home && !is_shared_with_me_dir && !is_shared_drives_dir);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, is_folder);

  g_file_info_set_is_symlink (info, is_symlink);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_VOLATILE, is_symlink);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE,
                                     !is_root && !is_home && !is_shared_with_me_dir && !is_shared_drives_dir);

  can_edit = gdata_documents_entry_can_edit (GDATA_DOCUMENTS_ENTRY (entry));
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
                                     !is_root && !is_shared_with_me_dir && !is_shared_drives_dir && can_edit);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);

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

          size = gdata_documents_entry_get_file_size (GDATA_DOCUMENTS_ENTRY (entry));
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

      g_file_info_set_symlink_target (info, entry_path);
    }

  if (content_type != NULL)
    {
      GIcon *icon;
      GIcon *symbolic_icon;

      g_file_info_set_content_type (info, content_type);
      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, content_type);

      if (is_home)
        {
          icon = g_themed_icon_new_with_default_fallbacks ("user-home");
          symbolic_icon = g_themed_icon_new_with_default_fallbacks ("user-home-symbolic");
        }
      else if (is_shared_with_me_dir)
        {
          icon = g_themed_icon_new_with_default_fallbacks ("folder-publicshare");
          symbolic_icon = g_themed_icon_new_with_default_fallbacks ("folder-publicshare-symbolic");
        }
      else if (is_shared_drives_dir)
        {
          icon = g_themed_icon_new_with_default_fallbacks ("folder-remote");
          symbolic_icon = g_themed_icon_new_with_default_fallbacks ("folder-remote-symbolic");
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

  id = gdata_entry_get_id (entry);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ID_FILE, id);

  if (is_root)
    name = "/";
  else if (is_symlink)
    name = symlink_name;
  else
    name = id;

  g_file_info_set_name (info, name);

  title = gdata_entry_get_title (entry);
  g_file_info_set_display_name (info, title);
  g_file_info_set_edit_name (info, title);

  if (is_root || is_home || is_shared_with_me_dir || is_shared_drives_dir)
    goto out;

  copy_name = generate_copy_name (self, entry, entry_path);

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
  g_free (symlink_name);
  g_free (copy_name);
  g_free (escaped_name);
  g_free (content_type);
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
  GDataEntry *source_parent;
  GDataEntry *existing_entry;
  GDataEntry *source_entry;
  GError *error;
  GType source_entry_type;
  const gchar *etag;
  const gchar *id;
  const gchar *source_parent_id;
  const gchar *destination_parent_id;
  const gchar *summary;
  const gchar *title;
  const gchar *dummy_entry_filename;
  gboolean will_overwrite = FALSE;
  gchar *destination_basename = NULL;
  gchar *entry_path = NULL;
  goffset size;
  gchar *parent_path = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ copy: %s -> %s, %d\n", source, destination, flags);
  log_dir_entries (self);

  if (flags & G_FILE_COPY_BACKUP)
    {
      /* Return G_IO_ERROR_NOT_SUPPORTED instead of
       * G_IO_ERROR_CANT_CREATE_BACKUP to proceed with the GIO
       * fallback copy.
       */
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  error = NULL;
  source_entry = resolve (self, source, cancellable, NULL, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  source_parent = resolve_dir (self, source, cancellable, NULL, NULL, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  destination_parent = resolve_dir (self, destination, cancellable, &destination_basename, &parent_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  if (source_entry == self->root || source_parent == self->root || source_parent == self->shared_drives_dir ||
      destination_parent == self->root || destination_parent == self->shared_with_me_dir ||
      destination_parent == self->shared_drives_dir)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  id = gdata_entry_get_id (source_entry);
  title = gdata_entry_get_title (source_entry);
  source_parent_id = gdata_entry_get_id (source_parent);
  destination_parent_id = gdata_entry_get_id (destination_parent);

  g_debug ("  wants overwrite: %d\n", flags & G_FILE_COPY_OVERWRITE);

  existing_entry = resolve_child (self, destination_parent, destination_basename, cancellable, NULL);
  if (existing_entry != NULL)
    {
      gint64 num_same_title_files;

      num_same_title_files = count_files_in_directory_with_title (self,
                                                                  destination_basename,
                                                                  destination_parent);

      g_debug ("  count of files with title same as destination_basename: %ld\n", num_same_title_files);

      if (flags & G_FILE_COPY_OVERWRITE)
        {
          if (num_same_title_files > 1)
            {
              g_vfs_job_failed_literal (G_VFS_JOB (job),
                                        G_IO_ERROR,
                                        G_IO_ERROR_FAILED,
                                        _("Operation not supported"));
              goto out;
            }

          /* If the destination-basename matches that of some other entry, we
           * further check if the files have the same ID or not. If the source
           * file as well as the destination file have same ID, we fail the
           * operation. */
          if (g_strcmp0 (gdata_entry_get_id (existing_entry), gdata_entry_get_id (source_entry)) == 0)
            {
              /* We return G_IO_ERROR_FAILED below instead of
               * G_IO_ERROR_NOT_SUPPORTED because _NOT_SUPPORTED implies that the
               * operation hasn't been implemented yet, and hence GIO takes a
               * fallback to perform the operation. So, for copy operation with
               * overwrite, it uses a (replace + read + write) fallback which in
               * turn overwrites the file even though we don't support overwrites.
               *
               * For a concrete case see this discussion:
               * https://gitlab.gnome.org/GNOME/gvfs/merge_requests/58#note_584083
               */
              g_vfs_job_failed_literal (G_VFS_JOB (job),
                                        G_IO_ERROR,
                                        G_IO_ERROR_FAILED,
                                        _("Operation not supported"));
              goto out;
            }
          else if (g_strcmp0 (gdata_entry_get_id (existing_entry), destination_basename) == 0 ||
                   g_strcmp0 (gdata_entry_get_title (existing_entry), destination_basename) == 0)
            will_overwrite = TRUE;
          else
            {
              /* Neither the ID nor the title of existing_entry matches the destination_basename.
               * This can only happen when destination_basename corresponds to the volatile entry
               * being pointed to by some other file. We simply ignore it and let the copy
               * operation to happen. */
            }
        }
      else
        {
          /* We just check for two conditions here (out of a total of 3 we checked above when
           * G_FILE_COPY_OVERWRITE is specified). The first condition "num_same_title_files > 0"
           * checks if we have multiple files with same destination_name in the destination
           * directory, whereas the second condition specifically checks for the case when we have a
           * file whose ID is same as that of source file (i.e. the same file with multiple
           * parents).
           *
           * In the case our destination_name matches the volatile entry of some other file, we
           * simply ignore it and let the copy operation to take place. */
          if (num_same_title_files > 0 ||
              g_strcmp0 (gdata_entry_get_id (existing_entry), destination_basename) == 0)
            {
              g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file already exists"));
              goto out;
            }
        }
    }

  /* For the cases like `gio copy id1 ./folder/`, the operation actually resolves to its longer form
   * `gio copy id1 ./folder/id1`. We cater such cases differently and check if another file with the
   * same title as that of file with real ID id1 exists in the destination folder. This check allows
   * us to be as similar to POSIX filesystem operations as possible. This way, we can make
   * title-based copy-overwrite operations to work in nautilus. */
  if (g_strcmp0 (destination_basename, id) == 0)
    {
      existing_entry = resolve_child (self, destination_parent, title, cancellable, NULL);
      if (existing_entry != NULL)
        {
          gint64 num_same_title_files;

          num_same_title_files = count_files_in_directory_with_title (self, title, destination_parent);
          g_debug ("  count of same title files: %ld\n", num_same_title_files);

          if (flags & G_FILE_COPY_OVERWRITE)
            {
              if (num_same_title_files > 1)
                {
                  /* We return G_IO_ERROR_FAILED below instead of
                   * G_IO_ERROR_NOT_SUPPORTED because _NOT_SUPPORTED implies that the
                   * operation hasn't been implemented yet, and hence GIO takes a
                   * fallback to perform the operation. So, for copy operation with
                   * overwrite, it uses a (replace + read + write) fallback which in
                   * turn overwrites the file even though we don't support overwrites.
                   *
                   * For a concrete case see this discussion:
                   * https://gitlab.gnome.org/GNOME/gvfs/merge_requests/58#note_584083
                   */
                  g_vfs_job_failed_literal (G_VFS_JOB (job),
                                            G_IO_ERROR,
                                            G_IO_ERROR_FAILED,
                                            _("Operation not supported"));
                  goto out;
                }

              if (GDATA_IS_DOCUMENTS_FOLDER (existing_entry))
                {
                  if (GDATA_IS_DOCUMENTS_FOLDER (source_entry))
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

              will_overwrite = (num_same_title_files == 1);
            }
          else
            {
              /* If the title matches the volatile path of some
               * other file (given by existing_entry) and if the titles are
               * different, move operation should be allowed. In that case, we will
               * be having two different entries (each with different titles), but
               * the stored volatile path (in GDataDocumentsProperty) will be same.
               *
               * This isn't an issue since we've already handled those extra
               * entries in insert_entry_full().
               */
              if (num_same_title_files > 0)
                {
                  g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file already exists"));
                  goto out;
                }
            }
        }
    }

  g_debug ("  will overwrite: %d\n", will_overwrite);

  /* We again resolve the source_entry after checking existing_entry. This is
   * because when we try to find existing_entry, resolve_child is called which
   * internally calls rebuild_dir function. Now, if between the initial
   * resolution of source_entry and the resolution of existing_entry, the
   * source_entry gets invalidated (possibly due to elapsing of
   * REBUILD_ENTRIES_TIMEOUT seconds), rebuild_dir will update the entry
   * internally in the structures but will free our source_entry. This results
   * in a segfault in the below step.
   *
   * This case was observed when doing the following set of operations:
   *    copy --> rename copied file (but don't refresh nautilus) --> copy renamed file
   *    in same directory */
  source_entry = resolve (self, source, cancellable, NULL, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  if (GDATA_IS_DOCUMENTS_FOLDER (source_entry))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_WOULD_RECURSE,
                        _("Can’t recursively copy directory"));
      goto out;
    }

  id = gdata_entry_get_id (source_entry);
  etag = gdata_entry_get_etag (source_entry);
  summary = gdata_entry_get_summary (source_entry);
  title = gdata_entry_get_title (source_entry);

  source_entry_type = G_OBJECT_TYPE (source_entry);

  /* When a file is copied to the same folder, Google Drive provides a "Make a
   * copy" option which creates a new file and changes its title from "Foobar.pdf"
   * to "Copy of Foobar". But instead here, nautilus does the heavy-lifting and
   * creates the destination file name as "Foobar (copy).pdf".
   *
   * Moreover, just after copy operation, a query_info operation is performed
   * and it needs the ("Foobar (copy).pdf", destination_parent_id) -> Entry mapping
   * in the cache. Hence, we set the new entry's filename conditionally. */
  if (g_strcmp0 (destination_basename, id) == 0)
    dummy_entry_filename = gdata_entry_get_title (source_entry);
  else
    {
      /* Case occurs when trying to overwrite an existing file using destination_basename
       * set to ID2 in `gio copy ID1 ./folder/ID2`. We need to retain the title of the
       * source file instead of setting dummy_entry_filename to destination_basename. */
      if (will_overwrite && existing_entry &&
          g_strcmp0 (gdata_entry_get_id (existing_entry), destination_basename) == 0)
        dummy_entry_filename = title;
      else
        dummy_entry_filename = destination_basename;
    }

  dummy_source_entry = g_object_new (source_entry_type,
                                     "etag", etag,
                                     "id", id,
                                     "summary", summary,
                                     "title", dummy_entry_filename,
                                     NULL);

  /*
   * When a file is copied inside the same parent, we don't add any
   * GDataDocumentsProperty on it. This is done so that whenever the new_entry
   * gets inserted into the cache, it doesn't overwrite the mapping
   * (source_id, source_parent) -> (GDataEntry of source)
   * with (source_id, source_parent) -> (GDataEntry of new_entry).
   *
   * Moreover, in this case, just after the copy operation, nautilus performs a
   * query_info searching for the new title of the file (to check whether file
   * has actually been created). Our new_entry already has its title set to
   * destination_basename, hence we don't need to add any extra property here.
   */
  if (g_strcmp0 (source_parent_id, destination_parent_id) != 0)
    {
      GDataDocumentsProperty *source_id_property, *parent_id_property;

      source_id_property = gdata_documents_property_new (SOURCE_ID_PROPERTY_KEY);
      gdata_documents_property_set_visibility (source_id_property, GDATA_DOCUMENTS_PROPERTY_VISIBILITY_PRIVATE);
      gdata_documents_property_set_value (source_id_property, id);

      parent_id_property = gdata_documents_property_new (PARENT_ID_PROPERTY_KEY);
      gdata_documents_property_set_visibility (parent_id_property, GDATA_DOCUMENTS_PROPERTY_VISIBILITY_PRIVATE);
      gdata_documents_property_set_value (parent_id_property, destination_parent_id);

      gdata_documents_entry_add_documents_property (GDATA_DOCUMENTS_ENTRY (dummy_source_entry), source_id_property);
      gdata_documents_entry_add_documents_property (GDATA_DOCUMENTS_ENTRY (dummy_source_entry), parent_id_property);

      g_clear_object (&source_id_property);
      g_clear_object (&parent_id_property);
    }

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

  entry_path = g_build_path ("/", parent_path, gdata_entry_get_id (GDATA_ENTRY (new_entry)), NULL);
  g_debug ("  new entry path: %s\n", entry_path);

  insert_entry (self, GDATA_ENTRY (new_entry));
  g_hash_table_foreach (self->monitors, emit_create_event, entry_path);

  if (will_overwrite)
    {
      GDataDocumentsEntry *entry_to_remove = NULL;
      guint parent_ids_len;
      GList *parent_ids;
      gchar *existing_entry_path = NULL;

      /* The internal ref count has to be increased before removing the
       * existing_entry since remove_entry_full calls g_object_unref() internally */
      g_object_ref (existing_entry);
      remove_entry (self, existing_entry);

      parent_ids = get_parent_ids (self, existing_entry);
      parent_ids_len = g_list_length (parent_ids);
      if (parent_ids_len > 1 || !is_owner (self, GDATA_ENTRY (existing_entry)))
        {
          /* gdata_documents_service_remove_entry_from_folder () returns the
           * updated entry variable provided as argument with an increased ref.
           * The ref count after the next line shall be 2. */
          entry_to_remove = gdata_documents_service_remove_entry_from_folder (self->service,
                                                                              GDATA_DOCUMENTS_ENTRY (existing_entry),
                                                                              GDATA_DOCUMENTS_FOLDER (destination_parent),
                                                                              cancellable,
                                                                              &error);
        }
      else
        {
          GDataAuthorizationDomain *auth_domain;

          auth_domain = gdata_documents_service_get_primary_authorization_domain ();
          gdata_service_delete_entry (GDATA_SERVICE (self->service), auth_domain, existing_entry, cancellable, &error);
        }

      if (error != NULL)
        {
          sanitize_error (&error);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          g_clear_object (&entry_to_remove);
          goto out;
        }

      existing_entry_path = g_build_path ("/", parent_path, gdata_entry_get_id (existing_entry), NULL);
      g_hash_table_foreach (self->monitors, emit_delete_event, existing_entry_path);

      g_object_unref (existing_entry);
      g_clear_object (&entry_to_remove);
    }

  size = gdata_documents_entry_get_file_size (new_entry);
  g_vfs_job_progress_callback (size, size, job);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_clear_object (&dummy_source_entry);
  g_clear_object (&new_entry);
  g_free (destination_basename);
  g_free (entry_path);
  g_free (parent_path);
  log_dir_entries (self);
  g_debug ("- copy\n");
  g_rec_mutex_unlock (&self->mutex);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_move (GVfsBackend           *_self,
                           GVfsJobMove           *job,
                           const gchar           *source,
                           const gchar           *destination,
                           GFileCopyFlags         flags,
                           GFileProgressCallback  progress_callback,
                           gpointer               progress_callback_data)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GDataAuthorizationDomain *auth_domain;
  GDataEntry *new_entry = NULL;
  GDataEntry *destination_parent;
  GDataEntry *source_parent;
  GDataEntry *existing_entry;
  GDataEntry *source_entry = NULL;
  GDataLink *source_parent_link = NULL;
  GDataLink *destination_parent_link = NULL;
  GError *error = NULL;
  gboolean will_overwrite = FALSE;
  const gchar *source_id;
  const gchar *source_parent_id;
  const gchar *destination_parent_id;
  const gchar *title;
  gchar *source_parent_link_uri = NULL;
  gchar *destination_parent_link_uri = NULL;
  gchar *destination_basename = NULL;
  gchar *entry_path = NULL;
  gchar *parent_path = NULL;
  goffset size;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ move: %s -> %s, %d\n", source, destination, flags);
  log_dir_entries (self);

  if (flags & G_FILE_COPY_BACKUP)
    {
      /* Return G_IO_ERROR_NOT_SUPPORTED instead of
       * G_IO_ERROR_CANT_CREATE_BACKUP to proceed with the GIO
       * fallback copy.
       */
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  source_entry = resolve (self, source, cancellable, NULL, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  source_parent = resolve_dir (self, source, cancellable, NULL, NULL, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  destination_parent = resolve_dir (self, destination, cancellable, &destination_basename, &parent_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  if (source_entry == self->root || source_parent == self->root || source_parent == self->shared_drives_dir ||
      destination_parent == self->root || destination_parent == self->shared_with_me_dir ||
      destination_parent == self->shared_drives_dir)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  source_id = gdata_entry_get_id (source_entry);
  source_parent_id = gdata_entry_get_id (source_parent);
  destination_parent_id = gdata_entry_get_id (destination_parent);
  title = gdata_entry_get_title (source_entry);
  auth_domain = gdata_documents_service_get_primary_authorization_domain ();

  g_debug ("  wants overwrite: %d\n", flags & G_FILE_COPY_OVERWRITE);

  existing_entry = resolve_child (self, destination_parent, destination_basename, cancellable, NULL);
  if (existing_entry != NULL)
    {
      const gchar *existing_entry_id;
      gint64 num_same_title_files;

      existing_entry_id = gdata_entry_get_id (existing_entry);
      num_same_title_files = count_files_in_directory_with_title (self,
                                                                  destination_basename,
                                                                  destination_parent);

      g_debug ("  count of files with title same as destination_basename: %ld\n", num_same_title_files);

      /* We don't support overwrites, so we don't need to care
       * about G_IO_ERROR_IS_DIRECTORY and G_IO_ERROR_WOULD_MERGE.
       *
       * Moreover, we don't need to take care of G_IO_ERROR_WOULD_RECURSE
       * too since changing a Folder's parent on Drive is a matter of
       * simply changing "parents" property. We don't have to do any
       * recursive move. */
      if (flags & G_FILE_COPY_OVERWRITE)
        {
          if (num_same_title_files > 1)
            {
              g_vfs_job_failed_literal (G_VFS_JOB (job),
                                        G_IO_ERROR,
                                        G_IO_ERROR_FAILED,
                                        _("Operation not supported"));
              goto out;
            }

          if (GDATA_IS_DOCUMENTS_FOLDER (existing_entry))
            {
              if (GDATA_IS_DOCUMENTS_FOLDER (source_entry))
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

          /* If the destination-basename matches that of some other entry, we
           * further check if the files have the same ID or not. If the source
           * file as well as the destination file have same ID, we fail the
           * operation. */
          if (g_strcmp0 (existing_entry_id, source_id) == 0)
            {
              g_vfs_job_failed_literal (G_VFS_JOB (job),
                                        G_IO_ERROR,
                                        G_IO_ERROR_FAILED,
                                        _("Operation not supported"));
              goto out;
            }
          else if (g_strcmp0 (existing_entry_id, destination_basename) == 0)
            {
              if (g_strcmp0 (source_parent_id, destination_parent_id) == 0)
                {
                  g_vfs_job_failed_literal (G_VFS_JOB (job),
                                            G_IO_ERROR,
                                            G_IO_ERROR_FAILED,
                                            _("Operation not supported"));
                  goto out;
                }
              else
                will_overwrite = TRUE;
            }
          else if (g_strcmp0 (gdata_entry_get_title (existing_entry), destination_basename) == 0)
            will_overwrite = TRUE;
          else
            {
              /* Neither the ID nor the title of existing_entry matches the destination_basename.
               * This can only happen when destination_basename corresponds to the volatile entry
               * being pointed to by some other file. We simply ignore it and let the move
               * operation to happen. */
            }
        }
      else
        {
          /* We just check for two conditions here (out of a total of 3 we checked above when
           * G_FILE_COPY_OVERWRITE is specified). The first condition "num_same_title_files > 0"
           * checks if we have multiple files with same destination_name in the destination
           * directory, whereas the second condition specifically checks for the case when we have a
           * file whose ID is same as that of source file (i.e. the same file with multiple
           * parents).
           *
           * In the case our destination_name matches the volatile entry of some other file, we
           * simply ignore it and let the move operation take place. */
          if (num_same_title_files > 0 ||
              g_strcmp0 (gdata_entry_get_id (existing_entry), destination_basename) == 0)
            {
              g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file already exists"));
              goto out;
            }
        }
    }

  /* For the cases like `gio move id1 ./folder/`, the operation actually resolves to its longer form
   * `gio move id1 ./folder/id1`. We cater such cases differently and check if another file with the
   * same title as that of file with real ID id1 exists in the destination folder. This check allows
   * us to be as similar to POSIX filesystem operations as possible. This way, we can make
   * title-based move-overwrite operations to work in nautilus. */
  if (g_strcmp0 (destination_basename, source_id) == 0)
    {
      existing_entry = resolve_child (self, destination_parent, title, cancellable, NULL);
      if (existing_entry != NULL)
        {
          gint64 num_same_title_files;

          num_same_title_files = count_files_in_directory_with_title (self,
                                                                      title,
                                                                      destination_parent);

          g_debug ("  count of same title files: %ld\n", num_same_title_files);

          if (flags & G_FILE_COPY_OVERWRITE)
            {
              if (num_same_title_files > 1)
                {
                  /* We return G_IO_ERROR_FAILED below instead of
                   * G_IO_ERROR_NOT_SUPPORTED because _NOT_SUPPORTED implies that the
                   * operation hasn't been implemented yet, and hence GIO takes a
                   * fallback to perform the operation. So, for copy operation with
                   * overwrite, it uses a (replace + read + write) fallback which in
                   * turn overwrites the file even though we don't support overwrites.
                   *
                   * For a concrete case see this discussion:
                   * https://gitlab.gnome.org/GNOME/gvfs/merge_requests/58#note_584083
                   */
                  g_vfs_job_failed_literal (G_VFS_JOB (job),
                                            G_IO_ERROR,
                                            G_IO_ERROR_FAILED,
                                            _("Operation not supported"));
                  goto out;
                }

              if (GDATA_IS_DOCUMENTS_FOLDER (existing_entry))
                {
                  if (GDATA_IS_DOCUMENTS_FOLDER (source_entry))
                    {
                      g_vfs_job_failed (G_VFS_JOB (job),
                                        G_IO_ERROR,
                                        G_IO_ERROR_WOULD_MERGE,
                                        _("Can’t move directory over directory"));
                      goto out;
                    }
                  else
                    {
                      g_vfs_job_failed (G_VFS_JOB (job),
                                        G_IO_ERROR,
                                        G_IO_ERROR_IS_DIRECTORY,
                                        _("Can’t move file over directory"));
                      goto out;
                    }
                }

              will_overwrite = (num_same_title_files == 1);
            }
          else
            {
              /* If the title matches the volatile path of some
               * other file (given by existing_entry) and if the titles are
               * different, move operation should be allowed. In that case, we will
               * be having two different entries (each with different titles), but
               * the stored volatile path (in GDataDocumentsProperty) will be same.
               *
               * This isn't an issue since we've already handled those extra
               * entries in insert_entry_full().
               */
              if (num_same_title_files > 0)
                {
                  g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file already exists"));
                  goto out;
                }
            }
        }
    }

  g_debug ("  will overwrite: %d\n", will_overwrite);

  /* The internal ref count has to be increased before removing the
   * source_entry since remove_entry_full calls g_object_unref() internally */
  g_object_ref (source_entry);
  remove_entry (self, source_entry);

  source_parent_link_uri = g_strconcat (URI_PREFIX, source_parent_id, NULL);
  source_parent_link = gdata_link_new (source_parent_link_uri, GDATA_LINK_PARENT);

  destination_parent_link_uri = g_strconcat (URI_PREFIX, destination_parent_id, NULL);
  destination_parent_link = gdata_link_new (destination_parent_link_uri, GDATA_LINK_PARENT);

  if (!gdata_entry_remove_link (source_entry, source_parent_link))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED, _("Error moving file/folder"));
      g_clear_object (&source_parent_link);
      g_object_unref (source_entry);
      goto out;
    }

  gdata_entry_add_link (source_entry, destination_parent_link);

  /* Handle the cases like `gio move id1 folder1/Foobar` and `gio move id1 Foobar`
   * where Foobar is the new title of the file to be moved.
   *
   * The catch here is that we can't support the case `gio move id1 folder1/id1`
   * because that conflicts with what nautilus does when doing a cut + paste
   * operation. This is a tradeoff between handling the common case vs handling a
   * rare case. If you explicity wish to move the file and update the title of
   * the file to its ID, do a move + rename operation. */
  if (g_strcmp0 (destination_basename, source_id) != 0)
    {
      /* When we're trying to overwrite a file using its ID, the destination_basename will be set to
       * the ID. Instead, we want to have the title to be same as that of source entry. */
      if (!(will_overwrite && existing_entry &&
            g_strcmp0 (gdata_entry_get_id (existing_entry), destination_basename) == 0))
        gdata_entry_set_title (source_entry, destination_basename);
    }

  error = NULL;
  new_entry = gdata_service_update_entry (GDATA_SERVICE (self->service),
                                          auth_domain,
                                          source_entry,
                                          cancellable,
                                          &error);
  if (error != NULL)
    {
      sanitize_error (&error);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      g_object_unref (source_entry);
      goto out;
    }

  if (will_overwrite)
    {
      GDataDocumentsEntry *entry_to_remove = NULL;
      guint parent_ids_len;
      GList *parent_ids;
      gchar *existing_entry_path = NULL;

      /* The internal ref count has to be increased before removing the
       * existing_entry since remove_entry_full calls g_object_unref() internally */
      g_object_ref (existing_entry);
      remove_entry (self, existing_entry);

      parent_ids = get_parent_ids (self, existing_entry);
      parent_ids_len = g_list_length (parent_ids);
      if (parent_ids_len > 1 || !is_owner (self, GDATA_ENTRY (existing_entry)))
        {
          /* gdata_documents_service_remove_entry_from_folder () returns the
           * updated entry variable provided as argument with an increased ref.
           * The ref count after the next line shall be 2. */
          entry_to_remove = gdata_documents_service_remove_entry_from_folder (self->service,
                                                                              GDATA_DOCUMENTS_ENTRY (existing_entry),
                                                                              GDATA_DOCUMENTS_FOLDER (destination_parent),
                                                                              cancellable,
                                                                              &error);
        }
      else
        {
          GDataAuthorizationDomain *auth_domain;

          auth_domain = gdata_documents_service_get_primary_authorization_domain ();
          gdata_service_delete_entry (GDATA_SERVICE (self->service), auth_domain, existing_entry, cancellable, &error);
        }

      if (error != NULL)
        {
          sanitize_error (&error);
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          g_clear_object (&entry_to_remove);
          goto out;
        }

      existing_entry_path = g_build_path ("/", parent_path, gdata_entry_get_id (existing_entry), NULL);
      g_hash_table_foreach (self->monitors, emit_delete_event, existing_entry_path);

      g_object_unref (existing_entry);
      g_clear_object (&entry_to_remove);
    }

  entry_path = g_build_path ("/", parent_path, gdata_entry_get_id (new_entry), NULL);
  g_debug ("  new entry path: %s\n", entry_path);

  insert_entry (self, new_entry);
  g_hash_table_foreach (self->monitors, emit_create_event, entry_path);

  size = gdata_documents_entry_get_file_size (GDATA_DOCUMENTS_ENTRY (new_entry));
  g_vfs_job_progress_callback (size, size, job);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_object_unref (source_entry);

 out:
  g_clear_object (&destination_parent_link);
  g_clear_object (&new_entry);
  g_free (source_parent_link_uri);
  g_free (destination_parent_link_uri);
  g_free (destination_basename);
  g_free (entry_path);
  g_free (parent_path);
  log_dir_entries (self);
  g_debug ("- move\n");
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
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  error = NULL;
  entry = resolve (self, filename, cancellable, &entry_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

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
  GDataEntry *entry;
  GDataEntry *parent;
  GDataDocumentsEntry *new_entry = NULL;
  GError *error;
  gchar *entry_path = NULL;
  GList *parent_ids;
  guint parent_ids_len;
  gchar *id = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ delete: %s\n", filename);

  error = NULL;
  entry = resolve (self, filename, cancellable, &entry_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  /* g_strdup() is necessary to prevent segfault because gdata_entry_get_id() calls g_free() */
  id = g_strdup (gdata_entry_get_id (entry));

  parent = resolve_dir (self, filename, cancellable, NULL, NULL, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  /* The G_IO_ERROR_NOT_EMPTY error is not intentionally returned for folders in
   * Shared with me folder, because the recursive delete would not work, or could
   * really remove the files from the original folder also for the owner... */
  if (GDATA_IS_DOCUMENTS_FOLDER (entry) && parent != self->shared_with_me_dir)
    {
      GHashTableIter iter;
      DirEntriesKey *key;

      if (!is_dir_listing_valid (self, entry))
        {
          rebuild_dir (self, entry, cancellable, &error);
          if (error != NULL)
            {
              g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
              g_error_free (error);
              goto out;
            }
        }

      g_hash_table_iter_init (&iter, self->dir_entries);
      while (g_hash_table_iter_next (&iter, (gpointer *) &key, NULL))
        {
          if (g_strcmp0 (key->parent_id, id) == 0)
            {
              g_vfs_job_failed (G_VFS_JOB (job),
                                G_IO_ERROR,
                                G_IO_ERROR_NOT_EMPTY,
                                _("Directory not empty"));
              goto out;
            }
        }
    }

  g_debug ("  entry path: %s\n", entry_path);

  if (entry == self->root || entry == self->home || entry == self->shared_with_me_dir ||
      entry == self->shared_drives_dir || parent == self->shared_drives_dir)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  /* It has to be removed before the actual call to properly invalidate dir entries. */
  g_object_ref (entry);
  remove_entry (self, entry);

  error = NULL;

  parent_ids = get_parent_ids (self, entry);
  parent_ids_len = g_list_length (parent_ids);

  /* The files in Shared with me folder doesn't have a parent and also we don't
   * have enough permissions to physically delete them. But they can be removed
   * by removal of our permissions... */
  if (parent == self->shared_with_me_dir)
    {
      GDataFeed *acl_feed;

      acl_feed = gdata_access_handler_get_rules (GDATA_ACCESS_HANDLER (entry),
                                                 GDATA_SERVICE (self->service),
                                                 cancellable,
                                                 NULL, NULL, &error);
      if (error == NULL)
        {
          GDataGoaAuthorizer *goa_authorizer;
          GoaAccount *account;
          const gchar *account_identity;
          GDataAuthorizationDomain *auth_domain;
          GList *entries;

          goa_authorizer = GDATA_GOA_AUTHORIZER (gdata_service_get_authorizer (GDATA_SERVICE (self->service)));
          account = goa_object_peek_account (gdata_goa_authorizer_get_goa_object (goa_authorizer));
          account_identity = goa_account_get_identity (account);
          auth_domain = gdata_documents_service_get_primary_authorization_domain ();

          for (entries = gdata_feed_get_entries (acl_feed); entries != NULL; entries = entries->next)
            {
              const gchar *scope_value = NULL;
              GDataAccessRule *rule = GDATA_ACCESS_RULE (entries->data);

              gdata_access_rule_get_scope (rule, NULL, &scope_value);
              if (g_strcmp0 (scope_value, account_identity) == 0)
                gdata_service_delete_entry (GDATA_SERVICE (self->service), auth_domain, GDATA_ENTRY (rule), NULL, &error);
            }

          g_object_unref (acl_feed);
       }
    }
  else if (parent_ids_len > 1 || !is_owner (self, GDATA_ENTRY (entry)))
    {
      /* gdata_documents_service_remove_entry_from_folder () returns the
       * updated entry variable provided as argument with an increased ref.
       * The ref count after the next line shall be 2. */
      new_entry = gdata_documents_service_remove_entry_from_folder (self->service,
                                                                    GDATA_DOCUMENTS_ENTRY (entry),
                                                                    GDATA_DOCUMENTS_FOLDER (parent),
                                                                    cancellable,
                                                                    &error);
    }
  else
    {
      GDataAuthorizationDomain *auth_domain;

      auth_domain = gdata_documents_service_get_primary_authorization_domain ();
      gdata_service_delete_entry (GDATA_SERVICE (self->service), auth_domain, entry, cancellable, &error);
    }

  if (error != NULL)
    {
      sanitize_error (&error);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      g_object_unref (entry);
      goto out;
    }

  /* In case of files owned by somebody else, the new entry is returned
   * even if it had just one parent before the operation. The backend
   * doesn't care about entries without parents (i.e. entries with
   * parent_ids_len = 1), so let's ignore it. */
  if (new_entry && parent_ids_len > 1)
    insert_entry (self, GDATA_ENTRY (new_entry));
  g_hash_table_foreach (self->monitors, emit_delete_event, entry_path);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_object_unref (entry);

 out:
  g_clear_object (&new_entry);
  g_free (entry_path);
  g_free (id);
  g_debug ("- delete\n");
  g_rec_mutex_unlock (&self->mutex);
}

/* ---------------------------------------------------------------------------------------------------- */

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
  GDataEntry *child;
  GError *error;
  GHashTableIter iter;
  char *parent_path;
  char *id = NULL;
  gboolean is_shared_with_me_dir;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ enumerate: %s\n", filename);

  error = NULL;
  entry = resolve (self, filename, cancellable, &parent_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  if (!GDATA_IS_DOCUMENTS_FOLDER (entry))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY,_("The file is not a directory"));
      goto out;
    }

  if (!is_dir_listing_valid (self, entry))
    {
      rebuild_dir (self, entry, cancellable, &error);
      if (error != NULL)
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  /* g_strdup() is necessary to prevent segfault because gdata_entry_get_id() calls g_free() */
  id = g_strdup (gdata_entry_get_id (entry));

  is_shared_with_me_dir = (entry == self->shared_with_me_dir);

  g_hash_table_iter_init (&iter, self->entries);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &child))
    {
      DirEntriesKey *k;
      gchar *child_id;

      /* g_strdup() is necessary to prevent segfault because gdata_entry_get_id() calls g_free() */
      child_id = g_strdup (gdata_entry_get_id (child));

      k = dir_entries_key_new (child_id, id);
      if ((is_shared_with_me_dir && is_shared_with_me (child)) ||
          (!is_shared_with_me_dir && g_hash_table_lookup (self->dir_entries, k) != NULL))
        {
          GFileInfo *info;
          gchar *entry_path;
          gchar *child_filename;

          /* Be sure that we are not matching title of volatile file */
          if (g_strcmp0 (child_id, gdata_entry_get_id (child)) != 0)
            {
              g_debug ("Skipping %s as it is volatile path for %s\n", child_id, gdata_entry_get_id (child));
              g_free (child_id);
              dir_entries_key_free (k);
              continue;
            }

          info = g_file_info_new ();
          entry_path = g_build_path ("/", parent_path, child_id, NULL);
          child_filename = g_build_filename (filename, child_id, NULL);
          build_file_info (self, child, flags, info, matcher, child_filename, entry_path, NULL);
          g_vfs_job_enumerate_add_info (job, info);
          g_object_unref (info);
          g_free (entry_path);
        }

      g_free (child_id);
      dir_entries_key_free (k);
    }

  g_vfs_job_enumerate_done (job);

 out:
  g_debug ("- enumerate\n");
  g_free (parent_path);
  g_free (id);
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
  GDataEntry *same_id_entry;
  GError *error;
  const gchar *summary;
  gchar *entry_path = NULL;
  gchar *basename = NULL;
  gchar *parent_path = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ make_directory: %s\n", filename);

  if (g_strcmp0 (filename, "/") == 0)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  error = NULL;
  parent = resolve_dir (self, filename, cancellable, &basename, &parent_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_debug ("  parent path: %s\n", parent_path);

  summary_entry = g_hash_table_lookup (self->entries, basename);
  if (summary_entry == NULL)
    summary = NULL;
  else
    summary = gdata_entry_get_summary (summary_entry);

  existing_entry = resolve_child (self, parent, basename, cancellable, NULL);
  if (existing_entry != NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file already exists"));
      goto out;
    }

  folder = gdata_documents_folder_new (NULL);

  /* This case is particularly troublesome. Normally, when we call
   * make_directory, the title supplied by the user or client is used to create
   * a file with same title. But strangely, when we're trying to copy a folder
   * over to another directory (i.e. to a different parent), the copy operation
   * would return G_IO_ERROR_WOULD_RECURSE. This means that nautilus will
   * manually create the folders (using make_directory) and copy each file.
   *
   * The major issue here is that nautilus calls make_directory with the title
   * of folder set to ID of the original folder. Suppose, we had a folder "folder1"
   * which needs to be copied. Now, copy operation would return
   * G_IO_ERROR_WOULD_RECURSE (if the parents are different), but nautilus will
   * call make_directory with ID of "folder1" instead of title of "folder1".
   * Furthermore, nautilus calls query_info on the "filename" argument right
   * after it has done make_directory hence we need to create a volatile entry
   * for the folder too.
   *
   * Below fix is hack-ish and will fail if we wish to create a folder
   * whose title equals the ID of any other folder. */
  if ((same_id_entry = g_hash_table_lookup (self->entries, basename)) != NULL)
    {
      GDataDocumentsProperty *source_id_property, *parent_id_property;

      source_id_property = gdata_documents_property_new (SOURCE_ID_PROPERTY_KEY);
      gdata_documents_property_set_visibility (source_id_property, GDATA_DOCUMENTS_PROPERTY_VISIBILITY_PRIVATE);
      gdata_documents_property_set_value (source_id_property, basename);

      parent_id_property = gdata_documents_property_new (PARENT_ID_PROPERTY_KEY);
      gdata_documents_property_set_visibility (parent_id_property, GDATA_DOCUMENTS_PROPERTY_VISIBILITY_PRIVATE);
      gdata_documents_property_set_value (parent_id_property, gdata_entry_get_id (parent));

      gdata_documents_entry_add_documents_property (GDATA_DOCUMENTS_ENTRY (folder), source_id_property);
      gdata_documents_entry_add_documents_property (GDATA_DOCUMENTS_ENTRY (folder), parent_id_property);

      gdata_entry_set_title (GDATA_ENTRY (folder), gdata_entry_get_title (same_id_entry));

      g_clear_object (&source_id_property);
      g_clear_object (&parent_id_property);
    }
  else
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

  entry_path = g_build_path ("/", parent_path, gdata_entry_get_id (GDATA_ENTRY (new_folder)), NULL);
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

static GList *
query_shared_drives (GVfsBackendGoogle  *self,
                     GCancellable       *cancellable,
                     GError            **error)
{
  GDataDocumentsDriveQuery *query;
  GList *shared_drives = NULL;

  query = gdata_documents_drive_query_new (NULL);
  while (TRUE)
    {
      GDataDocumentsFeed *feed;
      GList *entries;

      feed = gdata_documents_service_query_drives (self->service,
                                                   query,
                                                   cancellable,
                                                   NULL,
                                                   NULL,
                                                   error);
      if (feed == NULL)
        break;

      entries = gdata_feed_get_entries (GDATA_FEED (feed));
      if (entries == NULL)
        {
          g_object_unref (feed);
          break;
        }

      shared_drives = g_list_concat (shared_drives,
                                     g_list_copy_deep (entries, (GCopyFunc) g_object_ref, NULL));

      gdata_query_next_page (GDATA_QUERY (query));
      g_object_unref (feed);
    }

  g_clear_object (&query);

  return shared_drives;
}

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

  self->root = GDATA_ENTRY (gdata_documents_folder_new (ROOT_ID));
  gdata_entry_set_title (self->root, self->account_identity);

  error = NULL;
  self->home = gdata_service_query_single_entry (GDATA_SERVICE (self->service),
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
  insert_custom_entry (self, self->home, ROOT_ID);

  self->shared_with_me_dir = GDATA_ENTRY (gdata_documents_folder_new (SHARED_WITH_ME_ID));
  /* Translators: This is the "Shared with me" folder on https://drive.google.com. */
  gdata_entry_set_title (self->shared_with_me_dir, _("Shared with me"));
  insert_custom_entry (self, self->shared_with_me_dir, ROOT_ID);

  self->shared_drives = query_shared_drives (self, cancellable, &error);
  if (error != NULL)
    {
      sanitize_error (&error);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  if (self->shared_drives)
    {
      self->shared_drives_dir = GDATA_ENTRY (gdata_documents_folder_new (SHARED_DRIVES_ID));
      /* Translators: This is the "Shared drives" folder on https://drive.google.com. */
      gdata_entry_set_title (self->shared_drives_dir, _("Shared drives"));
      insert_custom_entry (self, self->shared_drives_dir, ROOT_ID);
    }

  /* TODO: Make it work with GOA volume monitor resp. shadow mounts. */
  g_vfs_backend_set_default_location (_self, gdata_entry_get_id (self->home));

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
  gchar *parent_path = NULL;
  gchar *local_file_title = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ push: %s -> %s, %d\n", local_path, destination, flags);

  if (remove_source && (flags & G_FILE_COPY_NO_FALLBACK_FOR_MOVE))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  if (flags & G_FILE_COPY_BACKUP)
    {
      /* Return G_IO_ERROR_NOT_SUPPORTED instead of
       * G_IO_ERROR_CANT_CREATE_BACKUP to proceed with the GIO
       * fallback copy.
       */
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  local_file = g_file_new_for_path (local_path);

  error = NULL;
  info = g_file_query_info (local_file,
                            G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE","
                            G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME","
                            G_FILE_ATTRIBUTE_STANDARD_TYPE","
                            G_FILE_ATTRIBUTE_STANDARD_SIZE,
                            G_FILE_QUERY_INFO_NONE,
                            cancellable,
                            &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  local_file_title = g_file_info_get_attribute_as_string (info, G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME);

  error = NULL;
  destination_parent = resolve_dir (self, destination, cancellable, &destination_basename, &parent_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  if (destination_parent == self->root || destination_parent == self->shared_with_me_dir || destination_parent == self->shared_drives_dir)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  existing_entry = resolve_child (self, destination_parent, destination_basename, cancellable, NULL);
  if (existing_entry != NULL)
    {
      const gchar *existing_entry_id;
      gint64 num_same_title_files;

      existing_entry_id = gdata_entry_get_id (existing_entry);
      num_same_title_files = count_files_in_directory_with_title (self,
                                                                  destination_basename,
                                                                  destination_parent);

      g_debug ("  count of files with title same as destination_basename: %ld\n", num_same_title_files);

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

          if (g_strcmp0 (local_file_title, existing_entry_id) == 0)
            {
              /* This corresponds to the operation when a local file has its title set to the
               * real ID of some file, and that the local file is being pushed with the same title.
               * We disallow such operation. */
              g_vfs_job_failed_literal (G_VFS_JOB (job),
                                        G_IO_ERROR,
                                        G_IO_ERROR_FAILED,
                                        _("Operation not supported"));
              goto out;
            }

          if (num_same_title_files > 1)
            {
              g_vfs_job_failed_literal (G_VFS_JOB (job),
                                        G_IO_ERROR,
                                        G_IO_ERROR_FAILED,
                                        _("Operation not supported"));
              goto out;
            }

          if (num_same_title_files == 1 || g_strcmp0 (destination_basename, existing_entry_id) == 0)
            needs_overwrite = TRUE;
        }
      else
        {
          if (num_same_title_files > 0 ||
              g_strcmp0 (destination_basename, existing_entry_id) == 0 ||
              g_strcmp0 (local_file_title, existing_entry_id) == 0)
            {

              g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file already exists"));
              goto out;
            }
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
      title = local_file_title;

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
  gvfs_output_stream_splice (G_OUTPUT_STREAM (ostream),
                             G_INPUT_STREAM (istream),
                             G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                             g_file_info_get_size (info),
                             progress_callback,
                             progress_callback_data,
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

  entry_path = g_build_path ("/", parent_path, gdata_entry_get_id (GDATA_ENTRY (new_document)), NULL);
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

  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_clear_object (&document);
  g_clear_object (&info);
  g_clear_object (&istream);
  g_clear_object (&local_file);
  g_clear_object (&new_document);
  g_clear_object (&ostream);
  g_free (destination_basename);
  g_free (local_file_title);
  g_free (entry_path);
  g_free (parent_path);
  g_debug ("- push\n");
  g_rec_mutex_unlock (&self->mutex);
}

/* ---------------------------------------------------------------------------------------------------- */

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
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_IF_ALWAYS);

  if (g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_FILESYSTEM_FREE) ||
      g_file_attribute_matcher_matches (matcher, G_FILE_ATTRIBUTE_FILESYSTEM_USED))
    {
      GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);
      GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
      gdata_documents_service_get_metadata_async (self->service, cancellable, fs_info_cb, job);
      return TRUE;
    }

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
  gchar *entry_path = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ query_info: %s, %d\n", filename, flags);
  log_dir_entries (self);

  error = NULL;
  entry = resolve (self, filename, cancellable, &entry_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_debug ("  entry path: %s\n", entry_path);

  error = NULL;
  build_file_info (self, entry, flags, info, matcher, filename, entry_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_free (entry_path);
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
  const gchar *filename;
  gchar *entry_path = NULL;

  g_debug ("+ query_info_on_read: %p\n", handle);

  entry = g_object_get_data (G_OBJECT (stream), "g-vfs-backend-google-entry");
  filename = g_object_get_data (G_OBJECT (stream), "g-vfs-backend-google-filename");
  entry_path = g_object_get_data (G_OBJECT (stream), "g-vfs-backend-google-entry-path");

  g_debug ("  entry path: %s\n", entry_path);

  error = NULL;
  build_file_info (self,
                   entry,
                   G_FILE_QUERY_INFO_NONE,
                   info,
                   matcher,
                   filename,
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

  g_debug ("+ query_info_on_write: %p\n", handle);
  g_debug ("  entry path: %s\n", wh->entry_path);

  error = NULL;
  build_file_info (self,
                   wh->document,
                   G_FILE_QUERY_INFO_NONE,
                   info,
                   matcher,
                   wh->filename,
                   wh->entry_path,
                   &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
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
  GDataAuthorizationDomain *auth_domain;
  const gchar *uri;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ open_for_read: %s\n", filename);

  error = NULL;
  entry = resolve (self, filename, cancellable, &entry_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

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

  if (is_native_file (entry))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE, _("File is not a regular file"));
      goto out;
    }

  auth_domain = gdata_documents_service_get_primary_authorization_domain ();
  uri = gdata_entry_get_content_uri (entry);
  stream = gdata_download_stream_new (GDATA_SERVICE (self->service), auth_domain, uri, cancellable);
  if (stream == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED, _("Error getting data from file"));
      goto out;
    }

  g_object_set_data_full (G_OBJECT (stream), "g-vfs-backend-google-entry", g_object_ref (entry), g_object_unref);
  g_object_set_data_full (G_OBJECT (stream), "g-vfs-backend-google-filename", g_strdup (filename), g_free);
  g_object_set_data_full (G_OBJECT (stream), "g-vfs-backend-google-entry-path", g_strdup (entry_path), g_free);
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
  GDataEntry *parent;
  GError *error;
  gchar *entry_path = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ set_display_name: %s, %s\n", filename, display_name);

  error = NULL;
  entry = resolve (self, filename, cancellable, &entry_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_debug ("  entry path: %s\n", entry_path);

  parent = resolve_dir (self, filename, cancellable, NULL, NULL, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  if (entry == self->root || entry == self->home || entry == self->shared_with_me_dir || parent == self->shared_drives_dir)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  /* The internal ref count has to be increased before removing the entry since
   * remove_entry_full calls g_object_unref(). */
  g_object_ref (entry);
  remove_entry (self, entry);

  gdata_entry_set_title (entry, display_name);
  auth_domain = gdata_documents_service_get_primary_authorization_domain ();

  error = NULL;
  new_entry = gdata_service_update_entry (GDATA_SERVICE (self->service), auth_domain, entry, cancellable, &error);
  if (error != NULL)
    {
      sanitize_error (&error);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      g_object_unref (entry);
      goto out;
    }

  insert_entry (self, new_entry);
  g_hash_table_foreach (self->monitors, emit_attribute_changed_event, entry_path);
  g_vfs_job_set_display_name_set_new_path (job, entry_path);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  g_object_unref (entry);

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
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  error = NULL;
  parent = resolve_dir (self, filename, cancellable, &basename, &parent_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_debug ("  parent path: %s\n", parent_path);

  if (parent == self->root || parent == self->shared_with_me_dir || parent == self->shared_drives_dir)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  existing_entry = resolve_child (self, parent, basename, cancellable, NULL);
  if (existing_entry != NULL)
    {
      if (flags & G_FILE_CREATE_REPLACE_DESTINATION)
        {
          g_vfs_job_failed_literal (G_VFS_JOB (job),
                                    G_IO_ERROR,
                                    G_IO_ERROR_NOT_SUPPORTED,
                                    _("Operation not supported"));
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

  entry_path = g_build_path ("/", parent_path, gdata_entry_get_id (GDATA_ENTRY (new_document)), NULL);
  g_debug ("  new entry path: %s\n", entry_path);

  insert_entry (self, GDATA_ENTRY (new_document));
  g_hash_table_foreach (self->monitors, emit_create_event, entry_path);

  handle = write_handle_new (GDATA_ENTRY (new_document), NULL, filename, entry_path);
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
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  error = NULL;
  parent = resolve_dir (self, filename, cancellable, &basename, &parent_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_debug ("  parent path: %s\n", parent_path);

  if (parent == self->root || parent == self->shared_with_me_dir || parent == self->shared_drives_dir)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  existing_entry = resolve_child (self, parent, basename, cancellable, NULL);
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

      entry_path = g_build_path ("/", parent_path, gdata_entry_get_id (existing_entry), NULL);
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

      handle = write_handle_new (NULL, stream, filename, entry_path);
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

      entry_path = g_build_path ("/", parent_path, gdata_entry_get_id (GDATA_ENTRY (new_document)), NULL);
      g_debug ("  new entry path: %s\n", entry_path);

      insert_entry (self, GDATA_ENTRY (new_document));
      g_hash_table_foreach (self->monitors, emit_create_event, entry_path);

      handle = write_handle_new (GDATA_ENTRY (new_document), NULL, filename, entry_path);
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
  g_debug ("  entry path: %s\n", wh->entry_path);

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

  g_hash_table_foreach (self->monitors, emit_changed_event, wh->entry_path);
  g_vfs_job_write_set_written_size (job, (gsize) nwrite);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
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

  g_debug ("  new entry path: %s\n", wh->entry_path);

  remove_entry (self, wh->document);
  insert_entry (self, GDATA_ENTRY (new_document));
  g_hash_table_foreach (self->monitors, emit_changes_done_event, wh->entry_path);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_clear_object (&new_document);
  write_handle_free (wh);
  g_debug ("- close_write\n");
}

/* ---------------------------------------------------------------------------------------------------- */

static void
g_vfs_backend_google_dispose (GObject *_self)
{
  GVfsBackendGoogle *self = G_VFS_BACKEND_GOOGLE (_self);

  if (self->dir_collisions != NULL)
    {
      g_list_free_full (self->dir_collisions, g_object_unref);
      self->dir_collisions = NULL;
    }

  g_clear_object (&self->service);
  g_clear_object (&self->root);
  g_clear_object (&self->home);
  g_clear_object (&self->shared_with_me_dir);
  g_clear_object (&self->shared_drives_dir);
  g_clear_list (&self->shared_drives, g_object_unref);
  g_clear_object (&self->client);
  g_clear_pointer (&self->entries, g_hash_table_unref);
  g_clear_pointer (&self->dir_entries, g_hash_table_unref);
  g_clear_pointer (&self->dir_timestamps, g_hash_table_unref);

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
  backend_class->move = g_vfs_backend_google_move;
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
  self->dir_timestamps = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->monitors = g_hash_table_new (NULL, NULL);
  g_rec_mutex_init (&self->mutex);
}
