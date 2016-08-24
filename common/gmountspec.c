/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
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

#include <config.h>

#include <string.h>
#include <stdlib.h>

#include <glib/gi18n-lib.h>

#include "gmountspec.h"

static GHashTable *unique_hash = NULL;
G_LOCK_DEFINE_STATIC(unique_hash);

static int
item_compare (const void *_a, const void *_b)
{
  const GMountSpecItem *a = _a;
  const GMountSpecItem *b = _b;

  return strcmp (a->key, b->key);
}

GMountSpec *
g_mount_spec_new (const char *type)
{
  GMountSpec *spec;

  spec = g_new0 (GMountSpec, 1);
  spec->ref_count = 1;
  spec->items = g_array_new (FALSE, TRUE, sizeof (GMountSpecItem));
  spec->mount_prefix = g_strdup ("/");
  
  if (type != NULL)
    g_mount_spec_set (spec, "type", type);
  
  return spec;
}

/* Takes ownership of passed in data */
GMountSpec *
g_mount_spec_new_from_data (GArray *items,
			    char *mount_prefix)
{
  GMountSpec *spec;

  spec = g_new0 (GMountSpec, 1);
  spec->ref_count = 1;
  spec->items = items;
  if (mount_prefix == NULL)
    spec->mount_prefix = g_strdup ("/");
  else
    spec->mount_prefix = g_mount_spec_canonicalize_path (mount_prefix);

  g_array_sort (spec->items, item_compare);
  
  return spec;
}

GMountSpec *
g_mount_spec_get_unique_for (GMountSpec *spec)
{
  GMountSpec *unique_spec;

  if (spec->is_unique)
    return g_mount_spec_ref (spec);
  
  G_LOCK (unique_hash);
  
  if (unique_hash == NULL)
    unique_hash = g_hash_table_new (g_mount_spec_hash, (GEqualFunc)g_mount_spec_equal);

  unique_spec = g_hash_table_lookup (unique_hash, spec);

  if (unique_spec == NULL)
    {
      spec->is_unique = TRUE;
      g_hash_table_insert (unique_hash, spec, spec);
      unique_spec = spec;
    }
  
  g_mount_spec_ref (unique_spec);
  
  G_UNLOCK (unique_hash);

  return unique_spec;
}

void
g_mount_spec_set_mount_prefix  (GMountSpec      *spec,
				const char      *mount_prefix)
{
  g_free (spec->mount_prefix);
  spec->mount_prefix = g_mount_spec_canonicalize_path (mount_prefix);
}


static void 
add_item (GMountSpec *spec,
	  const char *key,
	  char *value)
{
  GMountSpecItem item;

  g_return_if_fail (key != NULL);
  g_return_if_fail (value != NULL);

  item.key = g_strdup (key);
  item.value = value;

  g_array_append_val (spec->items, item);
}

static void 
g_mount_spec_set_with_len_internal (GMountSpec *spec,
                                    const char *key,
                                    const char *value,
                                    int value_len,
                                    gboolean copy)
{
  int i;
  char *value_copy;

  g_return_if_fail (key != NULL);
  g_return_if_fail (value != NULL);

  if (copy)
    {
      if (value_len == -1)
        value_copy = g_strdup (value);
      else
        value_copy = g_strndup (value, value_len);
    }
  else
    value_copy = (char*) value;

  if (g_str_equal ("prefix", key))
    {
      g_mount_spec_set_mount_prefix (spec, value_copy);
      g_free (value_copy);
      return;
    }

  for (i = 0; i < spec->items->len; i++)
    {
      GMountSpecItem *item = &g_array_index (spec->items, GMountSpecItem, i);
      if (strcmp (item->key, key) == 0)
	{
	  g_free (item->value);
	  item->value = value_copy;
	  return;
	}
    }

  add_item (spec, key, value_copy);
  g_array_sort (spec->items, item_compare);
}

void 
g_mount_spec_set_with_len (GMountSpec *spec,
                           const char *key,
                           const char *value,
                           int value_len)
{
  g_mount_spec_set_with_len_internal (spec, key, value, value_len, TRUE);
}

void 
g_mount_spec_set (GMountSpec *spec,
		  const char *key,
		  const char *value)
{
  g_mount_spec_set_with_len (spec, key, value, -1);
}

void
g_mount_spec_take (GMountSpec *spec,
                   const char *key,
                   char *value)
{
  g_mount_spec_set_with_len_internal (spec, key, value, -1, FALSE);
}

GMountSpec *
g_mount_spec_copy (GMountSpec *spec)
{
  GMountSpec *copy;
  int i;

  copy = g_mount_spec_new (NULL);
  g_mount_spec_set_mount_prefix (copy, spec->mount_prefix);

  for (i = 0; i < spec->items->len; i++)
    {
      GMountSpecItem *item = &g_array_index (spec->items, GMountSpecItem, i);
      g_mount_spec_set (copy, item->key, item->value);
    }
  
  return copy;
}

GMountSpec *
g_mount_spec_ref (GMountSpec *spec)
{
  g_atomic_int_inc (&spec->ref_count);
  return spec;
}

void
g_mount_spec_unref (GMountSpec *spec)
{
  int i;

  if (g_atomic_int_dec_and_test (&spec->ref_count))
    {
      G_LOCK (unique_hash);
      if (unique_hash != NULL &&
	  spec->is_unique)
	g_hash_table_remove (unique_hash, spec);
      G_UNLOCK (unique_hash);
      
      g_free (spec->mount_prefix);
      for (i = 0; i < spec->items->len; i++)
	{
	  GMountSpecItem *item = &g_array_index (spec->items, GMountSpecItem, i);
	  g_free (item->key);
	  g_free (item->value);
	}
      g_array_free (spec->items, TRUE);
      
      g_free (spec);
    }
}

GMountSpec *
g_mount_spec_from_dbus (GVariant *value)
{
  GMountSpec *spec;
  const gchar *key;
  const gchar *mount_prefix;
  GVariantIter *iter_mount_spec_items;
  GVariant *v;

  mount_prefix = NULL;
  g_variant_get (value, "(^&aya{sv})",
                 &mount_prefix,
                 &iter_mount_spec_items);
  
  spec = g_mount_spec_new (NULL);
  g_free (spec->mount_prefix);
  spec->mount_prefix = NULL;
  if (mount_prefix && mount_prefix[0])
    spec->mount_prefix = g_strdup (mount_prefix);

  while (g_variant_iter_loop (iter_mount_spec_items, "{&sv}", &key, &v))
    {
      add_item (spec, key, g_variant_dup_bytestring (v, NULL));
    }

  g_variant_iter_free (iter_mount_spec_items);

  /* Sort on key */
  g_array_sort (spec->items, item_compare);
  
  return spec;
}

GVariant *
g_mount_spec_to_dbus_with_path (GMountSpec *spec,
				const char *path)
{
  GVariantBuilder builder;
  GVariant *v;
  int i;

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  for (i = 0; i < spec->items->len; i++)
    {
      GMountSpecItem *item = &g_array_index (spec->items, GMountSpecItem, i);

      g_variant_builder_add_value (&builder, g_variant_new ("{sv}",
                                                            item->key,
                                                            g_variant_new_bytestring (item->value)));
    }
  
  v = g_variant_new ("(^aya{sv})",
                      path ? path : "",
                      &builder);
  g_variant_builder_clear (&builder);
  
  return v;
}

GVariant *
g_mount_spec_to_dbus (GMountSpec *spec)
{
  return g_mount_spec_to_dbus_with_path (spec, spec->mount_prefix);
}

static gboolean
items_equal (GArray *a,
	     GArray *b)
{
  int i;
  
  if (a->len != b->len)
    return FALSE;

  for (i = 0; i < a->len; i++)
    {
      GMountSpecItem *item_a = &g_array_index (a, GMountSpecItem, i);
      GMountSpecItem *item_b = &g_array_index (b, GMountSpecItem, i);
      
      if (strcmp (item_a->key, item_b->key) != 0)
	return FALSE;
      if (strcmp (item_a->value, item_b->value) != 0)
	return FALSE;
    }
  
  return TRUE;
}

static gboolean
path_has_prefix (const char *path,
		 const char *prefix)
{
  int prefix_len;

  if (prefix == NULL)
    return TRUE;

  prefix_len = strlen (prefix);
  
  if (strncmp (path, prefix, prefix_len) == 0 &&
      (prefix_len == 0 || /* empty prefix always matches */
       prefix[prefix_len - 1] == '/' || /* last char in prefix was a /, so it must be in path too */
       path[prefix_len] == 0 ||
       path[prefix_len] == '/'))
    return TRUE;
  
  return FALSE;
}

guint
g_mount_spec_hash (gconstpointer _mount)
{
  GMountSpec *mount = (GMountSpec *) _mount;
  guint hash;
  int i;

  hash = 0;
  if (mount->mount_prefix)
    hash ^= g_str_hash (mount->mount_prefix);
  
  for (i = 0; i < mount->items->len; i++)
    {
      GMountSpecItem *item = &g_array_index (mount->items, GMountSpecItem, i);
      hash ^= g_str_hash (item->value);
    }
  
  return hash;
}

gboolean
g_mount_spec_equal (GMountSpec      *mount1,
		    GMountSpec      *mount2)
{
  return items_equal (mount1->items, mount2->items) &&
    ((mount1->mount_prefix == mount2->mount_prefix) ||
     (mount1->mount_prefix != NULL && mount2->mount_prefix != NULL &&
      strcmp (mount1->mount_prefix, mount2->mount_prefix) == 0));
}

gboolean
g_mount_spec_match_with_path (GMountSpec      *mount,
			      GMountSpec      *spec,
			      const char      *path)
{
  if (items_equal (mount->items, spec->items) &&
      path_has_prefix (path, mount->mount_prefix))
    return TRUE;
  return FALSE;
}

gboolean
g_mount_spec_match (GMountSpec      *mount,
		    GMountSpec      *path)
{
  return g_mount_spec_match_with_path (mount, path, path->mount_prefix);
}

const char *
g_mount_spec_get (GMountSpec *spec,
		  const char *key)
{
  int i;
  
  for (i = 0; i < spec->items->len; i++)
    {
      GMountSpecItem *item = &g_array_index (spec->items, GMountSpecItem, i);
      
      if (strcmp (item->key, key) == 0)
	return item->value;
    }

  return NULL;
}

const char *
g_mount_spec_get_type (GMountSpec *spec)
{
  return g_mount_spec_get (spec, "type");
}
 
char *
g_mount_spec_to_string (GMountSpec *spec)
{
  GString *str;
  int i;
  gboolean first;

  if (spec == NULL)
    return g_strdup ("(null)");

  str = g_string_new (g_mount_spec_get_type (spec));
  g_string_append_c (str, ':');

  first = TRUE;
  for (i = 0; i < spec->items->len; i++)
    {
      GMountSpecItem *item = &g_array_index (spec->items, GMountSpecItem, i);

      if (strcmp (item->key, "type") == 0)
	continue;

      if (!first)
	g_string_append_c (str, ',');
      first = FALSE;

      g_string_append_printf (str, "%s=", item->key);
      g_string_append_uri_escaped (str, item->value,
				   "$&'()*+",
				   TRUE);
    }

  if (strcmp (spec->mount_prefix, "/") != 0)
    {
      g_string_append_printf (str, ",prefix=");
      g_string_append_uri_escaped (str, spec->mount_prefix,
				   "$&'()*+",
				   TRUE);
    }

  return g_string_free (str, FALSE);
}

GMountSpec *
g_mount_spec_new_from_string (const gchar     *str,
                              GError         **error)
{
  GArray *items;
  GMountSpec *mount_spec;
  char **kv_pairs;
  char *mount_prefix;
  const char *colon;
  GMountSpecItem item;
  int i;

  g_return_val_if_fail (str != NULL, NULL);

  mount_spec = NULL;
  mount_prefix = NULL;
  items = g_array_new (FALSE, TRUE, sizeof (GMountSpecItem));

  colon = strchr (str, ':');
  if (colon)
    {
      item.key = g_strdup ("type");
      item.value = g_strndup (str, colon - str);
      g_array_append_val (items, item);
      str = colon + 1;
    }

  kv_pairs = g_strsplit (str, ",", 0);
  for (i = 0; kv_pairs[i] != NULL; i++)
    {
      char **tokens;

      tokens = g_strsplit (kv_pairs[i], "=", 0);
      if (g_strv_length (tokens) != 2)
        {
          g_set_error (error,
                       G_IO_ERROR,
                       G_IO_ERROR_INVALID_ARGUMENT,
                       "Encountered invalid key/value pair '%s' while decoding GMountSpec",
                       kv_pairs[i]);
          g_strfreev (tokens);
          g_strfreev (kv_pairs);
          goto fail;
        }

      item.value = g_uri_unescape_string (tokens[1], NULL);
      if (strcmp (tokens[0], "prefix") == 0)
        {
          g_free (mount_prefix);
          mount_prefix = item.value;
        }
      else
        {
	  item.key = g_strdup (tokens[0]);
          g_array_append_val (items, item);
        }

      g_strfreev (tokens);
    }
  g_strfreev (kv_pairs);

  if (mount_prefix == NULL)
    mount_prefix = g_strdup ("/");

  /* this constructor takes ownership of the data we pass in */
  mount_spec = g_mount_spec_new_from_data (items,
                                           mount_prefix);

  return mount_spec;

 fail:
  for (i = 0; i < items->len; i++)
    {
      GMountSpecItem *item = &g_array_index (items, GMountSpecItem, i);
      g_free (item->key);
      g_free (item->value);
    }
  g_array_free (items, TRUE);
  g_free (mount_prefix);
  return NULL;
}


char *
g_mount_spec_canonicalize_path (const char *path)
{
  char *canon, *start, *p, *q;

  if (*path != '/')
    canon = g_strconcat ("/", path, NULL);
  else
    canon = g_strdup (path);

  /* Skip initial slash */
  start = canon + 1;

  p = start;
  while (*p != 0)
    {
      if (p[0] == '.' && (p[1] == 0 || p[1] == '/'))
	{
	  memmove (p, p+1, strlen (p+1)+1);
	}
      else if (p[0] == '.' && p[1] == '.' && (p[2] == 0 || p[2] == '/'))
	{
	  q = p + 2;
	  /* Skip previous separator */
	  p = p - 2;
	  if (p < start)
	    p = start;
	  while (p > start && *p != '/')
	    p--;
	  if (*p == '/')
	    p++;
	  memmove (p, q, strlen (q)+1);
	}
      else
	{
	  /* Skip until next separator */
	  while (*p != 0 && *p != '/')
	    p++;

	  /* Keep one separator */
	  if (*p != 0)
	    p++;
	}

      /* Remove additional separators */
      q = p;
      while (*q && *q == '/')
	q++;

      if (p != q)
	memmove (p, q, strlen (q)+1);
    }

  /* Remove trailing slashes */
  if (p > start && *(p-1) == '/')
    *(p-1) = 0;
  
  return canon;
}

GType
g_type_mount_spec_get_gtype (void)
{
  static GType type_id = 0;

  if (type_id == 0)
    type_id = g_boxed_type_register_static (g_intern_static_string ("GMountSpec"),
                                            (GBoxedCopyFunc) g_mount_spec_ref,
                                            (GBoxedFreeFunc) g_mount_spec_unref);
  return type_id;
}
