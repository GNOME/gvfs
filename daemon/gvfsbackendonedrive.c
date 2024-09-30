/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2019 Vilém Hořínek <vilem.horinek@hotmail.com>
 * Copyright (C) 2022-2023 Jan-Michael Brummer <jan-michael.brummer1@volkswagen.de>
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
 */

#include <config.h>

#include <libsoup/soup.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "gvfsbackendonedrive.h"
#include "gvfsicon.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobcloseread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobseekwrite.h"
#include "gvfsmonitor.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"

#define GOA_API_IS_SUBJECT_TO_CHANGE
#include <goa/goa.h>
#include <msg.h>

#include <stdio.h>

struct _GVfsBackendOnedrive
{
  GVfsBackend parent;

  GoaClient *client;
  char *account_identity;

  MsgDriveService *service;

  /* Special drive items */
  MsgDriveItem *root;
  MsgDriveItem *home;
  MsgDriveItem *shared_with_me_dir;

  /* Internal structures */
  GHashTable *items;
  GHashTable *dir_items;
  GHashTable *dir_timestamps;
  GHashTable *monitors;
  GRecMutex mutex;
};

struct _GVfsBackendOnedriveClass
{
  GVfsBackendClass parent_class;
};

G_DEFINE_TYPE (GVfsBackendOnedrive, g_vfs_backend_onedrive, G_VFS_TYPE_BACKEND)

#define ROOT_ID "GVfsRoot"
#define SHARED_WITH_ME_ID "GVfsSharedWithMe"
#define REBUILD_ENTRIES_TIMEOUT 60 /* s */

typedef struct
{
  gchar *name_or_id;
  gchar *parent_id;
} DirItemsKey;

typedef struct
{
  MsgDriveItem *item;
  GOutputStream *stream;
  char *item_path;
} WriteHandle;

typedef struct
{
  MsgDriveItem *item;
  GInputStream *stream;
  char *item_path;
} ReadHandle;

static MsgDriveItem *
resolve_dir (GVfsBackendOnedrive  *self,
             const gchar          *filename,
             GCancellable         *cancellable,
             gchar               **out_basename,
             gchar               **out_path,
             GError              **error);

static DirItemsKey *
dir_items_key_new (const gchar *name_or_id,
                   const gchar *parent_id)
{
  DirItemsKey *k;

  k = g_slice_new0 (DirItemsKey);
  k->name_or_id = g_strdup (name_or_id);
  k->parent_id = g_strdup (parent_id);
  return k;
}

static void
dir_items_key_free (gpointer data)
{
  DirItemsKey *k = (DirItemsKey *) data;

  if (k == NULL)
    return;

  g_free (k->name_or_id);
  g_free (k->parent_id);
  g_slice_free (DirItemsKey, k);
}

static guint
items_in_folder_hash (gconstpointer key)
{
  DirItemsKey *k = (DirItemsKey *) key;
  guint hash1;
  guint hash2;

  hash1 = g_str_hash (k->name_or_id);
  hash2 = g_str_hash (k->parent_id);
  return hash1 ^ hash2;
}

static gboolean
items_in_folder_equal (gconstpointer a, gconstpointer b)
{
  DirItemsKey *k_a = (DirItemsKey *) a;
  DirItemsKey *k_b = (DirItemsKey *) b;

  if (g_strcmp0 (k_a->name_or_id, k_b->name_or_id) == 0 &&
      g_strcmp0 (k_a->parent_id, k_b->parent_id) == 0)
    return TRUE;

  return FALSE;
}

static char *
get_full_item_id (MsgDriveItem *item)
{
  const char *drive_id = msg_drive_item_get_drive_id (item);

  if (!drive_id)
    drive_id = "";

  return g_strconcat (drive_id, msg_drive_item_get_id (item), NULL);
}

static char *
get_full_parent_id (MsgDriveItem *item)
{
  const char *drive_id = msg_drive_item_get_drive_id (item);

  if (!drive_id)
    drive_id = "";

  return g_strconcat (drive_id, msg_drive_item_get_parent_id (item), NULL);
}

static void
log_dir_items (GVfsBackendOnedrive *self)
{
  GHashTableIter iter;
  MsgDriveItem *item;
  DirItemsKey *key;

  if (!g_getenv ("GVFS_ONEDRIVE_DEBUG"))
    return;

  g_hash_table_iter_init (&iter, self->dir_items);
  while (g_hash_table_iter_next (&iter, (gpointer *) &key, (gpointer *) &item))
    {
      g_autofree char *id = get_full_item_id (MSG_DRIVE_ITEM (item));

      g_debug ("  Real ID = %s, (%s, %s) -> %p, %d\n",
               id,
               key->name_or_id,
               key->parent_id,
               item,
               ((GObject *) item)->ref_count);
    }
}

static WriteHandle *
write_handle_new (MsgDriveItem  *item,
                  GOutputStream *stream,
                  const char    *filename,
                  const char    *item_path)
{
  WriteHandle *handle;

  handle = g_slice_new0 (WriteHandle);

  if (item != NULL)
    handle->item = g_object_ref (item);

  if (stream != NULL)
    {
      handle->stream = g_object_ref (stream);
    }

  handle->item_path = g_strdup (item_path);

  return handle;
}

static void
write_handle_free (gpointer data)
{
  WriteHandle *handle = (WriteHandle *) data;

  if (handle == NULL)
    return;

  g_clear_object (&handle->item);
  g_clear_object (&handle->stream);
  g_free (handle->item_path);
  g_slice_free (WriteHandle, handle);
}

static ReadHandle *
read_handle_new (MsgDriveItem *item,
                 GInputStream *stream,
                 const char   *item_path)
{
  ReadHandle *handle;

  handle = g_slice_new0 (ReadHandle);

  if (item != NULL)
    handle->item = g_object_ref (item);

  if (stream != NULL)
    {
      handle->stream = g_object_ref (stream);
    }

  handle->item_path = g_strdup (item_path);

  return handle;
}

static void
read_handle_free (gpointer data)
{
  ReadHandle *handle = (ReadHandle *) data;

  if (handle == NULL)
    return;

  g_clear_object (&handle->item);
  g_clear_object (&handle->stream);
  g_free (handle->item_path);
  g_slice_free (ReadHandle, handle);
}

static void
emit_event_internal (GVfsMonitor       *monitor,
                     const char        *item_path,
                     GFileMonitorEvent  event)
{
  const char *monitored_path;
  g_autofree char *parent_path = NULL;

  if (item_path == NULL)
    return;

  monitored_path = g_object_get_data (G_OBJECT (monitor), "g-vfs-backend-onedrive-path");
  parent_path = g_path_get_dirname (item_path);

  if (g_strcmp0 (parent_path, monitored_path) == 0)
    {
      g_debug ("  emit event %d on parent directory for %s\n", event, item_path);
      g_vfs_monitor_emit_event (monitor, event, item_path, NULL);
    }
  else if (g_strcmp0 (item_path, monitored_path) == 0)
    {
      g_debug ("  emit event %d on file %s\n", event, item_path);
      g_vfs_monitor_emit_event (monitor, event, item_path, NULL);
    }
}

static void
emit_renamed_event (gpointer monitor,
                    gpointer unused,
                    gpointer item_path)
{
  emit_event_internal (G_VFS_MONITOR (monitor), item_path, G_FILE_MONITOR_EVENT_RENAMED);
}

static void
emit_changed_event (gpointer monitor,
                    gpointer unused,
                    gpointer item_path)
{
  emit_event_internal (G_VFS_MONITOR (monitor), item_path, G_FILE_MONITOR_EVENT_CHANGED);
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
                   gpointer item_path)
{
  emit_event_internal (G_VFS_MONITOR (monitor), item_path, G_FILE_MONITOR_EVENT_CREATED);
}

static void
emit_delete_event (gpointer monitor,
                   gpointer unused,
                   gpointer item_path)
{
  emit_event_internal (G_VFS_MONITOR (monitor), item_path, G_FILE_MONITOR_EVENT_DELETED);
}

static gboolean
insert_item (GVfsBackendOnedrive *self,
             MsgDriveItem        *parent,
             MsgDriveItem        *item)
{
  DirItemsKey *k;
  g_autofree char *id = NULL;
  g_autofree char *parent_id = NULL;
  const char *name;
  gint64 *timestamp;

  /* Set timestamp */
  timestamp = g_new (gint64, 1);
  *timestamp = g_get_real_time ();
  g_object_set_data_full (G_OBJECT (item), "timestamp", timestamp, g_free);

  /* Add item to items hash */
  id = get_full_item_id (item);
  g_hash_table_insert (self->items, g_strdup (id), g_object_ref (item));

  /* Add item to parent dir item hash */
  if (parent)
    parent_id = get_full_item_id (parent);
  else
    parent_id = get_full_parent_id (item);

  k = dir_items_key_new (id, parent_id);
  g_hash_table_insert (self->dir_items, k, g_object_ref (item));
  g_debug ("  insert_item: Inserted real     (%s, %s) -> %p\n", id, parent_id, item);

  name = msg_drive_item_get_name (item);
  k = dir_items_key_new (name, parent_id);
  g_hash_table_insert (self->dir_items, k, g_object_ref (item));
  g_debug ("  insert_item: Inserted name    (%s, %s) -> %p\n", name, parent_id, item);

  return TRUE;
}

static void
insert_custom_item (GVfsBackendOnedrive *self,
                    MsgDriveItem        *item,
                    const char          *parent_id)
{
  DirItemsKey *k;
  g_autofree char *id = get_full_item_id (item);
  const char *name;

  name = msg_drive_item_get_name (item);

  g_hash_table_insert (self->items, g_strdup (id), g_object_ref (item));

  k = dir_items_key_new (id, parent_id);
  g_hash_table_insert (self->dir_items, k, g_object_ref (item));
  g_debug ("  insert_custom_item: Inserted real     (%s, %s) -> %p\n", id, parent_id, item);

  k = dir_items_key_new (name, parent_id);
  g_hash_table_insert (self->dir_items, k, g_object_ref (item));
  g_debug ("  insert_custom_item: Inserted name    (%s, %s) -> %p\n", name, parent_id, item);
}

static gboolean
is_shared_with_me (MsgDriveItem *item)
{
  return msg_drive_item_is_shared (item);
}

static void
remove_item (GVfsBackendOnedrive *self,
             MsgDriveItem        *parent,
             MsgDriveItem        *item)
{
  DirItemsKey *k;
  g_autofree char *id = NULL;
  const char *parent_id = NULL;
  const char *name;

  id = get_full_item_id (item);
  name = msg_drive_item_get_name (item);

  /* Remove item from hash */
  g_hash_table_remove (self->items, id);

  if (is_shared_with_me (item))
    g_hash_table_remove (self->dir_timestamps, SHARED_WITH_ME_ID);

  parent_id = msg_drive_item_get_id (parent);
  g_hash_table_remove (self->dir_timestamps, parent_id);

  k = dir_items_key_new (id, parent_id);
  if (g_hash_table_remove (self->dir_items, k))
    g_debug ("  remove_item: Removed real     (%s, %s) -> %p\n", id, parent_id, item);
  dir_items_key_free (k);

  k = dir_items_key_new (name, parent_id);
  if (g_hash_table_remove (self->dir_items, k))
    g_debug ("  remove_item: Removed name     (%s, %s) -> %p\n", name, parent_id, item);
  dir_items_key_free (k);
}

static void
remove_dir (GVfsBackendOnedrive *self,
            MsgDriveItem        *parent)
{
  GHashTableIter iter;
  MsgDriveItem *item;
  g_autofree char *parent_id = NULL;

  parent_id = get_full_item_id (parent);

  g_hash_table_remove (self->dir_timestamps, parent_id);

  g_hash_table_iter_init (&iter, self->items);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &item))
    {
      DirItemsKey *k;
      g_autofree char *id = NULL;

      id = get_full_item_id (item);
      k = dir_items_key_new (id, parent_id);
      if (g_hash_table_lookup (self->dir_items, k) != NULL)
        {
          g_object_ref (item);
          g_hash_table_iter_remove (&iter);
          remove_item (self, parent, item);
          g_object_unref (item);
        }

      dir_items_key_free (k);
    }
}

static gboolean
is_item_valid (MsgDriveItem *item)
{
  gint64 *timestamp;

  timestamp = g_object_get_data (G_OBJECT (item), "timestamp");
  if (timestamp == NULL)
    return TRUE;

  return (g_get_real_time () - *timestamp < REBUILD_ENTRIES_TIMEOUT * G_USEC_PER_SEC);
}

static gboolean
is_dir_listing_valid (GVfsBackendOnedrive *self,
                      MsgDriveItem        *parent)
{
  gint64 *timestamp;
  g_autofree char *id = NULL;

  if (parent == self->root)
    return TRUE;

  id = get_full_item_id (parent);
  timestamp = g_hash_table_lookup (self->dir_timestamps, id);
  if (timestamp != NULL)
    return (g_get_real_time () - *timestamp < REBUILD_ENTRIES_TIMEOUT * G_USEC_PER_SEC);

  return FALSE;
}

static void
rebuild_shared_with_me_dir (GVfsBackendOnedrive  *self,
                            GCancellable         *cancellable,
                            GError              **error)
{
  GList *items;
  GError *local_error = NULL;
  gint64 *timestamp;

  items = msg_drive_service_get_shared_with_me (self->service, cancellable, &local_error);

  remove_dir (self, self->shared_with_me_dir);

  timestamp = g_new (gint64, 1);
  *timestamp = g_get_real_time ();
  g_hash_table_insert (self->dir_timestamps, g_strdup (SHARED_WITH_ME_ID), timestamp);

  for (GList *l = items; l != NULL; l = l->next)
    {
      MsgDriveItem *item = MSG_DRIVE_ITEM (l->data);

      msg_drive_item_set_parent_id (item, SHARED_WITH_ME_ID);
      insert_custom_item (self, item, SHARED_WITH_ME_ID);
    }

  g_clear_list (&items, g_object_unref);
}

static void
rebuild_dir (GVfsBackendOnedrive  *self,
             MsgDriveItem         *parent,
             GCancellable         *cancellable,
             GError              **error)
{
  GList *items;
  GError *local_error = NULL;
  gint64 *timestamp;

  if (parent == self->shared_with_me_dir)
    {
      rebuild_shared_with_me_dir (self, cancellable, error);
      return;
    }

  items = msg_drive_service_list_children (self->service, parent, cancellable, &local_error);
  if (local_error != NULL)
    {
      g_debug (" error: %s\n", local_error->message);
      g_propagate_error (error, local_error);
      return;
    }

  remove_dir (self, parent);

  timestamp = g_new (gint64, 1);
  *timestamp = g_get_real_time ();
  g_hash_table_insert (self->dir_timestamps, get_full_item_id (parent), timestamp);

  for (GList *l = items; l != NULL; l = l->next)
    {
      MsgDriveItem *item = MSG_DRIVE_ITEM (l->data);

      insert_item (self, parent, item);
    }

  g_clear_list (&items, g_object_unref);
}

static MsgDriveItem *
resolve_child (GVfsBackendOnedrive  *self,
               MsgDriveItem         *parent,
               const gchar          *basename,
               GCancellable         *cancellable,
               GError              **error)
{
  GError *local_error = NULL;
  DirItemsKey *k;
  MsgDriveItem *item;
  g_autofree char *parent_id = NULL;
  gboolean is_shared_with_me_dir = (parent == self->shared_with_me_dir);

  parent_id = get_full_item_id (parent);
  k = dir_items_key_new (basename, parent_id);

  item = g_hash_table_lookup (self->dir_items, k);

  if ((item == NULL && !is_dir_listing_valid (self, parent)) ||
      (item != NULL && !is_item_valid (item)))
    {
      rebuild_dir (self, parent, cancellable, &local_error);
      if (local_error != NULL)
        {
          g_propagate_error (error, local_error);
          goto out;
        }

        if (is_shared_with_me_dir)
          item = g_hash_table_lookup (self->items, basename);
        else
          item = g_hash_table_lookup (self->dir_items, k);
    }

  if (item == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("No such file or directory"));
      goto out;
    }

out:
  dir_items_key_free (k);

  return item;
}

static MsgDriveItem *
resolve (GVfsBackendOnedrive  *self,
         const char           *filename,
         GCancellable         *cancellable,
         char                **out_path,
         GError              **error)
{
  MsgDriveItem *parent;
  MsgDriveItem *ret_val = NULL;
  GError *local_error = NULL;
  g_autofree char *basename = NULL;

  g_assert (filename && filename[0] == '/');

  if (g_strcmp0 (filename, "/") == 0)
    {
      ret_val = self->root;

      if (out_path != NULL)
        *out_path = g_strdup ("/");

      goto out;
    }

  parent = resolve_dir (self, filename, cancellable, &basename, out_path, &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, local_error);
      goto out;
    }

  ret_val = resolve_child (self, parent, basename, cancellable, &local_error);
  if (ret_val == NULL)
    {
      g_propagate_error (error, local_error);
      goto out;
    }

  if (out_path != NULL)
    {
      char *tmp = g_build_path ("/", *out_path, msg_drive_item_get_name (ret_val), NULL);
      g_free (*out_path);
      *out_path = tmp;
    }

 out:
  return ret_val;
}

static MsgDriveItem *
resolve_dir (GVfsBackendOnedrive  *self,
             const char           *filename,
             GCancellable         *cancellable,
             char                **out_basename,
             char                **out_path,
             GError              **error)
{
  MsgDriveItem *parent;
  MsgDriveItem *ret_val = NULL;
  GError *local_error = NULL;
  g_autofree char *parent_path = NULL;

  parent_path = g_path_get_dirname (filename);

  parent = resolve (self, parent_path, cancellable, out_path, &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, local_error);
      goto out;
    }

    if (!MSG_IS_DRIVE_ITEM_FOLDER (parent))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, _("The file is not a directory"));
      goto out;
    }

  if (out_basename != NULL)
    {
      *out_basename = g_path_get_basename (filename);
    }

  ret_val = parent;

out:
  return ret_val;
}


static void
build_file_info (GVfsBackendOnedrive    *self,
                 MsgDriveItem           *item,
                 GFileQueryInfoFlags     flags,
                 GFileInfo              *info,
                 GFileAttributeMatcher  *matcher,
                 GError                **error)
{
  g_autofree char *mime_type = NULL;
  GFileType file_type;
  g_autofree char *id = NULL;
  const char *name;
  const char *user;
  const char *etag;
  gboolean is_folder = FALSE;
  gboolean is_root = FALSE;
  gboolean is_home = (item == self->home);
  gboolean is_shared_with_me = (item == self->shared_with_me_dir);
  gboolean uncertain_content_type = FALSE;

  if (MSG_IS_DRIVE_ITEM_FOLDER (item))
    is_folder = TRUE;

  if (item == self->root)
    is_root = TRUE;

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, !is_root && !is_home && !is_shared_with_me);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, is_folder);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_VOLATILE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, !is_root && !is_home && !is_shared_with_me);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, !is_root && !is_shared_with_me);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
  g_file_info_set_is_symlink (info, FALSE);

  if (is_folder || is_shared_with_me)
    {
      mime_type = g_strdup ("inode/directory");
      file_type = G_FILE_TYPE_DIRECTORY;
    }
  else
    {
      goffset size;

      mime_type = g_strdup (msg_drive_item_file_get_mime_type (MSG_DRIVE_ITEM_FILE (item)));
      if (mime_type == NULL || g_str_equal (mime_type, "application/octet-stream"))
        {
          g_free (mime_type);
          mime_type = g_content_type_guess (msg_drive_item_get_name (item), NULL, 0, &uncertain_content_type);
        }

      file_type = G_FILE_TYPE_REGULAR;

      size = msg_drive_item_get_size (item);
      g_file_info_set_size (info, size);
      g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_SIZE, (guint64) size);
    }

  if (mime_type != NULL)
    {
      g_autoptr (GIcon) icon = NULL;
      g_autoptr (GIcon) symbolic_icon = NULL;

      if (!uncertain_content_type)
        g_file_info_set_content_type (info, mime_type);

      g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE, mime_type);

      if (is_home)
        {
          icon = g_themed_icon_new_with_default_fallbacks ("user-home");
          symbolic_icon = g_themed_icon_new_with_default_fallbacks ("user-home-symbolic");
        }
      else if (is_shared_with_me)
        {
          icon = g_themed_icon_new_with_default_fallbacks ("folder-publicshare");
          symbolic_icon = g_themed_icon_new_with_default_fallbacks ("folder-publicshare-symbolic");
        }
      else
        {
          icon = g_content_type_get_icon (mime_type);
          symbolic_icon = g_content_type_get_symbolic_icon (mime_type);
        }

      g_file_info_set_icon (info, icon);
      g_file_info_set_symbolic_icon (info, symbolic_icon);
    }

  if (msg_drive_item_is_shared (item))
    {
      g_autoptr (GStrvBuilder) emblems_builder = g_strv_builder_new ();
      g_auto (GStrv) emblems = NULL;

      g_strv_builder_add (emblems_builder, "folder-remote");
      emblems = g_strv_builder_end (emblems_builder);
      g_file_info_set_attribute_stringv (info, "metadata::emblems", emblems);
    }

  g_file_info_set_file_type (info, file_type);

  id = get_full_item_id (item);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ID_FILE, id);

  if (is_root)
    name = "/";
  else
    name = msg_drive_item_get_name (item);

  g_file_info_set_name (info, name);

  g_file_info_set_display_name (info, name);
  g_file_info_set_edit_name (info, name);

  if (is_root || is_home || is_shared_with_me)
    return;

  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_CREATED, msg_drive_item_get_created (item));
  g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED, msg_drive_item_get_modified (item));

  user = msg_drive_item_get_user (item);
  if (user)
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_OWNER_USER_REAL, user);

  etag = msg_drive_item_get_etag (item);
  if (etag)
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE, etag);

  if (!is_folder)
    {
      const char *thumbnail_uri;

      thumbnail_uri = msg_drive_item_file_get_thumbnail_uri (MSG_DRIVE_ITEM_FILE (item));
      if (thumbnail_uri != NULL && thumbnail_uri[0] != '\0')
        {
          g_autoptr (GIcon) preview = NULL;
          GMountSpec *spec;

          spec = g_vfs_backend_get_mount_spec (G_VFS_BACKEND (self));
          preview = g_vfs_icon_new (spec, thumbnail_uri);
          g_file_info_set_attribute_object (info, G_FILE_ATTRIBUTE_PREVIEW_ICON, G_OBJECT (preview));
        }
    }
}

static void
remove_monitor_weak_ref (gpointer monitor,
                         gpointer unused,
                         gpointer monitors)
{
  g_object_weak_unref (G_OBJECT (monitor), (GWeakNotify) g_hash_table_remove, monitors);
}

static gboolean
g_vfs_backend_onedrive_try_create_dir_monitor (GVfsBackend          *_self,
                                               GVfsJobCreateMonitor *job,
                                               const char           *filename,
                                               GFileMonitorFlags     flags)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  MsgDriveItem *item;
  GError *error = NULL;
  GVfsMonitor *monitor = NULL;
  g_autofree char *item_path = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ create_dir_monitor: %s, %d\n", filename, flags);

  if (flags & G_FILE_MONITOR_SEND_MOVED)
    {
      g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  error = NULL;
  item = resolve (self, filename, cancellable, &item_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  g_debug ("  item path: %s\n", item_path);

  if (!MSG_IS_DRIVE_ITEM_FOLDER (item))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, _("The file is not a directory"));
      goto out;
    }

  monitor = g_vfs_monitor_new (_self);
  g_object_set_data_full (G_OBJECT (monitor), "g-vfs-backend-onedrive-path", g_strdup (item_path), g_free);
  g_hash_table_add (self->monitors, monitor);
  g_object_weak_ref (G_OBJECT (monitor), (GWeakNotify) g_hash_table_remove, self->monitors);
  g_vfs_job_create_monitor_set_monitor (job, monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_clear_object (&monitor);
  g_debug ("- create_dir_monitor\n");
  g_rec_mutex_unlock (&self->mutex);
  return TRUE;
}

static void
g_vfs_backend_onedrive_delete (GVfsBackend   *_self,
                               GVfsJobDelete *job,
                               const char    *filename)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  MsgDriveItem *item = NULL;
  MsgDriveItem *parent;
  GError *error = NULL;
  g_autofree char *item_path = NULL;
  g_autofree char *id = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ delete: %s\n", filename);

  item = resolve (self, filename, cancellable, &item_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  id = get_full_item_id (item);

  parent = resolve_dir (self, filename, cancellable, NULL, NULL, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  if (MSG_IS_DRIVE_ITEM_FOLDER (item) && parent != self->shared_with_me_dir)
    {
      GHashTableIter iter;
      DirItemsKey *key;

      if (!is_dir_listing_valid (self, item))
        {
          rebuild_dir (self, item, cancellable, &error);
          if (error != NULL)
            {
              g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
              goto out;
            }
        }

      g_hash_table_iter_init (&iter, self->dir_items);
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

  g_debug ("  item path: %s\n", item_path);

  if (item == self->root || item == self->home || item == self->shared_with_me_dir)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  g_object_ref (item);
  remove_item (self, parent, item);

  error = NULL;
  msg_drive_service_delete (self->service, item, cancellable, &error);
  g_object_unref (item);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  g_hash_table_foreach (self->monitors, emit_delete_event, item_path);
  g_vfs_job_succeeded (G_VFS_JOB (job));

out:
  g_debug ("- delete\n");
  g_rec_mutex_unlock (&self->mutex);
}

static void
g_vfs_backend_onedrive_enumerate (GVfsBackend           *_self,
                                  GVfsJobEnumerate      *job,
                                  const char            *filename,
                                  GFileAttributeMatcher *matcher,
                                  GFileQueryInfoFlags    flags)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GHashTableIter iter;
  MsgDriveItem *item;
  MsgDriveItem *child;
  GError *error = NULL;
  g_autofree char *parent_path = NULL;
  g_autofree char *id = NULL;
  gboolean is_shared_with_me_dir;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ enumerate: %s\n", filename);

  item = resolve (self, filename, cancellable, &parent_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  if (!MSG_IS_DRIVE_ITEM_FOLDER (item))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY, _("The file is not a directory"));
      goto out;
    }

  if (!is_dir_listing_valid (self, item))
    {
      rebuild_dir (self, item, cancellable, &error);
      if (error != NULL)
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  id = get_full_item_id (item);
  is_shared_with_me_dir = (item == self->shared_with_me_dir);

  g_hash_table_iter_init (&iter, self->items);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &child))
    {
      DirItemsKey *k;
      g_autofree char *child_id = NULL;

      child_id = get_full_item_id (child);
      k = dir_items_key_new (child_id, id);

      if ((is_shared_with_me_dir && is_shared_with_me (child)) ||
          (!is_shared_with_me_dir && g_hash_table_lookup (self->dir_items, k) != NULL))
        {
          g_autoptr (GFileInfo) info = NULL;

          info = g_file_info_new ();
          build_file_info (self, child, flags, info, matcher, NULL);
          g_vfs_job_enumerate_add_info (job, info);
        }

      dir_items_key_free (k);
    }

  g_vfs_job_enumerate_done (job);

out:
  g_debug ("- enumerate\n");
  g_rec_mutex_unlock (&self->mutex);
}

static void
g_vfs_backend_onedrive_make_directory (GVfsBackend          *_self,
                                       GVfsJobMakeDirectory *job,
                                       const char           *filename)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  MsgDriveItem *new_folder = NULL;
  MsgDriveItem *parent;
  MsgDriveItem *existing_item;
  GError *error = NULL;
  g_autofree char *basename = NULL;
  g_autofree char *parent_path = NULL;
  g_autofree char *item_path = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ make_directory: %s\n", filename);

  if (g_strcmp0 (filename, "/") == 0)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  parent = resolve_dir (self, filename, cancellable, &basename, &parent_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  g_debug ("  parent path: %s\n", parent_path);
  if (parent == self->shared_with_me_dir)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  existing_item = resolve_child (self, parent, basename, cancellable, NULL);
  if (existing_item != NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_EXISTS, _("Target file already exists"));
      goto out;
    }

  new_folder = msg_drive_service_create_folder (self->service, parent, basename, cancellable, &error);
   if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  item_path = g_build_path ("/", parent_path, msg_drive_item_get_name (new_folder), NULL);
  g_debug ("  new item path: %s\n", item_path);

  insert_item (self, parent, new_folder);
  g_hash_table_foreach (self->monitors, emit_create_event, item_path);
  g_vfs_job_succeeded (G_VFS_JOB (job));

out:
  g_debug ("- make_directory\n");
  g_rec_mutex_unlock (&self->mutex);
}

static void
g_vfs_backend_onedrive_mount (GVfsBackend  *_self,
                              GVfsJobMount *job,
                              GMountSpec   *spec,
                              GMountSource *source,
                              gboolean      is_automount)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GMountSpec *real_mount_spec;
  GError *error = NULL;
  GList *accounts = NULL;
  g_autolist (MsgDrive) drives = NULL;
  GList *l = NULL;
  const char *host = NULL;
  const char *user = NULL;

  g_debug ("+ mount\n");

  self->client = goa_client_new_sync (cancellable, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  host = g_mount_spec_get (spec, "host");
  user = g_mount_spec_get (spec, "user");
  self->account_identity = g_strconcat (user, "@", host, NULL);

  accounts = goa_client_get_accounts (self->client);
  for (l = accounts; l != NULL; l = l->next)
    {
      GoaObject *object = GOA_OBJECT (l->data);
      g_autoptr (GoaAccount) account = NULL;
      const char *account_identity = NULL;
      const char *provider_type = NULL;

      account = goa_object_get_account (object);
      account_identity = goa_account_get_presentation_identity (account);
      provider_type = goa_account_get_provider_type (account);

      if (g_strcmp0 (provider_type, "ms_graph") == 0 &&
          g_strcmp0 (account_identity, self->account_identity) == 0)
        {
          MsgGoaAuthorizer *authorizer = NULL;

          authorizer = msg_goa_authorizer_new (object);
          self->service = msg_drive_service_new (MSG_AUTHORIZER (authorizer));
          g_object_unref (authorizer);
          break;
        }
    }

  if (!self->service)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("Invalid mount spec"));
      goto out;
    }

  self->root = MSG_DRIVE_ITEM (msg_drive_item_folder_new ());
  msg_drive_item_set_id (self->root, ROOT_ID);
  msg_drive_item_set_name (MSG_DRIVE_ITEM (self->root), self->account_identity);

  drives = msg_drive_service_get_drives (self->service, cancellable, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  for (l = drives; l != NULL; l = l->next)
    {
      MsgDrive *drive = NULL;
      MsgDriveItem *item = NULL;
      g_autoptr (GError) local_error = NULL;
      const char *drive_name = NULL;

      drive = MSG_DRIVE (l->data);
      item = msg_drive_service_get_root (self->service, drive, cancellable, &local_error);
      if (local_error)
        {
          if (g_strcmp0 (local_error->message, "ObjectHandle is Invalid") == 0)
            {
              /* Reduce log level for this specific message as there can
               * be drives which aren't iterable.... problem report created.
               * https://gitlab.gnome.org/GNOME/gvfs/-/issues/763
               */
              g_debug ("Could not get root: %s", local_error->message);
            }
          else
            {
              g_warning ("Could not get root: %s", local_error->message);
            }
          continue;
        }

      if (!self->home)
        {
          self->home = item;
        }

      drive_name = msg_drive_get_name (drive);
      if (drive_name)
        {
          msg_drive_item_set_name (item, drive_name);
        }
      else
        {
          msg_drive_item_set_name (item, _("My Files"));
        }

      insert_custom_item (self, item, ROOT_ID);
    }

  self->shared_with_me_dir = MSG_DRIVE_ITEM (msg_drive_item_folder_new ());
  msg_drive_item_set_id (self->shared_with_me_dir, SHARED_WITH_ME_ID);
  msg_drive_item_set_name (self->shared_with_me_dir, _("Shared with me"));
  insert_custom_item (self, self->shared_with_me_dir, ROOT_ID);

  g_vfs_backend_set_default_location (_self, msg_drive_item_get_name (self->home));

  real_mount_spec = g_mount_spec_new ("onedrive");
  g_mount_spec_set (real_mount_spec, "host", host);
  g_mount_spec_set (real_mount_spec, "user", user);
  g_vfs_backend_set_mount_spec (_self, real_mount_spec);
  g_mount_spec_unref (real_mount_spec);

  g_vfs_backend_set_display_name (_self, self->account_identity);
  g_vfs_job_succeeded (G_VFS_JOB (job));

out:
  g_list_free_full (accounts, g_object_unref);
  g_debug ("- mount\n");
}

static void
g_vfs_backend_onedrive_open_icon_for_read (GVfsBackend            *_self,
                                           GVfsJobOpenIconForRead *job,
                                           const char             *icon_id)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GInputStream *stream;
  GError *error = NULL;
  ReadHandle *rh;

  g_debug ("+ open_icon_for_read: %s\n", icon_id);

  stream = msg_drive_service_download_url (self->service, icon_id, cancellable, &error);
  if (stream == NULL)
    {
      g_debug (" Could not download icon: %s\n", error->message);
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_FAILED, _("Error getting data from file"));
      goto out;
    }

  rh = read_handle_new (NULL, stream, icon_id);
  g_vfs_job_open_for_read_set_handle (G_VFS_JOB_OPEN_FOR_READ (job), rh);
  g_vfs_job_open_for_read_set_can_seek (G_VFS_JOB_OPEN_FOR_READ (job), TRUE);
  g_vfs_job_succeeded (G_VFS_JOB (job));

out:
  g_debug ("- open_icon_for_read\n");
}

static gboolean
g_vfs_backend_onedrive_try_query_fs_info (GVfsBackend           *_self,
                                          GVfsJobQueryFsInfo    *job,
                                          const char            *filename,
                                          GFileInfo             *info,
                                          GFileAttributeMatcher *matcher)
{
  GMountSpec *spec = NULL;
  const char *type;

  g_debug ("+ query_fs_info: %s\n", filename);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, FALSE);

  spec = g_vfs_backend_get_mount_spec (_self);
  type = g_mount_spec_get_type (spec);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, type);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, TRUE);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_IF_ALWAYS);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  g_debug ("- query_fs_info\n");

  return TRUE;
}

static void
g_vfs_backend_onedrive_query_info (GVfsBackend           *_self,
                                   GVfsJobQueryInfo      *job,
                                   const char            *filename,
                                   GFileQueryInfoFlags    flags,
                                   GFileInfo             *info,
                                   GFileAttributeMatcher *matcher)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  MsgDriveItem *item = NULL;
  g_autofree char *item_path = NULL;
  GError *error = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ query_info: %s, %d\n", filename, flags);
  log_dir_items (self);

  item = resolve (self, filename, cancellable, &item_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  g_debug ("  item path: %s\n", item_path);

  build_file_info (self, item, flags, info, matcher, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

out:
  g_debug ("- query_info\n");
  g_rec_mutex_unlock (&self->mutex);
}

static gboolean
g_vfs_backend_onedrive_try_query_info_on_read (GVfsBackend           *_self,
                                               GVfsJobQueryInfoRead  *job,
                                               GVfsBackendHandle      handle,
                                               GFileInfo             *info,
                                               GFileAttributeMatcher *matcher)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);
  GError *error = NULL;
  gboolean ret = TRUE;
  ReadHandle *read_handle = (ReadHandle *) handle;

  g_debug ("+ try_query_info_on_read: %p\n", handle);

  g_debug ("  item path: %s\n", read_handle->item_path);

  build_file_info (self, read_handle->item, G_FILE_QUERY_INFO_NONE, info, matcher, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      ret = FALSE;
      goto out;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

out:
  g_debug ("- try_query_info_on_read\n");

  return ret;
}

static gboolean
g_vfs_backend_onedrive_try_query_info_on_write (GVfsBackend           *_self,
                                                GVfsJobQueryInfoWrite *job,
                                                GVfsBackendHandle      handle,
                                                GFileInfo             *info,
                                                GFileAttributeMatcher *matcher)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);
  GError *error = NULL;
  WriteHandle *wh = (WriteHandle *) handle;
  gboolean ret = TRUE;

  g_debug ("+ try_query_info_on_write: %p\n", handle);
  g_debug ("  item path: %s\n", wh->item_path);

  build_file_info (self, wh->item, G_FILE_QUERY_INFO_NONE, info, matcher, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      ret = FALSE;
      goto out;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

out:
  g_debug ("- try_query_info_on_write\n");
  return ret;
}

static void
g_vfs_backend_onedrive_open_for_read (GVfsBackend        *backend,
                                      GVfsJobOpenForRead *job,
                                      const char         *filename)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (backend);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  MsgDriveItem *item = NULL;
  GInputStream *stream;
  GError *error = NULL;
  g_autofree char *item_path = NULL;
  ReadHandle *read_handle;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ open_for_read: %s\n", filename);

  item = resolve (self, filename, cancellable, &item_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  g_debug ("  item path: %s\n", item_path);

  if (MSG_IS_DRIVE_ITEM_FOLDER (item))
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY, _("Can’t open directory"));
      goto out;
    }

  stream = msg_drive_service_download_item (self->service, item, cancellable, &error);
  if (stream == NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
  }

  read_handle = read_handle_new (item, stream, item_path);

  g_vfs_job_open_for_read_set_handle (job, read_handle);
  g_vfs_job_open_for_read_set_can_seek (job, TRUE);
  g_vfs_job_succeeded (G_VFS_JOB (job));

out:
  g_debug ("- open_for_read\n");
  g_rec_mutex_unlock (&self->mutex);
}

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
g_vfs_backend_onedrive_try_read (GVfsBackend       *_self,
                                 GVfsJobRead       *job,
                                 GVfsBackendHandle  handle,
                                 gchar             *buffer,
                                 gsize              bytes_requested)
{
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  ReadHandle *rh = (ReadHandle *)handle;

  g_debug ("+ read: %p\n", rh->stream);
  g_input_stream_read_async (rh->stream, buffer, bytes_requested, G_PRIORITY_DEFAULT, cancellable, read_cb, job);
  return TRUE;
}

static void
g_vfs_backend_onedrive_seek_on_read (GVfsBackend       *_self,
                                     GVfsJobSeekRead   *job,
                                     GVfsBackendHandle  handle,
                                     goffset            offset,
                                     GSeekType          type)
{
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GError *error = NULL;
  ReadHandle *rh = (ReadHandle *)handle;
  GInputStream *stream = rh->stream;
  goffset cur_offset;

  g_debug ("+ seek_on_read: %p\n", handle);

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

static void
g_vfs_backend_onedrive_seek_on_write (GVfsBackend       *backend,
                                      GVfsJobSeekWrite  *job,
                                      GVfsBackendHandle  handle,
                                      goffset            offset,
                                      GSeekType          type)
{
  WriteHandle *wh = handle;
  g_autoptr (GError) error = NULL;

  g_debug ("+ seek_on_write: %p\n", handle);

  if (g_seekable_seek (G_SEEKABLE (wh->stream), offset, job->seek_type, NULL, &error))
    {
      g_vfs_job_seek_write_set_offset (job, g_seekable_tell (G_SEEKABLE (wh->stream)));
      g_vfs_job_succeeded (G_VFS_JOB (job));
    }
  else
    {
      g_warning ("Could not seek: %s", error->message);
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    }

  g_debug ("- seek_on_write\n");
}

static void
close_read_cb (GObject      *source_object,
               GAsyncResult *res,
               gpointer      user_data)
{
  GError *error = NULL;
  GInputStream *stream = G_INPUT_STREAM (source_object);
  GVfsJobCloseRead *job = G_VFS_JOB_CLOSE_READ (user_data);
  ReadHandle *rh = job->handle;

  g_input_stream_close_finish (stream, res, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }
  g_vfs_job_succeeded (G_VFS_JOB (job));

out:
  read_handle_free (rh);
  g_debug ("- close_read\n");
}

static gboolean
g_vfs_backend_onedrive_try_close_read (GVfsBackend       *backend,
                                       GVfsJobCloseRead  *job,
                                       GVfsBackendHandle  handle)
{
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  ReadHandle *rh = (ReadHandle *) handle;

  g_debug ("+ close_read: %p\n", handle);

  g_input_stream_close_async (rh->stream, G_PRIORITY_DEFAULT, cancellable, close_read_cb, job);
  return TRUE;
}

static void
g_vfs_backend_onedrive_set_display_name (GVfsBackend           *_self,
                                         GVfsJobSetDisplayName *job,
                                         const char            *filename,
                                         const char            *display_name)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  MsgDriveItem *item = NULL;
  MsgDriveItem *new_item = NULL;
  g_autofree char *item_path = NULL;
  MsgDriveItem *parent = NULL;
  GError *error = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ set_display_name: %s, %s\n", filename, display_name);

  parent = resolve_dir (self, filename, cancellable, NULL, NULL, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  item = resolve (self, filename, cancellable, &item_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_debug ("  item path: %s\n", item_path);

  if (item == self->root || item == self->home || item == self->shared_with_me_dir)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  g_object_ref (item);
  remove_item (self, parent, item);

  new_item = msg_drive_service_rename (self->service, item, display_name, cancellable, &error);
  g_object_unref (item);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      g_object_unref (item);
      goto out;
    }


  insert_item (self, parent, new_item);
  g_hash_table_foreach (self->monitors, emit_renamed_event, item_path);
  g_vfs_job_set_display_name_set_new_path (job, item_path);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_debug ("- set_display_name\n");
  g_rec_mutex_unlock (&self->mutex);
}

static void
g_vfs_backend_onedrive_create (GVfsBackend         *_self,
                               GVfsJobOpenForWrite *job,
                               const char          *filename,
                               GFileCreateFlags     flags)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  g_autoptr (MsgDriveItemFile) item_file = NULL;
  g_autoptr (MsgDriveItem) new_item = NULL;
  MsgDriveItem *existing_item;
  MsgDriveItem *parent;
  GError *error = NULL;
  WriteHandle *handle;
  g_autofree char *basename = NULL;
  g_autofree char *item_path = NULL;
  g_autofree char *parent_path = NULL;
  GOutputStream *stream = NULL;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ create: %s, %d\n", filename, flags);

  if (g_strcmp0 (filename, "/") == 0)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  parent = resolve_dir (self, filename, cancellable, &basename, &parent_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_debug ("  parent path: %s\n", parent_path);

  if (parent == self->root || parent == self->shared_with_me_dir)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  existing_item = resolve_child (self, parent, basename, cancellable, NULL);
  if (existing_item != NULL)
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

  item_file = msg_drive_item_file_new ();
  msg_drive_item_set_name (MSG_DRIVE_ITEM (item_file), basename);

  error = NULL;
  new_item = msg_drive_service_add_item_to_folder (self->service, parent, MSG_DRIVE_ITEM (item_file), cancellable, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  item_path = g_build_path ("/", parent_path, msg_drive_item_get_name (new_item), NULL);
  g_debug ("  new item path: %s\n", item_path);

  insert_item (self, parent, new_item);
  g_hash_table_foreach (self->monitors, emit_create_event, item_path);

  stream = msg_drive_service_update (self->service, new_item, cancellable, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  handle = write_handle_new (new_item, stream, filename, item_path);
  g_vfs_job_open_for_write_set_handle (job, handle);
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_debug ("- create\n");
  g_rec_mutex_unlock (&self->mutex);
}

static void
g_vfs_backend_onedrive_write (GVfsBackend       *_self,
                              GVfsJobWrite      *job,
                              GVfsBackendHandle  handle,
                              gchar             *buffer,
                              gsize              buffer_size)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GError *error;
  WriteHandle *wh = (WriteHandle *) handle;
  gssize nwrite;

  g_debug ("+ write\n");

  g_debug ("  writing to stream: %p\n", wh->stream);
  g_debug ("  item path: %s\n", wh->item_path);

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

  g_hash_table_foreach (self->monitors, emit_changed_event, wh->item_path);
  g_vfs_job_write_set_written_size (job, (gsize) nwrite);
  g_vfs_job_succeeded (G_VFS_JOB (job));

 out:
  g_debug ("- write\n");
}

static void
g_vfs_backend_onedrive_close_write (GVfsBackend       *_self,
                                    GVfsJobCloseWrite *job,
                                    GVfsBackendHandle  handle)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  g_autoptr (MsgDriveItem) new_item = NULL;
  MsgDriveItem *parent;
  GError *error = NULL;
  WriteHandle *wh = (WriteHandle *) handle;

  g_debug ("+ close_write: %p\n", handle);
  new_item = msg_drive_service_update_finish (self->service, wh->item, wh->stream, cancellable, &error);
  if (new_item == NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  g_debug ("  new item path: %s\n", wh->item_path);

  parent = resolve_dir (self, wh->item_path, cancellable, NULL, NULL, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      goto out;
    }

  remove_item (self, parent, wh->item);
  insert_item (self, parent, MSG_DRIVE_ITEM (new_item));
  g_hash_table_foreach (self->monitors, emit_changes_done_event, wh->item_path);
  g_vfs_job_succeeded (G_VFS_JOB (job));

out:
  write_handle_free (wh);
  g_debug ("- close_write\n");
}

static void
g_vfs_backend_onedrive_replace (GVfsBackend         *_self,
                                GVfsJobOpenForWrite *job,
                                const char          *filename,
                                const char          *etag,
                                gboolean             make_backup,
                                GFileCreateFlags     flags)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);
  GCancellable *cancellable = G_VFS_JOB (job)->cancellable;
  GOutputStream *stream = NULL;
  GError *error = NULL;
  MsgDriveItemFile *item;
  MsgDriveItem *new_item;
  MsgDriveItem *parent;
  MsgDriveItem *existing_item;
  WriteHandle *handle;
  g_autofree char *parent_path = NULL;
  g_autofree char *basename = NULL;
  g_autofree char *item_path = NULL;
  gboolean needs_overwrite = FALSE;

  g_rec_mutex_lock (&self->mutex);
  g_debug ("+ replace: %s, %s, %d, %d\n", filename, etag, make_backup, flags);

  if (make_backup)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_CANT_CREATE_BACKUP,
                        _("Backups not supported"));
      goto out;
    }

  if (g_strcmp0 (filename, "/") == 0)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  parent = resolve_dir (self, filename, cancellable, &basename, &parent_path, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
      g_error_free (error);
      goto out;
    }

  g_debug ("  parent path: %s\n", parent_path);

  if (parent == self->root || parent == self->shared_with_me_dir)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));
      goto out;
    }

  existing_item = resolve_child (self, parent, basename, cancellable, NULL);
  if (existing_item != NULL)
    {
      if (MSG_IS_DRIVE_ITEM_FOLDER (existing_item))
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY, _("Target file is a directory"));
          goto out;
        }

      needs_overwrite = TRUE;
    }

  g_debug ("  will overwrite: %d\n", needs_overwrite);

  if (needs_overwrite)
    {
      item_path = g_build_path ("/", parent_path, msg_drive_item_get_name (existing_item), NULL);
      g_debug ("  existing item path: %s\n", item_path);

      error = NULL;
      stream = msg_drive_service_update (self->service, existing_item, cancellable, &error);
      if (error != NULL)
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }

      handle = write_handle_new (existing_item, stream, filename, item_path);
    }
  else
    {
      item = msg_drive_item_file_new ();
      msg_drive_item_set_name (MSG_DRIVE_ITEM (item), basename);

      error = NULL;
      new_item = msg_drive_service_add_item_to_folder (self->service, parent, MSG_DRIVE_ITEM (item), cancellable, &error);
      if (error != NULL)
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }

      item_path = g_build_filename (parent_path, msg_drive_item_get_name (MSG_DRIVE_ITEM (item)), NULL);
      g_debug ("  new item path: %s\n", item_path);

      insert_item (self, parent, MSG_DRIVE_ITEM (new_item));
      g_hash_table_foreach (self->monitors, emit_create_event, item_path);

      stream = msg_drive_service_update (self->service, new_item, cancellable, &error);
      if (error != NULL)
        {
          g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
          g_error_free (error);
          goto out;
        }

      handle = write_handle_new (MSG_DRIVE_ITEM (new_item), stream, filename, item_path);
    }

  g_vfs_job_open_for_write_set_handle (job, handle);
  g_vfs_job_open_for_write_set_can_seek (job, TRUE);
  g_vfs_job_succeeded (G_VFS_JOB (job));

out:
  g_debug ("- replace\n");
  g_rec_mutex_unlock (&self->mutex);
}

static void
g_vfs_backend_onedrive_dispose (GObject *_self)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);

  g_clear_object (&self->service);
  g_clear_object (&self->root);
  g_clear_object (&self->home);
  g_clear_object (&self->shared_with_me_dir);
  g_clear_object (&self->client);
  g_clear_pointer (&self->items, g_hash_table_unref);
  g_clear_pointer (&self->dir_items, g_hash_table_unref);
  g_clear_pointer (&self->dir_timestamps, g_hash_table_unref);

  G_OBJECT_CLASS (g_vfs_backend_onedrive_parent_class)->dispose (_self);
}

static void
g_vfs_backend_onedrive_finalize (GObject *_self)
{
  GVfsBackendOnedrive *self = G_VFS_BACKEND_ONEDRIVE (_self);

  g_hash_table_foreach (self->monitors, remove_monitor_weak_ref, self->monitors);
  g_hash_table_unref (self->monitors);
  g_free (self->account_identity);

  g_rec_mutex_clear (&self->mutex);

  G_OBJECT_CLASS (g_vfs_backend_onedrive_parent_class)->finalize (_self);
}

static void
g_vfs_backend_onedrive_class_init (GVfsBackendOnedriveClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

  gobject_class->dispose = g_vfs_backend_onedrive_dispose;
  gobject_class->finalize = g_vfs_backend_onedrive_finalize;

  backend_class->try_close_read = g_vfs_backend_onedrive_try_close_read;
  backend_class->close_write = g_vfs_backend_onedrive_close_write;
  backend_class->create = g_vfs_backend_onedrive_create;
  backend_class->try_create_dir_monitor = g_vfs_backend_onedrive_try_create_dir_monitor;
  backend_class->delete = g_vfs_backend_onedrive_delete;
  backend_class->enumerate = g_vfs_backend_onedrive_enumerate;
  backend_class->make_directory = g_vfs_backend_onedrive_make_directory;
  backend_class->mount = g_vfs_backend_onedrive_mount;
  backend_class->open_for_read = g_vfs_backend_onedrive_open_for_read;
  backend_class->open_icon_for_read = g_vfs_backend_onedrive_open_icon_for_read;
  backend_class->try_query_fs_info = g_vfs_backend_onedrive_try_query_fs_info;
  backend_class->query_info = g_vfs_backend_onedrive_query_info;
  backend_class->try_query_info_on_read = g_vfs_backend_onedrive_try_query_info_on_read;
  backend_class->try_query_info_on_write = g_vfs_backend_onedrive_try_query_info_on_write;
  backend_class->seek_on_read = g_vfs_backend_onedrive_seek_on_read;
  backend_class->seek_on_write = g_vfs_backend_onedrive_seek_on_write;
  backend_class->set_display_name = g_vfs_backend_onedrive_set_display_name;
  backend_class->try_read = g_vfs_backend_onedrive_try_read;
  backend_class->replace = g_vfs_backend_onedrive_replace;
  backend_class->write = g_vfs_backend_onedrive_write;
}

static void
g_vfs_backend_onedrive_init (GVfsBackendOnedrive *self)
{
  self->items = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  self->dir_items = g_hash_table_new_full (items_in_folder_hash,
                                           items_in_folder_equal,
                                           dir_items_key_free,
                                           g_object_unref);
  self->dir_timestamps = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  self->monitors = g_hash_table_new (NULL, NULL);
  g_rec_mutex_init (&self->mutex);
}

