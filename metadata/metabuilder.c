#include "config.h"
#include "metabuilder.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <glib/gstdio.h>

#if HAVE_SYS_STATFS_H
#include <sys/statfs.h>
#endif
#if HAVE_SYS_STATVFS_H
#include <sys/statvfs.h>
#endif
#if HAVE_SYS_VFS_H
#include <sys/vfs.h>
#elif HAVE_SYS_MOUNT_H
#if HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <sys/mount.h>
#endif

#if defined(HAVE_STATFS) && defined(HAVE_STATVFS)
/* Some systems have both statfs and statvfs, pick the
   most "native" for these */
# if !defined(HAVE_STRUCT_STATFS_F_BAVAIL)
   /* on solaris and irix, statfs doesn't even have the
      f_bavail field */
#  define USE_STATVFS
# else
  /* at least on linux, statfs is the actual syscall */
#  define USE_STATFS
# endif

#elif defined(HAVE_STATFS)

# define USE_STATFS

#elif defined(HAVE_STATVFS)

# define USE_STATVFS

#endif


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
compare_metafile (gconstpointer a,
                  gconstpointer b,
                  gpointer      user_data)
{
  const MetaFile *aa, *bb;

  aa = a;
  bb = b;
  return strcmp (aa->name, bb->name);
}

static gint
compare_metadata (gconstpointer a,
                  gconstpointer b,
                  gpointer      user_data)
{
  const MetaData *aa, *bb;

  aa = a;
  bb = b;
  return strcmp (aa->key, bb->key);
}

static void
metadata_free (MetaData *data)
{
  g_free (data->key);
  if (data->is_list)
    g_list_free_full (data->values, g_free);
  else
    g_free (data->value);

  g_free (data);
}

MetaFile *
metafile_new (const char *name,
	      MetaFile *parent)
{
  MetaFile *f;

  f = g_new0 (MetaFile, 1);
  f->name = g_strdup (name);
  f->children = g_sequence_new ((GDestroyNotify)metafile_free);
  f->data = g_sequence_new ((GDestroyNotify)metadata_free);
  if (parent)
    g_sequence_insert_sorted (parent->children, f, compare_metafile, NULL);

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
    g_sequence_insert_sorted (file->data, data, compare_metadata, NULL);

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

void
metafile_free (MetaFile *file)
{
  g_free (file->name);
  g_sequence_free (file->children);
  g_sequence_free (file->data);
  g_free (file);
}

MetaFile *
metafile_lookup_child (MetaFile *metafile,
		       const char *name,
		       gboolean create)
{
  MetaFile *child;
  MetaFile lookup_file;
  GSequenceIter *lookup_file_iter;

  lookup_file.name = (char *)name;

  lookup_file_iter = g_sequence_lookup (metafile->children,
                                        &lookup_file,
                                        compare_metafile,
                                        NULL);

  if (lookup_file_iter)
    return g_sequence_get (lookup_file_iter);

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
      GSequenceIter *iter;

      iter = g_sequence_lookup (parent->children,
                                f,
                                compare_metafile,
                                NULL);
      g_sequence_remove (iter);

      if (mtime)
	parent->last_changed = mtime;
    }
  else
    {
      /* Removing root not allowed, just remove children */
      g_sequence_remove_range (g_sequence_get_begin_iter (f->children),
                               g_sequence_get_end_iter (f->children));
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
  GSequenceIter *iter;

  if (mtime)
    dest->last_changed = mtime;
  else
    dest->last_changed = src->last_changed;

  for (iter = g_sequence_get_begin_iter (src->data);
       iter != g_sequence_get_end_iter (src->data);
       iter = g_sequence_iter_next (iter))
    metadata_dup (dest, g_sequence_get (iter));

  for (iter = g_sequence_get_begin_iter (src->children);
       iter != g_sequence_get_end_iter (src->children);
       iter = g_sequence_iter_next (iter))
    {
      src_child = g_sequence_get (iter);
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
  MetaFile *src, *dest, *temp;

  meta_builder_remove (builder, dest_path, mtime);

  src = meta_builder_lookup (builder, source_path, FALSE);
  if (src == NULL)
    return;

  temp = metafile_new (NULL, NULL);
  meta_file_copy_into (src, temp, mtime);

  dest = meta_builder_lookup (builder, dest_path, TRUE);
  g_sequence_free (dest->data);
  g_sequence_free (dest->children);
  dest->data = temp->data;
  dest->children = temp->children;
  dest->last_changed = temp->last_changed;

  g_free (temp);
}

void
metafile_set_mtime (MetaFile    *file,
		    guint64      mtime)
{
  file->last_changed = mtime;
}

static GSequenceIter *
metafile_key_lookup_iter (MetaFile *file,
                          const char *key)
{
  MetaData lookup_data;

  lookup_data.key = (char *)key;

  return g_sequence_lookup (file->data,
                            &lookup_data,
                            compare_metadata,
                            NULL);
}

MetaData *
metafile_key_lookup (MetaFile *file,
		     const char *key,
		     gboolean create)
{
  MetaData *data;
  GSequenceIter *iter;

  iter = metafile_key_lookup_iter (file, key);
  if (iter)
    return g_sequence_get (iter);

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
      g_list_free_full (data->values, g_free);
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
  GSequenceIter *iter;

  iter = metafile_key_lookup_iter (metafile, key);
  if (iter)
    g_sequence_remove (iter);
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
  g_list_free_full (data->values, g_free);
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
  GSequenceIter *iter;
  GList *v;
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

  for (iter = g_sequence_get_begin_iter (file->data);
       iter != g_sequence_get_end_iter (file->data);
       iter = g_sequence_iter_next (iter))
    {
      data = g_sequence_get (iter);
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
  for (iter = g_sequence_get_begin_iter (file->children);
       iter != g_sequence_get_end_iter (file->children);
       iter = g_sequence_iter_next (iter))
    {
      metafile_print (g_sequence_get (iter), indent, dir);
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
  GSequenceIter *iter;
  MetaFile *child;

  if (*time_t_min == 0)
    *time_t_min = file->last_changed;
  else if (file->last_changed != 0 && file->last_changed < *time_t_min)
    *time_t_min = file->last_changed;

  if (file->last_changed > *time_t_max)
    *time_t_max = file->last_changed;

  for (iter = g_sequence_get_begin_iter (file->children);
       iter != g_sequence_get_end_iter (file->children);
       iter = g_sequence_iter_next (iter))
    {
      child = g_sequence_get (iter);
      metafile_collect_times (child, time_t_min, time_t_max);
    }
}

static void
metafile_collect_keywords (MetaFile *file,
			   GHashTable *hash)
{
  GSequenceIter *iter;
  MetaData *data;
  MetaFile *child;

  file->metadata_pointer = 0;
  file->children_pointer = 0;

  for (iter = g_sequence_get_begin_iter (file->data);
       iter != g_sequence_get_end_iter (file->data);
       iter = g_sequence_iter_next (iter))
    {
      data = g_sequence_get (iter);
      g_hash_table_insert (hash, data->key, GINT_TO_POINTER (1));
    }

  for (iter = g_sequence_get_begin_iter (file->children);
       iter != g_sequence_get_end_iter (file->children);
       iter = g_sequence_iter_next (iter))
    {
      child = g_sequence_get (iter);
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
  GQueue *offsets;

  append_uint32 (out, 0xdeaddead, &offset);

  if (!g_hash_table_lookup_extended (string_block,
                                     string, NULL,
                                     (gpointer *)&offsets))
    {
      offsets = g_queue_new ();

      g_hash_table_insert (string_block,
                           (char *)string,
                           offsets);
    }

  g_queue_push_tail (offsets, GUINT_TO_POINTER (offset));
}

static void
string_block_end (GString *out,
		  GHashTable *string_block)
{
  char *string;
  GQueue *offsets;
  GList *l;
  guint32 string_offset, offset;
  GHashTableIter iter;

  g_hash_table_iter_init (&iter, string_block);
  while (g_hash_table_iter_next (&iter,
				 (gpointer *)&string,
				 (gpointer *)&offsets))
    {
      string_offset = out->len;
      g_string_append_len (out, string, strlen (string) + 1);
      for (l = g_queue_peek_head_link (offsets); l != NULL; l = l->next)
	{
	  offset = GPOINTER_TO_UINT (l->data);
	  set_uint32 (out, offset, string_offset);
	}
      g_queue_free (offsets);
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
  GSequenceIter *iter;
  GQueue *files;

  files = g_queue_new ();

  g_queue_push_tail (files, builder->root);

  while (!g_queue_is_empty (files))
    {
      file = g_queue_pop_head (files);

      if (file->children == NULL)
	continue; /* No children, skip file */

      strings = string_block_begin ();

      if (file->children_pointer != 0)
	set_uint32 (out, file->children_pointer, out->len);

      append_uint32 (out, g_sequence_get_length (file->children), NULL);

      for (iter = g_sequence_get_begin_iter (file->children);
           iter != g_sequence_get_end_iter (file->children);
           iter = g_sequence_iter_next (iter))
	{
	  child = g_sequence_get (iter);

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

          if (child->children)
            g_queue_push_tail (files, child);
        }

      string_block_end (out, strings);
    }

  g_queue_free (files);
}

static void
write_metadata_for_file (GString *out,
			 MetaFile *file,
			 GList **stringvs,
			 GHashTable *strings,
			 GHashTable *key_hash)
{
  GSequenceIter *iter;
  MetaData *data;
  guint32 key;

  g_assert (file->metadata_pointer != 0);
  set_uint32 (out, file->metadata_pointer, out->len);

  append_uint32 (out, g_sequence_get_length (file->data), NULL);

  for (iter = g_sequence_get_begin_iter (file->data);
       iter != g_sequence_get_end_iter (file->data);
       iter = g_sequence_iter_next (iter))
    {
      data = g_sequence_get (iter);

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
  GSequenceIter *iter;
  GQueue *files;

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
  files = g_queue_new ();

  g_queue_push_tail (files, builder->root);

  while (!g_queue_is_empty (files))
    {
      file = g_queue_pop_head (files);

      if (file->children == NULL)
	continue; /* No children, skip file */

      strings = string_block_begin ();
      stringvs = stringv_block_begin ();

      for (iter = g_sequence_get_begin_iter (file->children);
           iter != g_sequence_get_end_iter (file->children);
           iter = g_sequence_iter_next (iter))
	{
	  child = g_sequence_get (iter);

	  if (child->data != NULL)
	    write_metadata_for_file (out, child,
				     &stringvs, strings, key_hash);

	  if (child->children != NULL)
	    g_queue_push_tail (files, child);
	}

      stringv_block_end (out, strings, stringvs);
      string_block_end (out, strings);
    }

  g_queue_free (files);
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

gboolean
meta_builder_is_on_nfs (const char *filename)
{
#ifdef USE_STATFS
  struct statfs statfs_buffer;
  int statfs_result;
#elif defined(USE_STATVFS) && defined(HAVE_STRUCT_STATVFS_F_BASETYPE)
  struct statvfs statfs_buffer;
  int statfs_result;
#endif
  char *dirname;
  gboolean res;

  dirname = g_path_get_dirname (filename);

  res = FALSE;

#ifdef USE_STATFS

# if STATFS_ARGS == 2
  statfs_result = statfs (dirname, &statfs_buffer);
# elif STATFS_ARGS == 4
  statfs_result = statfs (dirname, &statfs_buffer,
                          sizeof (statfs_buffer), 0);
# endif
  if (statfs_result == 0)
#ifdef __OpenBSD__
    res = strcmp(statfs_buffer.f_fstypename, MOUNT_NFS) == 0;
#else
    res = statfs_buffer.f_type == 0x6969;
#endif

#elif defined(USE_STATVFS) && defined(HAVE_STRUCT_STATVFS_F_BASETYPE)
  statfs_result = statvfs (dirname, &statfs_buffer);

  if (statfs_result == 0)
    res = strcmp (statfs_buffer.f_basetype, "nfs") == 0;
#endif

  g_free (dirname);

  return res;
}

static char *
get_runtime_journal_dir (const char *tree_filename)
{
  const char *rd;
  char *dbname;
  char *real_path;
  char *ret;

  rd = g_get_user_runtime_dir ();
  if (! rd || *rd == '\0')
    return NULL;

  real_path = g_build_filename (rd, "gvfs-metadata", NULL);
  if (! g_file_test (real_path, G_FILE_TEST_EXISTS))
    {
      if (g_mkdir_with_parents (real_path, 0700) != 0)
        {
          g_free (real_path);
          return NULL;
        }
    }

  dbname = g_path_get_basename (tree_filename);
  ret = g_build_filename (real_path, dbname, NULL);

  g_free (dbname);
  g_free (real_path);

  return ret;
}

char *
meta_builder_get_journal_filename (const char *tree_filename, guint32 random_tag)
{
  const char *hexdigits = "0123456789abcdef";
  char tag[9];
  int i;
  char *ret;
  char *real_filename = NULL;

  for (i = 7; i >= 0; i--)
    {
      tag[i] = hexdigits[random_tag % 0x10];
      random_tag >>= 4;
    }

  tag[8] = 0;

  if (meta_builder_is_on_nfs (tree_filename))
    {
      /* Put the journal in $XDG_RUNTIME_DIR to avoid file usage from concurrent clients */
      real_filename = get_runtime_journal_dir (tree_filename);
    }

  if (! real_filename)
    return g_strconcat (tree_filename, "-", tag, ".log", NULL);

  ret = g_strconcat (real_filename, "-", tag, ".log", NULL);
  g_free (real_filename);
  return ret;
}

gboolean
meta_builder_create_new_journal (const char *filename, guint32 random_tag)
{
  char *journal_name;
  guint32 size_offset;
  GString *out;
  gsize pos;
  gboolean res;

  journal_name = meta_builder_get_journal_filename (filename, random_tag);

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

  /* Ensure journal file has secure permissions consistent with tree files */
  if (res && chmod (journal_name, 0600) != 0)
    {
      g_warning ("Failed to set permissions on journal file %s: %s",
                 journal_name, g_strerror (errno));
    }

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

  if (!meta_builder_create_new_journal (filename, random_tag))
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

      if (data != MAP_FAILED)
	{
	  old_tag = GUINT32_FROM_BE (*(guint32 *)(data + RANDOM_TAG_OFFSET));
	  *(guint32 *)(data + ROTATED_OFFSET) = 0xffffffff;
	  munmap (data, RANDOM_TAG_OFFSET + 4);
	  close (fd2);

	  old_log = meta_builder_get_journal_filename (filename, old_tag);
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
