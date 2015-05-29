/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2009 Red Hat, Inc.
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
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#ifndef __META_TREE_H__
#define __META_TREE_H__

#include <glib.h>
#include "metadata-dbus.h"

typedef struct _MetaTree MetaTree;
typedef struct _MetaLookupCache MetaLookupCache;

typedef enum {
  META_KEY_TYPE_NONE,
  META_KEY_TYPE_STRING,
  META_KEY_TYPE_STRINGV
} MetaKeyType;

/* Note: These are called with the read-lock held, so
   don't call any MetaTree operations */
typedef gboolean (*meta_tree_dir_enumerate_callback) (const char *entry,
						      guint64 last_changed,
						      gboolean has_children,
						      gboolean has_data,
						      gpointer user_data);

typedef gboolean (*meta_tree_keys_enumerate_callback) (const char *key,
						       MetaKeyType type,
						       gpointer value,
						       gpointer user_data);

/* MetaLookupCache is not threadsafe */
MetaLookupCache *meta_lookup_cache_new         (void);
void             meta_lookup_cache_free        (MetaLookupCache *cache);
MetaTree        *meta_lookup_cache_lookup_path (MetaLookupCache *cache,
						const char *filename,
						guint64 device,
						gboolean for_write,
						char **tree_path);

/* All public MetaTree calls are threadsafe */
MetaTree *  meta_tree_open           (const char *filename,
				      gboolean    for_write);
MetaTree *  meta_tree_lookup_by_name (const char *name,
				      gboolean    for_write);
MetaTree *  meta_tree_ref            (MetaTree   *tree);
void        meta_tree_unref          (MetaTree   *tree);
gboolean    meta_tree_refresh        (MetaTree   *tree);
const char *meta_tree_get_filename   (MetaTree   *tree);
gboolean    meta_tree_exists         (MetaTree   *tree);
gboolean    meta_tree_is_on_nfs      (MetaTree   *tree);

MetaKeyType meta_tree_lookup_key_type  (MetaTree                         *tree,
					const char                       *path,
					const char                       *key);
guint64     meta_tree_get_last_changed (MetaTree                         *tree,
					const char                       *path);
char *      meta_tree_lookup_string    (MetaTree                         *tree,
					const char                       *path,
					const char                       *key);
char **     meta_tree_lookup_stringv   (MetaTree                         *tree,
					const char                       *path,
					const char                       *key);
void        meta_tree_enumerate_dir    (MetaTree                         *tree,
					const char                       *path,
					meta_tree_dir_enumerate_callback  callback,
					gpointer                          user_data);
void        meta_tree_enumerate_keys   (MetaTree                         *tree,
					const char                       *path,
					meta_tree_keys_enumerate_callback callback,
					gpointer                          user_data);
gboolean    meta_tree_flush            (MetaTree                         *tree);
gboolean    meta_tree_unset            (MetaTree                         *tree,
					const char                       *path,
					const char                       *key);
gboolean    meta_tree_set_string       (MetaTree                         *tree,
					const char                       *path,
					const char                       *key,
					const char                       *value);
gboolean    meta_tree_set_stringv      (MetaTree                         *tree,
					const char                       *path,
					const char                       *key,
					char                            **value);
gboolean    meta_tree_remove           (MetaTree                         *tree,
					const char                       *path);
gboolean    meta_tree_copy             (MetaTree                         *tree,
					const char                       *src,
					const char                       *dest);

GVfsMetadata *meta_tree_get_metadata_proxy (void);

#endif /* __META_TREE_H__ */
