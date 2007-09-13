#include <config.h>

#include <string.h>
#include <stdlib.h>

#include <glib/gi18n-lib.h>

#include "gdbusutils.h"
#include "gmountspec.h"

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

  if (type != NULL)
    g_mount_spec_set (spec, "type", type);
  
  return spec;
}

static void 
add_item (GMountSpec *spec,
	  const char *key,
	  char *value)
{
  GMountSpecItem item;

  item.key = g_strdup (key);
  item.value = value;

  g_array_append_val (spec->items, item);
}


void 
g_mount_spec_set_with_len (GMountSpec *spec,
			   const char *key,
			   const char *value,
			   int value_len)
{
  int i;
  char *value_copy;
  
  for (i = 0; i < spec->items->len; i++)
    {
      GMountSpecItem *item = &g_array_index (spec->items, GMountSpecItem, i);
      if (strcmp (item->key, key) == 0)
	{
	  g_free (item->value);
	  item->value = g_strdup (value);
	  return;
	}
    }

  if (value_len == -1)
    value_copy = g_strdup (value);
  else
    value_copy = g_strndup (value, value_len);
  add_item (spec, key, value_copy);
  g_array_sort (spec->items, item_compare);
}

void 
g_mount_spec_set (GMountSpec *spec,
		  const char *key,
		  const char *value)
{
  g_mount_spec_set_with_len (spec, key, value, -1);
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
g_mount_spec_from_dbus (DBusMessageIter *iter)
{
  GMountSpec *spec;
  DBusMessageIter array_iter, struct_iter, spec_iter;
  const char *key;
  char *value;
  char *mount_prefix;

  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_STRUCT)
    return NULL;

  dbus_message_iter_recurse (iter, &spec_iter);

  mount_prefix = NULL;
  if (!_g_dbus_message_iter_get_args (&spec_iter, NULL,
				      G_DBUS_TYPE_CSTRING, &mount_prefix,
				      0))
    return NULL;

  spec = g_mount_spec_new (NULL);
  spec->mount_prefix = mount_prefix;
  
  if (dbus_message_iter_get_arg_type (&spec_iter) != DBUS_TYPE_ARRAY ||
      dbus_message_iter_get_element_type (&spec_iter) != DBUS_TYPE_STRUCT)
    {
      g_mount_spec_unref (spec);
      return NULL;
    }

  dbus_message_iter_recurse (&spec_iter, &array_iter);
  while (dbus_message_iter_get_arg_type (&array_iter) == DBUS_TYPE_STRUCT)
    {
      dbus_message_iter_recurse (&array_iter, &struct_iter);
      if (_g_dbus_message_iter_get_args (&struct_iter, NULL,
					 DBUS_TYPE_STRING, &key,
					 G_DBUS_TYPE_CSTRING, &value,
					 0))
	add_item (spec, key, value);
      dbus_message_iter_next (&array_iter);
    }

  dbus_message_iter_next (iter);
  
  /* Sort on key */
  g_array_sort (spec->items, item_compare);
  
  return spec;
}

void
g_mount_spec_to_dbus_with_path (DBusMessageIter *iter,
				GMountSpec *spec,
				const char *path)
{
  DBusMessageIter spec_iter, array_iter, item_iter;
  int i;

  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_STRUCT,
					 G_MOUNT_SPEC_INNER_TYPE_AS_STRING,
					 &spec_iter))
    _g_dbus_oom ();

  _g_dbus_message_iter_append_cstring (&spec_iter, path ? path : "");

  if (!dbus_message_iter_open_container (&spec_iter,
					 DBUS_TYPE_ARRAY,
 					 G_MOUNT_SPEC_ITEM_TYPE_AS_STRING,
					 &array_iter))
    _g_dbus_oom ();

  for (i = 0; i < spec->items->len; i++)
    {
      GMountSpecItem *item = &g_array_index (spec->items, GMountSpecItem, i);

      if (!dbus_message_iter_open_container (&array_iter,
					     DBUS_TYPE_STRUCT,
					     G_MOUNT_SPEC_ITEM_INNER_TYPE_AS_STRING,
					     &item_iter))
	_g_dbus_oom ();

      if (!dbus_message_iter_append_basic (&item_iter, DBUS_TYPE_STRING,
					   &item->key))
	_g_dbus_oom ();
      _g_dbus_message_iter_append_cstring  (&item_iter, item->value);
      
      if (!dbus_message_iter_close_container (&array_iter, &item_iter))
	_g_dbus_oom ();
      
    }
  
  if (!dbus_message_iter_close_container (&spec_iter, &array_iter))
    _g_dbus_oom ();
  
  
  
  if (!dbus_message_iter_close_container (iter, &spec_iter))
    _g_dbus_oom ();
    
}

void
g_mount_spec_to_dbus (DBusMessageIter *iter,
		      GMountSpec      *spec)
{
  g_mount_spec_to_dbus_with_path (iter, spec, spec->mount_prefix);
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
      (path[prefix_len] == 0 ||
       path[prefix_len] == '/'))
    return TRUE;
  
  return FALSE;
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
 
