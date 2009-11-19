#include "metabuilder.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <glib/gstdio.h>

#define MAJOR_VERSION 1
#define MINOR_VERSION 0
#define MAJOR_JOURNAL_VERSION 1
#define MINOR_JOURNAL_VERSION 0
#define NEW_JOURNAL_SIZE (32*1024)

#define RANDOM_TAG_OFFSET 12
#define ROTATED_OFFSET 8

#define KEY_IS_LIST_MASK (1<<31)

MetaBuilder *
meta_builder_new (void)
{
  MetaBuilder *builder;

  builder = g_new0 (MetaBuilder, 1);
  builder->root = metafile_new ("/", NULL);

  return builder;
}

void
meta_builder_free (MetaBuilder *builder)
{
  if (builder->root)
    metafile_free (builder->root);
  g_free (builder);
}

static gint
compare_metafile (gconstpointer  a,
		  gconstpointer  b)
{
  const MetaFile *aa, *bb;

  aa = a;
  bb = b;
  return strcmp (aa->name, bb->name);
}

static gint
compare_metadata (gconstpointer  a,
		  gconstpointer  b)
{
  const MetaData *aa, *bb;

  aa = a;
  bb = b;
  return strcmp (aa->key, bb->key);
}

MetaFile *
metafile_new (const char *name,
	      MetaFile *parent)
{
  MetaFile *f;

  f = g_new0 (MetaFile, 1);
  f->name = g_strdup (name);
  if (parent)
    parent->children = g_list_insert_sorted (parent->children, f,
					     compare_metafile);

  return f;
}

static MetaData *
metadata_new (const char *key,
	      MetaFile *file)
{
  MetaData *data;

  data = g_new0 (MetaData, 1);
  data->key = g_strdup (key);

  if (file)
    file->data = g_list_insert_sorted (file->data, data, compare_metadata);

  return data;
}

static MetaData *
metadata_dup (MetaFile *file,
	      MetaData *data)
{
  MetaData *new_data;
  GList *l;

  new_data = metadata_new (data->key, file);

  new_data->is_list = data->is_list;
  if (data->is_list)
    {
      for (l = data->values; l != NULL; l = l->next)
	new_data->values =
	  g_list_prepend (new_data->values, g_strdup (l->data));
      new_data->values = g_list_reverse (new_data->values);
    }
  else
    new_data->value = g_strdup (data->value);

  return new_data;
}

static void
metadata_free (MetaData *data)
{
  g_free (data->key);
  if (data->is_list)
    {
      g_list_foreach (data->values, (GFunc)g_free, NULL);
      g_list_free (data->values);
    }
  else
    g_free (data->value);

  g_free (data);
}

void
metafile_free (MetaFile *file)
{
  g_free (file->name);
  g_list_foreach (file->children, (GFunc)metafile_free, NULL);
  g_list_free (file->children);
  g_list_foreach (file->data, (GFunc)metadata_free, NULL);
  g_list_free (file->data);
  g_free (file);
}

MetaFile *
metafile_lookup_child (MetaFile *metafile,
		       const char *name,
		       gboolean create)
{
  GList *l;
  MetaFile *child;

  for (l = metafile->children; l != NULL; l = l->next)
    {
      child = l->data;
      if (strcmp (child->name, name) == 0)
	return child;
    }
  child = NULL;
  if (create)
    child = metafile_new (name, metafile);
  return child;
}

static MetaFile *
meta_builder_lookup_with_parent (MetaBuilder *builder,
				 const char *path,
				 gboolean create,
				 MetaFile **parent)
{
  MetaFile *f, *last;
  const char *element_start;
  char *element;

  last = NULL;
  f = builder->root;
  while (f)
    {
      while (*path == '/')
	path++;

      if (*path == 0)
	break; /* Found it! */

      element_start = path;
      while (*path != 0 && *path != '/')
	path++;
      element = g_strndup (element_start, path - element_start);

      last = f;
      f = metafile_lookup_child (f, element, create);
      g_free (element);
    }

  if (parent)
    *parent = last;

  return f;
}


MetaFile *
meta_builder_lookup (MetaBuilder *builder,
		     const char *path,
		     gboolean create)
{
  return meta_builder_lookup_with_parent (builder, path, create, NULL);
}

void
meta_builder_remove (MetaBuilder *builder,
		     const char  *path,
		     guint64 mtime)
{
  MetaFile *f, *parent;

  f = meta_builder_lookup_with_parent (builder, path, FALSE, &parent);

  if (f == NULL)
    return;

  if (parent != NULL)
    {
      parent->children = g_list_remove (parent->children, f);
      metafile_free (f);
      if (mtime)
	parent->last_changed = mtime;
    }
  else
    {
      /* Removing root not allowed, just remove children */
      g_list_foreach (f->children, (GFunc)metafile_free, NULL);
      g_list_free (f->children);
      f->children = NULL;
      if (mtime)
	f->last_changed = mtime;
    }
}


static void
meta_file_copy_into (MetaFile *src,
		     MetaFile *dest,
		     guint64 mtime)
{
  MetaFile *src_child, *dest_child;
  GList *l;

  if (mtime)
    dest->last_changed = mtime;
  else
    dest->last_changed = src->last_changed;

  for (l = src->data; l != NULL; l = l->next)
    metadata_dup (dest, l->data);

  for (l = src->children; l != NULL; l = l->next)
    {
      src_child = l->data;
      dest_child = metafile_new (src_child->name, dest);
      meta_file_copy_into (src_child, dest_child, mtime);
    }
}

void
meta_builder_copy (MetaBuilder *builder,
		   const char  *source_path,
		   const char  *dest_path,
		   guint64      mtime)
{
  MetaFile *src, *dest;

  meta_builder_remove (builder, dest_path, mtime);

  src = meta_builder_lookup (builder, source_path, FALSE);
  if (src == NULL)
    return;

  dest = meta_builder_lookup (builder, dest_path, TRUE);

  meta_file_copy_into (src, dest, mtime);
}

void
metafile_set_mtime (MetaFile    *file,
		    guint64      mtime)
{
  file->last_changed = mtime;
}

MetaData *
metafile_key_lookup (MetaFile *file,
		     const char *key,
		     gboolean create)
{
  GList *l;
  MetaData *data;

  for (l = file->data; l != NULL; l = l->next)
    {
      data = l->data;
      if (strcmp (data->key, key) == 0)
	return data;
    }

  data = NULL;
  if (create)
    data = metadata_new (key, file);

  return data;
}

static void
metadata_clear (MetaData *data)
{
  if (data->is_list)
    {
      g_list_foreach (data->values, (GFunc)g_free, NULL);
      g_list_free (data->values);
      data->values = NULL;
    }
  else
    {
      g_free (data->value);
    }
}

void
metafile_key_unset (MetaFile *metafile,
		    const char *key)
{
  MetaData *data;

  data = metafile_key_lookup (metafile, key, FALSE);
  if (data)
    {
      metafile->data = g_list_remove (metafile->data, data);
      metadata_free (data);
    }
}

void
metafile_key_set_value (MetaFile *metafile,
			const char *key,
			const char *value)
{
  MetaData *data;

  data = metafile_key_lookup (metafile, key, TRUE);
  metadata_clear (data);
  data->is_list = FALSE;
  data->value = g_strdup (value);
}

void
metafile_key_list_set (MetaFile    *metafile,
		       const char  *key)
{
  MetaData *data;

  data = metafile_key_lookup (metafile, key, TRUE);
  if (!data->is_list)
    {
      metadata_clear (data);
      data->is_list = TRUE;
    }
  g_list_foreach (data->values, (GFunc)g_free, NULL);
  g_list_free (data->values);
  data->values = NULL;
}

void
metafile_key_list_add (MetaFile *metafile,
		       const char *key,
		       const char *value)
{
  MetaData *data;

  data = metafile_key_lookup (metafile, key, TRUE);
  if (!data->is_list)
    {
      metadata_clear (data);
      data->is_list = TRUE;
    }

  data->values = g_list_append (data->values, g_strdup (value));
}

static void
metafile_print (MetaFile *file, int indent, char *parent)
{
  GList *l, *v;
  MetaData *data;
  char *dir;

  if (parent)
    dir = g_strconcat (parent, "/", file->name, NULL);
  else
    dir = g_strdup ("");

  if (parent)
    {
      g_print ("%*s%s\n", indent, "", dir);
      indent += 3;
    }

  for (l = file->data; l != NULL; l = l->next)
    {
      data = l->data;
      g_print ("%*s%s=", indent, "", data->key);
      if (data->is_list)
	{
	  for (v = data->values; v != NULL; v = v->next)
	    {
	      g_print ("%s", (char *)v->data);
	      if (v->next != NULL)
		g_print (", ");
	    }
	}
      else
	g_print ("%s", data->value);
      g_print ("\n");
    }
  for (l = file->children; l != NULL; l = l->next)
    {
      metafile_print (l->data, indent, dir);
    }

  g_free (dir);
}

void
meta_builder_print (MetaBuilder *builder)
{
  metafile_print (builder->root, 0, NULL);
}

static void
set_uint32 (GString *s, guint32 offset, guint32 val)
{
  union {
    guint32 as_int;
    char as_bytes[4];
  } u;

  u.as_int = GUINT32_TO_BE (val);
  memcpy (s->str + offset, u.as_bytes, 4);
}

static GString *
append_uint32 (GString *s, guint32 val, guint32 *offset)
{
  union {
    guint32 as_int;
    char as_bytes[4];
  } u;

  if (offset)
    *offset = s->len;

  u.as_int = GUINT32_TO_BE (val);

  g_string_append_len (s, u.as_bytes, 4);

  return s;
}

static GString *
append_time_t (GString *s, gint64 val, MetaBuilder *builder)
{
  guint32 offset;

  if (val == 0)
    offset = 0;
  else if (val <= builder->time_t_base)
    offset = 1;
  else
    offset = val - builder->time_t_base;

  return append_uint32 (s, offset, NULL);
}

static GString *
append_int64 (GString *s, gint64 val)
{
  union {
    gint64 as_int;
    char as_bytes[8];
  } u;

  u.as_int = GINT64_TO_BE (val);

  g_string_append_len (s, u.as_bytes, 8);

  return s;
}

static void
metafile_collect_times (MetaFile *file,
			gint64 *time_t_min,
			gint64 *time_t_max)
{
  GList *l;
  MetaFile *child;

  if (*time_t_min == 0)
    *time_t_min = file->last_changed;
  else if (file->last_changed != 0 && file->last_changed < *time_t_min)
    *time_t_min = file->last_changed;

  if (file->last_changed > *time_t_max)
    *time_t_max = file->last_changed;

  for (l = file->children; l != NULL; l = l->next)
    {
      child = l->data;
      metafile_collect_times (child, time_t_min, time_t_max);
    }
}

static void
metafile_collect_keywords (MetaFile *file,
			   GHashTable *hash)
{
  GList *l;
  MetaData *data;
  MetaFile *child;

  file->metadata_pointer = 0;
  file->children_pointer = 0;

  for (l = file->data; l != NULL; l = l->next)
    {
      data = l->data;
      g_hash_table_insert (hash, data->key, GINT_TO_POINTER (1));
    }

  for (l = file->children; l != NULL; l = l->next)
    {
      child = l->data;
      metafile_collect_keywords (child, hash);
    }
}

static GHashTable *
string_block_begin (void)
{
  return g_hash_table_new (g_str_hash, g_str_equal);
}

static void
append_string (GString *out,
	       const char *string,
	       GHashTable *string_block)
{
  guint32 offset;
  GList *offsets;

  append_uint32 (out, 0xdeaddead, &offset);

  if (g_hash_table_lookup_extended (string_block,
				    string, NULL,
				    (gpointer *)&offsets))
    {
      offsets = g_list_append (offsets, GUINT_TO_POINTER (offset));
    }
  else
    {
      g_hash_table_insert (string_block,
			   (char *)string,
			   g_list_prepend (NULL, GUINT_TO_POINTER (offset)));
    }
}

static void
string_block_end (GString *out,
		  GHashTable *string_block)
{
  char *string;
  GList *offsets, *l;
  guint32 string_offset, offset;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, string_block);
  while (g_hash_table_iter_next (&iter,
				 (gpointer *)&string,
				 (gpointer *)&offsets))
    {
      string_offset = out->len;
      g_string_append_len (out, string, strlen (string) + 1);
      for (l = offsets; l != NULL; l = l->next)
	{
	  offset = GPOINTER_TO_UINT (l->data);
	  set_uint32 (out, offset, string_offset);
	}
      g_list_free (offsets);
    }

  g_hash_table_destroy (string_block);

  /* Pad to 32bit */
  while (out->len % 4 != 0)
    g_string_append_c (out, 0);
}


static GList *
stringv_block_begin (void)
{
  return NULL;
}


typedef struct {
  guint32 offset;
  GList *strings;
} StringvInfo;

static void
append_stringv (GString *out,
		GList *strings,
		GList **stringv_block)
{
  guint32 offset;
  StringvInfo *info;

  append_uint32 (out, 0xdeaddead, &offset);

  info = g_new (StringvInfo, 1);
  info->offset = offset;
  info->strings = strings;

  *stringv_block = g_list_prepend (*stringv_block, info);
}

static void
stringv_block_end (GString *out,
		   GHashTable *string_block,
		   GList *stringv_block)
{
  guint32 table_offset;
  StringvInfo *info;
  GList *l, *s;


  for (l = stringv_block; l != NULL; l = l->next)
    {
      info = l->data;

      table_offset = out->len;

      append_uint32 (out, g_list_length (info->strings), NULL);
      for (s = info->strings; s != NULL; s = s->next)
	append_string (out, s->data, string_block);

      set_uint32 (out, info->offset, table_offset);

      g_free (info);
    }

  g_list_free (stringv_block);

  /* Pad to 32bit */
  while (out->len % 4 != 0)
    g_string_append_c (out, 0);
}

static void
write_children (GString *out,
		MetaBuilder *builder)
{
  GHashTable *strings;
  MetaFile *child, *file;
  GList *l;
  GList *files;

  files = g_list_prepend (NULL, builder->root);

  while (files != NULL)
    {
      file = files->data;
      files = g_list_delete_link (files, files);

      if (file->children == NULL)
	continue; /* No children, skip file */

      strings = string_block_begin ();

      if (file->children_pointer != 0)
	set_uint32 (out, file->children_pointer, out->len);

      append_uint32 (out, g_list_length (file->children), NULL);

      for (l = file->children; l != NULL; l = l->next)
	{
	  child = l->data;

	  /* No mtime, children or metadata, no need for this
	     to be in the file */
	  if (child->last_changed == 0 &&
	      child->children == NULL &&
	      child->data == NULL)
	    continue;

	  append_string (out, child->name, strings);
	  append_uint32 (out, 0, &child->children_pointer);
	  append_uint32 (out, 0, &child->metadata_pointer);
	  append_time_t (out, child->last_changed, builder);

	  if (file->children)
	    files = g_list_append (files, child);
	}

      string_block_end (out, strings);
    }
}

static void
write_metadata_for_file (GString *out,
			 MetaFile *file,
			 GList **stringvs,
			 GHashTable *strings,
			 GHashTable *key_hash)
{
  GList *l;
  MetaData *data;
  guint32 key;

  g_assert (file->metadata_pointer != 0);
  set_uint32 (out, file->metadata_pointer, out->len);

  append_uint32 (out, g_list_length (file->data), NULL);

  for (l = file->data; l != NULL; l = l->next)
    {
      data = l->data;

      key = GPOINTER_TO_UINT (g_hash_table_lookup (key_hash, data->key));
      if (data->is_list)
	key |= KEY_IS_LIST_MASK;
      append_uint32 (out, key, NULL);
      if (data->is_list)
	append_stringv (out, data->values, stringvs);
      else
	append_string (out, data->value, strings);
    }
}

static void
write_metadata (GString *out,
		MetaBuilder *builder,
		GHashTable *key_hash)
{
  GHashTable *strings;
  GList *stringvs;
  MetaFile *child, *file;
  GList *l;
  GList *files;

  /* Root metadata */
  if (builder->root->data != NULL)
    {
      strings = string_block_begin ();
      stringvs = stringv_block_begin ();
      write_metadata_for_file (out, builder->root,
			       &stringvs, strings, key_hash);
      stringv_block_end (out, strings, stringvs);
      string_block_end (out, strings);
    }

  /* the rest, breadth first with all files in one
     dir sharing string block */
  files = g_list_prepend (NULL, builder->root);
  while (files != NULL)
    {
      file = files->data;
      files = g_list_delete_link (files, files);

      if (file->children == NULL)
	continue; /* No children, skip file */

      strings = string_block_begin ();
      stringvs = stringv_block_begin ();

      for (l = file->children; l != NULL; l = l->next)
	{
	  child = l->data;

	  if (child->data != NULL)
	    write_metadata_for_file (out, child,
				     &stringvs, strings, key_hash);

	  if (child->children != NULL)
	    files = g_list_append (files, child);
	}

      stringv_block_end (out, strings, stringvs);
      string_block_end (out, strings);
    }
}

static gboolean
write_all_data_and_close (int fd, char *data, gsize len)
{
  gssize written;
  gboolean res;

  res = FALSE;

  while (len > 0)
    {
      written = write (fd, data, len);

      if (written < 0)
	{
	  if (errno == EAGAIN)
	    continue;
	  goto out;
	}
      else if (written == 0)
	goto out; /* WTH? Don't loop forever*/

      len -= written;
      data += written;
    }

  if (fsync (fd) == -1)
    goto out;

  res = TRUE; /* Succeeded! */

 out:
  if (close (fd) == -1)
    res = FALSE;

  return res;
}

static char *
get_journal_filename (const char *filename, guint32 random_tag)
{
  const char *hexdigits = "0123456789abcdef";
  char tag[9];
  int i;

  for (i = 7; i >= 0; i--)
    {
      tag[i] = hexdigits[random_tag % 0x10];
      random_tag >>= 4;
    }

  tag[8] = 0;

  return g_strconcat (filename, "-", tag, ".log", NULL);
}

static gboolean
create_new_journal (const char *filename, guint32 random_tag)
{
  char *journal_name;
  guint32 size_offset;
  GString *out;
  gsize pos;
  gboolean res;

  journal_name = get_journal_filename (filename, random_tag);

  out = g_string_new (NULL);

  /* HEADER */
  g_string_append_c (out, 0xda);
  g_string_append_c (out, 0x1a);
  g_string_append_c (out, 'j');
  g_string_append_c (out, 'o');
  g_string_append_c (out, 'u');
  g_string_append_c (out, 'r');

  /* VERSION */
  g_string_append_c (out, MAJOR_JOURNAL_VERSION);
  g_string_append_c (out, MINOR_JOURNAL_VERSION);

  append_uint32 (out, random_tag, NULL);
  append_uint32 (out, 0, &size_offset);
  append_uint32 (out, 0, NULL); /* Num entries, none so far */

  pos = out->len;

  g_string_set_size (out, NEW_JOURNAL_SIZE);
  memset (out->str + pos, 0, out->len - pos);

  set_uint32 (out, size_offset, out->len);

  res = g_file_set_contents (journal_name,
			     out->str, out->len,
			     NULL);

  g_free (journal_name);
  g_string_free (out, TRUE);

  return res;
}

static GString *
metadata_create_static (MetaBuilder *builder,
			guint32 *random_tag_out)
{
  GString *out;
  GHashTable *hash, *key_hash;
  GHashTableIter iter;
  char *key;
  GList *keys, *l;
  GHashTable *strings;
  guint32 index;
  guint32 attributes_pointer;
  gint64 time_t_min;
  gint64 time_t_max;
  guint32 random_tag, root_name;

  out = g_string_new (NULL);

  /* HEADER */
  g_string_append_c (out, 0xda);
  g_string_append_c (out, 0x1a);
  g_string_append_c (out, 'm');
  g_string_append_c (out, 'e');
  g_string_append_c (out, 't');
  g_string_append_c (out, 'a');

  /* VERSION */
  g_string_append_c (out, MAJOR_VERSION);
  g_string_append_c (out, MINOR_VERSION);

  append_uint32 (out, 0, NULL); /* Rotated */
  random_tag = g_random_int ();
  *random_tag_out = random_tag;
  append_uint32 (out, random_tag, NULL);
  append_uint32 (out, 0, &builder->root_pointer);
  append_uint32 (out, 0, &attributes_pointer);

  time_t_min = 0;
  time_t_max = 0;
  metafile_collect_times (builder->root, &time_t_min, &time_t_max);

  /* Store the base as the min value in use minus one so that
     0 is free to mean "not defined" */
  if (time_t_min != 0)
    time_t_min = time_t_min - 1;

  /* Pick the base as the minimum, unless that leads to
     a 32bit overflow */
  if (time_t_max - time_t_min > G_MAXUINT32)
    time_t_min = time_t_max - G_MAXUINT32;
  builder->time_t_base = time_t_min;
  append_int64 (out, builder->time_t_base);

  /* Collect and sort all used keys */
  hash = g_hash_table_new (g_str_hash, g_str_equal);
  metafile_collect_keywords (builder->root, hash);
  g_hash_table_iter_init (&iter, hash);
  keys = NULL;
  while (g_hash_table_iter_next (&iter, (gpointer *)&key, NULL))
    keys = g_list_prepend (keys, key);
  g_hash_table_destroy (hash);
  keys = g_list_sort (keys, (GCompareFunc)strcmp);

  /* Write keys to file and collect mapping for keys */
  set_uint32 (out, attributes_pointer, out->len);
  key_hash = g_hash_table_new (g_str_hash, g_str_equal);
  strings = string_block_begin ();
  append_uint32 (out, g_list_length (keys), NULL);
  for (l = keys, index = 0; l != NULL; l = l->next, index++)
    {
      key = l->data;
      append_string (out, key, strings);
      g_hash_table_insert (key_hash, key, GUINT_TO_POINTER (index));
    }
  string_block_end (out, strings);

  /* update root pointer */
  set_uint32 (out, builder->root_pointer, out->len);

  /* Root name */
  append_uint32 (out, 0, &root_name);

  /* Root child pointer */
  append_uint32 (out, 0, &builder->root->children_pointer);

  /* Root metadata pointer */
  append_uint32 (out, 0, &builder->root->metadata_pointer);

  /* Root last changed */
  append_uint32 (out, builder->root->last_changed, NULL);

  /* Root name */
  set_uint32 (out, root_name, out->len);
  g_string_append_len (out, "/", 2);

  /* Pad to 32bit */
  while (out->len % 4 != 0)
    g_string_append_c (out, 0);

  write_children (out, builder);
  write_metadata (out, builder, key_hash);

  g_hash_table_destroy (key_hash);
  g_list_free (keys);

  return out;
}

gboolean
meta_builder_write (MetaBuilder *builder,
		    const char *filename)
{
  GString *out;
  guint32 random_tag;
  int fd, fd2, fd_dir;
  char *tmp_name, *dirname;

  out = metadata_create_static (builder, &random_tag);

  tmp_name = g_strdup_printf ("%s.XXXXXX", filename);
  fd = g_mkstemp (tmp_name);
  if (fd == -1)
    goto out;

  if (!write_all_data_and_close (fd, out->str, out->len))
    goto out;

  if (!create_new_journal (filename, random_tag))
    goto out;

  /* Open old file so we can set it rotated */
  fd2 = open (filename, O_RDWR);
  if (g_rename (tmp_name, filename) == -1)
    {
      if (fd2 != -1)
	close (fd2);
      goto out;
    }

  /* Sync the directory to make sure that the entry in the directory containing
     the new medata file has also reached disk. */
  dirname = g_path_get_dirname (filename);
  fd_dir = open (dirname, O_RDONLY);
  if (fd_dir > -1)
    {
      fsync (fd_dir);
      close (fd_dir);
    }
  g_free (dirname);

  /* Mark old file (if any) as rotated) */
  if (fd2 != -1)
    {
      guint32 old_tag;
      char *old_log;
      char *data;

      data = mmap (NULL, RANDOM_TAG_OFFSET + 4, PROT_READ|PROT_WRITE, MAP_SHARED, fd2, 0);

      if (data)
	{
	  old_tag = GUINT32_FROM_BE (*(guint32 *)(data + RANDOM_TAG_OFFSET));
	  *(guint32 *)(data + ROTATED_OFFSET) = 0xffffffff;
	  munmap (data, RANDOM_TAG_OFFSET + 4);
	  close (fd2);

	  old_log = get_journal_filename (filename, old_tag);
	  g_unlink (old_log);
	  g_free (old_log);
	}
    }

  g_string_free (out, TRUE);
  g_free (tmp_name);
  return TRUE;

 out:
  if (fd != -1)
    g_unlink (tmp_name);
  g_string_free (out, TRUE);
  g_free (tmp_name);
  return FALSE;
}
