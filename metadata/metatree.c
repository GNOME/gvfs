#include "config.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

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

#include "metatree.h"
#include "metabuilder.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include "crc32.h"

#ifdef HAVE_LIBUDEV
#define LIBUDEV_I_KNOW_THE_API_IS_SUBJECT_TO_CHANGE
#include <libudev.h>
#endif

#define MAGIC "\xda\x1ameta"
#define MAGIC_LEN 6
#define MAJOR_VERSION 1
#define MINOR_VERSION 0
#define JOURNAL_MAGIC "\xda\x1ajour"
#define JOURNAL_MAGIC_LEN 6
#define JOURNAL_MAJOR_VERSION 1
#define JOURNAL_MINOR_VERSION 0

#define KEY_IS_LIST_MASK (1<<31)

static GStaticRWLock metatree_lock = G_STATIC_RW_LOCK_INIT;

typedef enum {
  JOURNAL_OP_SET_KEY,
  JOURNAL_OP_SETV_KEY,
  JOURNAL_OP_UNSET_KEY,
  JOURNAL_OP_COPY_PATH,
  JOURNAL_OP_REMOVE_PATH
} MetaJournalEntryType;

typedef struct {
  guchar magic[6];
  guchar major;
  guchar minor;
  guint32 rotated;
  guint32 random_tag;
  guint32 root;
  guint32 attributes;
  guint64 time_t_base;
} MetaFileHeader;

typedef struct {
  guint32 name;
  guint32 children;
  guint32 metadata;
  guint32 last_changed;
} MetaFileDirEnt;

typedef struct {
  guint32 num_children;
  MetaFileDirEnt children[1];
} MetaFileDir;

typedef struct {
  guint32 num_strings;
  guint32 strings[1];
} MetaFileStringv;

typedef struct {
  guint32 key;
  guint32 value;
} MetaFileDataEnt;

typedef struct {
  guint32 num_keys;
  MetaFileDataEnt keys[1];
} MetaFileData;

typedef struct {
  guchar magic[6];
  guchar major;
  guchar minor;
  guint32 random_tag;
  guint32 file_size;
  guint32 num_entries;
} MetaJournalHeader;

typedef struct {
  guint32 entry_size;
  guint32 crc32;
  guint64 mtime;
  guint8 entry_type;
  char path[1];
} MetaJournalEntry;

typedef struct {
  char *filename;
  int fd;
  char *data;
  gsize len;

  MetaJournalHeader *header;
  MetaJournalEntry *first_entry;
  guint last_entry_num;
  MetaJournalEntry *last_entry;

  gboolean journal_valid; /* True if all entries validated on open */
} MetaJournal;

struct _MetaTree {
  volatile guint ref_count;
  char *filename;
  gboolean for_write;
  gboolean on_nfs;

  int fd;
  char *data;
  gsize len;
  ino_t inode;

  guint32 tag;
  gint64 time_t_base;
  MetaFileHeader *header;
  MetaFileDirEnt *root;

  int num_attributes;
  char **attributes;

  MetaJournal *journal;
};

static void         meta_tree_refresh_locked   (MetaTree    *tree);
static MetaJournal *meta_journal_open          (MetaTree    *tree,
						const char  *filename,
						gboolean     for_write,
						guint32      tag);
static void         meta_journal_free          (MetaJournal *journal);
static void         meta_journal_validate_more (MetaJournal *journal);

static gpointer
verify_block_pointer (MetaTree *tree, guint32 pos, guint32 len)
{
  pos = GUINT32_FROM_BE (pos);

  /* Ensure 32bit aligned */
  if (pos %4 != 0)
    return NULL;

  if (pos > tree->len)
    return NULL;

  if (pos + len < pos ||
      pos + len > tree->len)
    return NULL;

  return tree->data + pos;
}

static gpointer
verify_array_block (MetaTree *tree, guint32 pos, gsize element_size)
{
  guint32 *nump, num;

  nump = verify_block_pointer (tree, pos, sizeof (guint32));
  if (nump == NULL)
    return NULL;

  num = GUINT32_FROM_BE (*nump);

  return verify_block_pointer (tree, pos, sizeof (guint32) + num * element_size);
}

static gpointer
verify_children_block (MetaTree *tree, guint32 pos)
{
  return verify_array_block (tree, pos, sizeof (MetaFileDirEnt));
}

static gpointer
verify_metadata_block (MetaTree *tree, guint32 pos)
{
  return verify_array_block (tree, pos, sizeof (MetaFileDataEnt));
}

static char *
verify_string (MetaTree *tree, guint32 pos)
{
  char *str, *ptr, *end;

  pos = GUINT32_FROM_BE (pos);

  if (pos > tree->len)
    return NULL;

  str = ptr = tree->data + pos;
  end = tree->data + tree->len;

  while (ptr < end && *ptr != 0)
    ptr++;

  if (ptr == end)
    return NULL;

  return str;
}

static void
meta_tree_clear (MetaTree *tree)
{
  if (tree->journal)
    {
      meta_journal_free (tree->journal);
      tree->journal = NULL;
    }

  g_free (tree->attributes);
  tree->num_attributes = 0;
  tree->attributes = NULL;

  tree->tag = 0;
  tree->time_t_base = 0;
  tree->header = NULL;
  tree->root = NULL;

  if (tree->data)
    {
      munmap(tree->data, tree->len);
      tree->data = NULL;
    }

  tree->len = 0;
  if (tree->fd != -1)
    {
      close (tree->fd);
      tree->fd = 0;
    }
}

static gboolean
is_on_nfs (char *filename)
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
    res = statfs_buffer.f_type == 0x6969;

#elif defined(USE_STATVFS) && defined(HAVE_STRUCT_STATVFS_F_BASETYPE)
  statfs_result = statvfs (dirname, &statfs_buffer);

  if (statfs_result == 0)
    res = strcmp (statfs_buffer.f_basetype, "nfs") == 0;
#endif

  g_free (dirname);

  return res;
}

static gboolean
link_to_tmp (const char *source, char *tmpl)
{
  char *XXXXXX;
  int count, res;
  static const char letters[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static const int NLETTERS = sizeof (letters) - 1;
  glong value;
  GTimeVal tv;
  static int counter = 0;

  /* find the last occurrence of "XXXXXX" */
  XXXXXX = g_strrstr (tmpl, "XXXXXX");
  g_assert (XXXXXX != NULL);

  /* Get some more or less random data.  */
  g_get_current_time (&tv);
  value = (tv.tv_usec ^ tv.tv_sec) + counter++;

  for (count = 0; count < 100; value += 7777, ++count)
    {
      glong v = value;

      /* Fill in the random bits.  */
      XXXXXX[0] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[1] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[2] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[3] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[4] = letters[v % NLETTERS];
      v /= NLETTERS;
      XXXXXX[5] = letters[v % NLETTERS];

      res = link (source, tmpl);

      if (res >= 0)
	return TRUE;
      else if (errno != EEXIST)
	/* Any other error will apply also to other names we might
	 *  try, and there are 2^32 or so of them, so give up now.
	 */
	return FALSE;
    }

  return FALSE;
}

static int
safe_open (MetaTree *tree,
	   char *filename,
	   int flags)
{
  if (tree->on_nfs)
    {
      char *dirname, *tmpname;
      int fd, errsv;

      /* On NFS if another client unlinks an open file
       * it is actually removed on the server and this
       * client will get an ESTALE error on later access.
       *
       * For a local (i.e. on this client) unlink this is
       * handled by the kernel keeping track of unlinks of
       * open files (by this client) using ".nfsXXXX" files.
       *
       * We work around the ESTALE problem by first linking
       * the file to a temp file that we then unlink on
       * this client. We never leak the tmpfile (unless
       * the kernel crashes) and no other client should
       * remove our tmpfile.
       */

      dirname = g_path_get_dirname (filename);
      tmpname = g_build_filename (dirname, ".openXXXXXX", NULL);
      g_free (dirname);

      if (!link_to_tmp (filename, tmpname))
	fd = open (filename, flags); /* link failed, what can we do... */
      else
	{
	  fd = open (tmpname, flags);
	  errsv = errno;
	  unlink (tmpname);
	  errno = errsv;
	}

      g_free (tmpname);
      return fd;
    }
  else
    return open (filename, flags);

}

static gboolean
meta_tree_init (MetaTree *tree)
{
  struct stat statbuf;
  int fd;
  void *data;
  guint32 *attributes;
  gboolean retried;
  int i;

  retried = FALSE;
 retry:
  tree->on_nfs = is_on_nfs (tree->filename);
  fd = safe_open (tree, tree->filename, O_RDONLY);
  if (fd == -1)
    {
      if (tree->for_write && !retried)
	{
	  MetaBuilder *builder;
	  char *dir;

	  dir = g_path_get_dirname (tree->filename);
	  g_mkdir_with_parents (dir, 0700);
	  g_free (dir);

	  builder = meta_builder_new ();
	  retried = TRUE;
	  if (meta_builder_write (builder, tree->filename))
	    {
	      meta_builder_free (builder);
	      goto retry;
	    }
	  meta_builder_free (builder);
	}
      tree->fd = -1;
      return FALSE;
    }

  if (fstat (fd, &statbuf) != 0 ||
      statbuf.st_size < sizeof (MetaFileHeader))
    {
      close (fd);
      return FALSE;
    }

  data = mmap (NULL, statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED)
    {
      close (fd);
      return FALSE;
    }

  tree->fd = fd;
  tree->len = statbuf.st_size;
  tree->inode = statbuf.st_ino;
  tree->data = data;
  tree->header = (MetaFileHeader *)data;

  if (memcmp (tree->header->magic, MAGIC, MAGIC_LEN) != 0)
    goto err;

  if (tree->header->major != MAJOR_VERSION)
    goto err;

  tree->root = verify_block_pointer (tree, tree->header->root, sizeof (MetaFileDirEnt));
  if (tree->root == NULL)
    goto err;

  attributes = verify_array_block (tree, tree->header->attributes, sizeof (guint32));
  if (attributes == NULL)
    goto err;

  tree->num_attributes = GUINT32_FROM_BE (*attributes);
  attributes++;
  tree->attributes = g_new (char *, tree->num_attributes);
  for (i = 0; i < tree->num_attributes; i++)
    {
      tree->attributes[i] = verify_string (tree, attributes[i]);
      if (tree->attributes[i] == NULL)
	goto err;
    }

  tree->tag = GUINT32_FROM_BE (tree->header->random_tag);
  tree->time_t_base = GINT64_FROM_BE (tree->header->time_t_base);

  tree->journal = meta_journal_open (tree, tree->filename, tree->for_write, tree->tag);

  /* There is a race with tree replacing, where the journal could have been
     deleted (and the tree replaced) inbetween opening the tree file and the
     journal. However we can detect this case by looking at the tree and see
     if its been rotated, we do this to ensure we have an uptodate tree+journal
     combo. */
  meta_tree_refresh_locked (tree);

  return TRUE;

 err:
  meta_tree_clear (tree);
  return FALSE;
}

MetaTree *
meta_tree_open (const char *filename,
		gboolean for_write)
{
  MetaTree *tree;

  g_assert (sizeof (MetaFileHeader) == 32);
  g_assert (sizeof (MetaFileDirEnt) == 16);
  g_assert (sizeof (MetaFileDataEnt) == 8);

  tree = g_new0 (MetaTree, 1);
  tree->ref_count = 1;
  tree->filename = g_strdup (filename);
  tree->for_write = for_write;
  tree->fd = -1;

  meta_tree_init (tree);

  return tree;
}

const char *
meta_tree_get_filename (MetaTree *tree)
{
  return tree->filename;
}

gboolean
meta_tree_exists (MetaTree *tree)
{
  return tree->fd != -1;
}

static GHashTable *cached_trees = NULL;
G_LOCK_DEFINE_STATIC (cached_trees);

MetaTree *
meta_tree_lookup_by_name (const char *name,
			  gboolean    for_write)
{
  MetaTree *tree;
  char *filename;

  G_LOCK (cached_trees);

  if (cached_trees == NULL)
    cached_trees = g_hash_table_new_full (g_str_hash,
					  g_str_equal,
					  (GDestroyNotify)g_free,
					  (GDestroyNotify)meta_tree_unref);

  tree = g_hash_table_lookup (cached_trees, name);
  if (tree && tree->for_write == for_write)
    {
      meta_tree_ref (tree);
      G_UNLOCK (cached_trees);

      meta_tree_refresh (tree);
      return tree;
    }

  filename = g_build_filename (g_get_user_data_dir (), "gvfs-metadata", name, NULL);
  tree = meta_tree_open (filename, for_write);
  g_free (filename);

  if (tree)
    g_hash_table_insert (cached_trees, g_strdup (name), meta_tree_ref (tree));

  G_UNLOCK (cached_trees);

  return tree;
}

MetaTree *
meta_tree_ref (MetaTree *tree)
{
  gint old_val;

  old_val = g_atomic_int_exchange_and_add ((int *)&tree->ref_count, 1);
  return tree;
}

void
meta_tree_unref (MetaTree *tree)
{
  gboolean is_zero;

  is_zero = g_atomic_int_dec_and_test ((int *)&tree->ref_count);
  if (is_zero)
    {
      meta_tree_clear (tree);
      g_free (tree->filename);
      g_free (tree);
    }
}

static gboolean
meta_tree_needs_rereading (MetaTree *tree)
{
  struct stat statbuf;

  if (tree->fd == -1)
    return TRUE;

  if (tree->header != NULL &&
      GUINT32_FROM_BE (tree->header->rotated) == 0)
    return FALSE; /* Got a valid tree and its not rotated */

  /* Sanity check to avoid infinite loops when a stable file
     has the rotated bit set to 1 (see gnome bugzilla bug #600057) */

  if (lstat (tree->filename, &statbuf) != 0)
    return FALSE;

  if (tree->inode == statbuf.st_ino)
    return FALSE;

  return TRUE;
}

static gboolean
meta_tree_has_new_journal_entries (MetaTree *tree)
{
  guint32 num_entries;
  MetaJournal *journal;

  journal = tree->journal;

  if (journal == NULL ||
      !tree->journal->journal_valid)
    return FALSE; /* Once we've seen a failure, never look for more */

  /* TODO: Use atomic read here? */
  num_entries = GUINT32_FROM_BE (*(volatile guint32 *)&journal->header->num_entries);

  return journal->last_entry_num < num_entries;
}


/* Must be called with a write lock held */
static void
meta_tree_refresh_locked (MetaTree *tree)
{
  /* Needs to recheck since we dropped read lock */
  if (meta_tree_needs_rereading (tree))
    {
      if (tree->header)
	meta_tree_clear (tree);
      meta_tree_init (tree);
    }
  else if (meta_tree_has_new_journal_entries (tree))
    meta_journal_validate_more (tree->journal);
}

void
meta_tree_refresh (MetaTree *tree)
{
  gboolean needs_refresh;

  g_static_rw_lock_reader_lock (&metatree_lock);
  needs_refresh =
    meta_tree_needs_rereading (tree) ||
    meta_tree_has_new_journal_entries (tree);
  g_static_rw_lock_reader_unlock (&metatree_lock);

  if (needs_refresh)
    {
      g_static_rw_lock_writer_lock (&metatree_lock);
      meta_tree_refresh_locked (tree);
      g_static_rw_lock_writer_unlock (&metatree_lock);
    }
}

struct FindName {
  MetaTree *tree;
  const char *name;
};

static int
find_dir_element (const void *_key, const void *_dirent)
{
  const struct FindName *key = _key;
  const MetaFileDirEnt *dirent = _dirent;
  char *dirent_name;

  dirent_name = verify_string (key->tree, dirent->name);
  if (dirent_name == NULL)
    return -1;
  return strcmp (key->name, dirent_name);
}

/* modifies path!!! */
static MetaFileDirEnt *
dir_lookup_path (MetaTree *tree,
		 MetaFileDirEnt *dirent,
		 char *path)
{
  char *end_path;
  MetaFileDir *dir;
  struct FindName key;

  while (*path == '/')
    path++;

  if (*path == 0)
    return dirent;

  if (dirent->children == 0)
    return NULL;

  dir = verify_children_block (tree, dirent->children);
  if (dir == NULL)
    return NULL;

  end_path = path;
  while (*end_path != 0 &&
	 *end_path != '/')
    end_path++;

  if (*end_path != 0)
    *end_path++ = 0;

  key.name = path;
  key.tree = tree;
  dirent = bsearch (&key, &dir->children[0],
		    GUINT32_FROM_BE (dir->num_children), sizeof (MetaFileDirEnt),
		    find_dir_element);

  if (dirent == NULL)
    return NULL;

  return dir_lookup_path (tree, dirent, end_path);
}

static MetaFileDirEnt *
meta_tree_lookup (MetaTree *tree,
		  const char *path)
{
  MetaFileDirEnt *dirent;
  char *path_copy;

  if (tree->root == NULL)
    return NULL;

  path_copy = g_strdup (path);
  dirent = dir_lookup_path (tree, tree->root, path_copy);
  g_free (path_copy);

  return dirent;
}

static MetaFileData *
meta_tree_lookup_data (MetaTree *tree,
		       const char *path)
{
  MetaFileDirEnt *dirent;
  MetaFileData *data;

  data = NULL;
  dirent = meta_tree_lookup (tree, path);
  if (dirent)
    data = verify_metadata_block (tree, dirent->metadata);

  return data;
}

static int
find_attribute_id (const void *_key, const void *_entry)
{
  const char *key = _key;
  const char *const*entry = _entry;

  return strcmp (key, *entry);
}

#define NO_KEY ((guint32)-1)

static guint32
get_id_for_key (MetaTree *tree,
		const char *attribute)
{
  char **attribute_ptr;

  attribute_ptr = bsearch (attribute, tree->attributes,
			   tree->num_attributes, sizeof (char *),
			   find_attribute_id);

  if (attribute_ptr == NULL)
    return NO_KEY;

  return attribute_ptr - tree->attributes;
}

struct FindId {
  MetaTree *tree;
  guint32 id;
};

static int
find_data_element (const void *_key, const void *_dataent)
{
  const struct FindId *key = _key;
  const MetaFileDataEnt *dataent = _dataent;
  guint32 key_id;

  key_id = GUINT32_FROM_BE (dataent->key) & ~KEY_IS_LIST_MASK;

  return key->id - key_id;
}

static MetaFileDataEnt *
meta_data_get_key (MetaTree *tree,
		   MetaFileData *data,
		   const char *attribute)
{
  MetaFileDataEnt *dataent;
  struct FindId key;

  key.id = get_id_for_key (tree, attribute);
  key.tree = tree;
  dataent = bsearch (&key, &data->keys[0],
		     GUINT32_FROM_BE (data->num_keys), sizeof (MetaFileDataEnt),
		     find_data_element);

  return dataent;
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

static void
meta_journal_free (MetaJournal *journal)
{
  g_free (journal->filename);
  munmap(journal->data, journal->len);
  close (journal->fd);
  g_free (journal);
}

static MetaJournalEntry *
verify_journal_entry (MetaJournal *journal,
		      MetaJournalEntry *entry)
{
  guint32 offset, real_crc32;
  guint32 entry_len, entry_len_end;
  char *ptr;

  ptr = (char *)entry;
  if (ptr < journal->data)
    return NULL;
  offset =  ptr - journal->data;

  /* Must be 32bit aligned */
  if (offset % 4 != 0)
    return NULL;

  /* entry_size must be valid */
  if (offset > journal->len - 4)
    return NULL;

  /* Verify that entry fits and has right size */
  entry_len = GUINT32_FROM_BE (entry->entry_size);

  /* Must be 32bit aligned */
  if (entry_len % 4 != 0)
    return NULL;
  /* Must have space for at the very least:
     len+crc32+mtime+type+path_terminating_zeor+end_len */
  if (journal->len < 4 + 4 + 8 + 1 + 1 + 4)
    return NULL;

  if (entry_len > journal->len ||
      offset > journal->len - entry_len)
    return NULL;

  entry_len_end = GUINT32_FROM_BE (*(guint32 *)(journal->data + offset + entry_len - 4));
  if (entry_len != entry_len_end)
    return NULL;

  real_crc32 = metadata_crc32 (journal->data + offset + 8, entry_len - 8);
  if (real_crc32 != GUINT32_FROM_BE (entry->crc32))
    return NULL;

  return (MetaJournalEntry *)(journal->data + offset + entry_len);
}

/* Try to validate more entries, call with writer lock */
static void
meta_journal_validate_more (MetaJournal *journal)
{
  guint32 num_entries, i;
  MetaJournalEntry *entry, *next_entry;

  if (!journal->journal_valid)
    return; /* Once we've seen a failure, never look for more */

  /* TODO: Use atomic read here? */
  num_entries = GUINT32_FROM_BE (*(volatile guint32 *)&journal->header->num_entries);

  entry = journal->last_entry;
  i = journal->last_entry_num;
  while (i < num_entries)
    {
      next_entry = verify_journal_entry (journal, entry);

      if (next_entry == NULL)
	{
	  journal->journal_valid = FALSE;
	  break;
	}

      entry = next_entry;
      i++;
    }

  journal->last_entry = entry;
  journal->last_entry_num = i;
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
append_uint32 (GString *s, guint32 val)
{
  union {
    guint32 as_int;
    char as_bytes[4];
  } u;

  u.as_int = GUINT32_TO_BE (val);
  g_string_append_len (s, u.as_bytes, 4);
  return s;
}

static GString *
append_uint64 (GString *s, guint64 val)
{
  union {
    guint64 as_int;
    char as_bytes[8];
  } u;

  u.as_int = GUINT64_TO_BE (val);
  g_string_append_len (s, u.as_bytes, 8);
  return s;
}

static GString *
append_string (GString *s, const char *str)
{
  g_string_append (s, str);
  g_string_append_c (s, 0);
  return s;
}

static guint64
get_time_t (MetaTree *tree, guint32 val)
{
  val = GUINT32_FROM_BE (val);
  if (val == 0)
    return 0;
  return val + tree->time_t_base;
}

static GString *
meta_journal_entry_init (int op,
			 guint64 mtime,
			 const char *path)
{
  GString *out;

  out = g_string_new (NULL);
  append_uint32 (out, 0); /* len */
  append_uint32 (out, 0); /* crc32 */
  append_uint64 (out, mtime);
  g_string_append_c (out, (char)op);
  append_string (out, path);

  return out;
}

static GString *
meta_journal_entry_finish (GString *out)
{
  guint32 len;

  while (out->len % 4 != 0)
    g_string_append_c (out, 0);

  len = out->len + 4;
  append_uint32 (out, len);
  set_uint32 (out, 0, len);
  set_uint32 (out, 4, metadata_crc32 (out->str + 8, len - 8));
  return out;
}

static GString *
meta_journal_entry_new_set (guint64 mtime,
			    const char *path,
			    const char *key,
			    const char *value)
{
  GString *out;

  out = meta_journal_entry_init (JOURNAL_OP_SET_KEY, mtime, path);
  append_string (out, key);
  append_string (out, value);
  return meta_journal_entry_finish (out);
}

static GString *
meta_journal_entry_new_setv (guint64 mtime,
			     const char *path,
			     const char *key,
			     char      **value)
{
  GString *out;
  int i;

  out = meta_journal_entry_init (JOURNAL_OP_SETV_KEY, mtime, path);
  append_string (out, key);

  /* Pad to 32bit */
  while (out->len % 4 != 0)
    g_string_append_c (out, 0);

  append_uint32 (out, g_strv_length ((char **)value));
  for (i = 0; value[i] != NULL; i++)
    append_string (out, value[i]);

  return meta_journal_entry_finish (out);
}

static GString *
meta_journal_entry_new_remove (guint64 mtime,
			       const char *path)
{
  GString *out;

  out = meta_journal_entry_init (JOURNAL_OP_REMOVE_PATH, mtime, path);
  return meta_journal_entry_finish (out);
}

static GString *
meta_journal_entry_new_copy (guint64 mtime,
			     const char *src,
			     const char *dst)
{
  GString *out;

  out = meta_journal_entry_init (JOURNAL_OP_COPY_PATH, mtime, dst);
  append_string (out, src);
  return meta_journal_entry_finish (out);
}

static GString *
meta_journal_entry_new_unset (guint64 mtime,
			      const char *path,
			      const char *key)
{
  GString *out;

  out = meta_journal_entry_init (JOURNAL_OP_UNSET_KEY, mtime, path);
  append_string (out, key);
  return meta_journal_entry_finish (out);
}


/* Call with writer lock held */
static gboolean
meta_journal_add_entry (MetaJournal *journal,
			GString *entry)
{
  char *ptr;
  guint32 offset;

  g_assert (journal->journal_valid);

  ptr = (char *)journal->last_entry;
  offset =  ptr - journal->data;

  /* Does the entry fit? */
  if (entry->len > journal->len - offset)
    return FALSE;

  memcpy (ptr, entry->str, entry->len);

  journal->header->num_entries = GUINT_TO_BE (journal->last_entry_num + 1);
  meta_journal_validate_more (journal);
  g_assert (journal->journal_valid);

  return TRUE;
}

static MetaJournal *
meta_journal_open (MetaTree *tree, const char *filename, gboolean for_write, guint32 tag)
{
  MetaJournal *journal;
  struct stat statbuf;
  int fd;
  char *data;
  char *journal_filename;
  int open_flags, mmap_prot;

  g_assert (sizeof (MetaJournalHeader) == 20);

  journal_filename = get_journal_filename (filename, tag);

  if (for_write)
    open_flags = O_RDWR;
  else
    open_flags = O_RDONLY;

  fd = safe_open (tree, journal_filename, open_flags);
  g_free (journal_filename);
  if (fd == -1)
    return NULL;

  if (fstat (fd, &statbuf) != 0 ||
      statbuf.st_size < sizeof (MetaJournalHeader))
    {
      close (fd);
      return NULL;
    }

  mmap_prot = PROT_READ;
  if (for_write)
    mmap_prot |= PROT_WRITE;
  data = mmap (NULL, statbuf.st_size, mmap_prot, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED)
    {
      close (fd);
      return NULL;
    }

  journal = g_new0 (MetaJournal, 1);
  journal->filename = g_strdup (filename);
  journal->fd = fd;
  journal->len = statbuf.st_size;
  journal->data = data;
  journal->header = (MetaJournalHeader *)data;
  journal->first_entry = (MetaJournalEntry *)(data + sizeof (MetaJournalHeader));
  journal->last_entry = journal->first_entry;
  journal->last_entry_num = 0;

  if (memcmp (journal->header->magic, JOURNAL_MAGIC, JOURNAL_MAGIC_LEN) != 0)
    goto err;

  if (journal->header->major != JOURNAL_MAJOR_VERSION)
    goto err;

  if (journal->len != GUINT32_FROM_BE (journal->header->file_size))
    goto err;

  if (tag != GUINT32_FROM_BE (journal->header->random_tag))
    goto err;

  journal->journal_valid = TRUE;
  meta_journal_validate_more (journal);

  return journal;

 err:
  meta_journal_free (journal);
  return NULL;
}

static char *
get_next_arg (char *str)
{
  return str  + strlen (str) + 1;
}

static gboolean
journal_entry_is_key_type (MetaJournalEntry *entry)
{
 return
   entry->entry_type == JOURNAL_OP_SET_KEY ||
   entry->entry_type == JOURNAL_OP_SETV_KEY ||
   entry->entry_type == JOURNAL_OP_UNSET_KEY;
}

static gboolean
journal_entry_is_path_type (MetaJournalEntry *entry)
{
 return
   entry->entry_type == JOURNAL_OP_COPY_PATH ||
   entry->entry_type == JOURNAL_OP_REMOVE_PATH;
}

/* returns remainer if path has "prefix" as prefix (or is equal to prefix) */
static const char *
get_prefix_match (const char *path,
		  const char *prefix)
{
  gsize prefix_len;
  const char *remainder;

  prefix_len = strlen (prefix);

  /* Handle trailing slashes in prefix, this is not
     generally common, but happens in the case of the
     root dir "/" */
  while (prefix_len > 0 &&
	 prefix[prefix_len-1] == '/')
    prefix_len--;

  if (strncmp (path, prefix, prefix_len) != 0)
    return NULL;

  remainder = path + prefix_len;
  if (*remainder != 0 &&
      *remainder != '/')
    return NULL; /* only a string prefix, not a path prefix */

  while (*remainder == '/')
    remainder++;

  return remainder;
}

typedef gboolean (*journal_key_callback) (MetaJournal *journal,
					  MetaJournalEntryType entry_type,
					  const char *path,
					  guint64 mtime,
					  const char *key,
					  gpointer value,
					  char **iter_path,
					  gpointer user_data);
typedef gboolean (*journal_path_callback) (MetaJournal *journal,
					   MetaJournalEntryType entry_type,
					   const char *path,
					   guint64 mtime,
					   const char *source_path,
					   char **iter_path,
					   gpointer user_data);

static char *
meta_journal_iterate (MetaJournal *journal,
		      const char *path,
		      journal_key_callback key_callback,
		      journal_path_callback path_callback,
		      gpointer user_data)
{
  MetaJournalEntry *entry;
  guint32 *sizep;
  char *journal_path, *journal_key, *source_path;
  char *path_copy, *value;
  gboolean res;
  guint64 mtime;

  path_copy = g_strdup (path);

  if (journal == NULL)
    return path_copy;

  entry = journal->last_entry;
  while (entry > journal->first_entry)
    {
      sizep = (guint32 *)entry;
      entry = (MetaJournalEntry *)((char *)entry - GUINT32_FROM_BE (*(sizep-1)));

      mtime = GUINT64_FROM_BE (entry->mtime);
      journal_path = &entry->path[0];

      if (journal_entry_is_key_type (entry) &&
	  key_callback) /* set, setv or unset */
	{
	  journal_key = get_next_arg (journal_path);
	  value = get_next_arg (journal_key);

	  /* Only affects is path is exactly the same */
	  res = key_callback (journal, entry->entry_type,
			      journal_path, mtime, journal_key,
			      value,
			      &path_copy, user_data);
	  if (!res)
	    {
	      g_free (path_copy);
	      return NULL;
	    }
	}
      else if (journal_entry_is_path_type (entry) &&
	       path_callback) /* copy or remove */
	{
	  source_path = NULL;
	  if (entry->entry_type == JOURNAL_OP_COPY_PATH)
	    source_path = get_next_arg (journal_path);

	  res = path_callback (journal, entry->entry_type,
			       journal_path, mtime, source_path,
			       &path_copy, user_data);
	  if (!res)
	    {
	      g_free (path_copy);
	      return NULL;
	    }
	}
      else
	g_warning ("Unknown journal entry type %d\n", entry->entry_type);
    }

  return path_copy;
}

typedef struct {
  const char *key;
  MetaKeyType type;
  guint64 mtime;
  gpointer value;
} PathKeyData;

static gboolean
journal_iter_key (MetaJournal *journal,
		  MetaJournalEntryType entry_type,
		  const char *path,
		  guint64 mtime,
		  const char *key,
		  gpointer value,
		  char **iter_path,
		  gpointer user_data)
{
  PathKeyData *data = user_data;

  if (strcmp (path, *iter_path) != 0)
    return TRUE; /* No match, continue */

  data->mtime = mtime;

  if (data->key == NULL)
    return FALSE; /* Matched path, not interested in key, stop iterating */

  if (strcmp (data->key, key) != 0)
    return TRUE; /* No match, continue */

  switch (entry_type)
    {
    case JOURNAL_OP_SET_KEY:
      data->type = META_KEY_TYPE_STRING;
      data->value = value;
      break;
    case JOURNAL_OP_SETV_KEY:
      data->type = META_KEY_TYPE_STRINGV;
      data->value = value;
      break;
    case JOURNAL_OP_UNSET_KEY:
      data->type = META_KEY_TYPE_NONE;
      data->value = NULL;
      break;
    default:
      /* No other key type should reach this  */
      g_assert_not_reached ();
    }
  return FALSE; /* stop iterating */
}

static gboolean
journal_iter_path (MetaJournal *journal,
		   MetaJournalEntryType entry_type,
		   const char *path,
		   guint64 mtime,
		   const char *source_path,
		   char **iter_path,
		   gpointer user_data)
{
  PathKeyData *data = user_data;
  char *old_path;
  const char *remainder;

  /* is this a parent of the iter path */
  remainder = get_prefix_match (*iter_path, path);
  if (remainder == NULL)
    return TRUE; /* Not related, continue */

  /* path is affected as a child of this node */
  if (entry_type == JOURNAL_OP_REMOVE_PATH)
    {
      if (data)
	{
	  data->mtime = mtime;
	  data->type = META_KEY_TYPE_NONE;
	  data->value = NULL;
	}
      return FALSE; /* stop iterating */
    }
  else if (entry_type == JOURNAL_OP_COPY_PATH)
    {
      old_path = *iter_path;
      *iter_path = g_build_filename (source_path, remainder, NULL);
      g_free (old_path);
      return TRUE; /* Continue, with new path */
    }
  return TRUE;
}

static char *
meta_journal_reverse_map_path_and_key (MetaJournal *journal,
				       const char *path,
				       const char *key,
				       MetaKeyType *type,
				       guint64 *mtime,
				       gpointer *value)
{
  PathKeyData data = {0};
  char *res_path;

  data.key = key;
  res_path = meta_journal_iterate (journal,
				   path,
				   journal_iter_key,
				   journal_iter_path,
				   &data);
  *type = data.type;
  if (mtime)
    *mtime = data.mtime;
  *value = data.value;
  return res_path;
}

MetaKeyType
meta_tree_lookup_key_type  (MetaTree                         *tree,
			    const char                       *path,
			    const char                       *key)
{
  MetaFileData *data;
  MetaFileDataEnt *ent;
  char *new_path;
  MetaKeyType type;
  gpointer value;

  g_static_rw_lock_reader_lock (&metatree_lock);

  new_path = meta_journal_reverse_map_path_and_key (tree->journal,
						    path,
						    key,
						    &type, NULL, &value);
  if (new_path == NULL)
    goto out; /* type is set */

  data = meta_tree_lookup_data (tree, new_path);
  ent = NULL;
  if (data)
    ent = meta_data_get_key (tree, data, key);

  g_free (new_path);

  if (ent == NULL)
    type = META_KEY_TYPE_NONE;
  else if (GUINT32_FROM_BE (ent->key) & KEY_IS_LIST_MASK)
    type = META_KEY_TYPE_STRINGV;
  else
    type = META_KEY_TYPE_STRING;

 out:
  g_static_rw_lock_reader_unlock (&metatree_lock);
  return type;
}

guint64
meta_tree_get_last_changed (MetaTree *tree,
			    const char *path)
{
  MetaFileDirEnt *dirent;
  MetaKeyType type;
  char *new_path;
  gpointer value;
  guint64 res, mtime;

  g_static_rw_lock_reader_lock (&metatree_lock);

  new_path = meta_journal_reverse_map_path_and_key (tree->journal,
						    path,
						    NULL,
						    &type, &mtime, &value);
  if (new_path == NULL)
    {
      res = mtime;
      goto out;
    }

  res = 0;
  dirent = meta_tree_lookup (tree, new_path);
  if (dirent)
    res = get_time_t (tree, dirent->last_changed);

  g_free (new_path);

 out:
  g_static_rw_lock_reader_unlock (&metatree_lock);

  return res;
}

char *
meta_tree_lookup_string (MetaTree   *tree,
			 const char *path,
			 const char *key)
{
  MetaFileData *data;
  MetaFileDataEnt *ent;
  MetaKeyType type;
  gpointer value;
  char *new_path;
  char *res;

  g_static_rw_lock_reader_lock (&metatree_lock);

  new_path = meta_journal_reverse_map_path_and_key (tree->journal,
						    path,
						    key,
						    &type, NULL, &value);
  if (new_path == NULL)
    {
      res = NULL;
      if (type == META_KEY_TYPE_STRING)
	res = g_strdup (value);
      goto out;
    }

  data = meta_tree_lookup_data (tree, new_path);
  ent = NULL;
  if (data)
    ent = meta_data_get_key (tree, data, key);

  g_free (new_path);

  if (ent == NULL)
    res = NULL;
  else if (GUINT32_FROM_BE (ent->key) & KEY_IS_LIST_MASK)
    res = NULL;
  else
    res = g_strdup (verify_string (tree, ent->value));

 out:
  g_static_rw_lock_reader_unlock (&metatree_lock);

  return res;
}

static char **
get_stringv_from_journal (gpointer value,
			  gboolean dup_strings)
{
  char *valuep = value;
  guint32 num_strings, i;
  char **res;

  while (((gsize)valuep) % 4 != 0)
    valuep++;

  num_strings = GUINT32_FROM_BE (*(guint32 *)valuep);
  valuep += 4;

  res = g_new (char *, num_strings + 1);

  for (i = 0; i < num_strings; i++)
    {
      if (dup_strings)
	res[i] = g_strdup (valuep);
      else
	res[i] = valuep;
      valuep = get_next_arg (valuep);
    }

  res[i] = NULL;

  return res;
}

char **
meta_tree_lookup_stringv   (MetaTree                         *tree,
			    const char                       *path,
			    const char                       *key)
{
  MetaFileData *data;
  MetaFileDataEnt *ent;
  MetaKeyType type;
  MetaFileStringv *stringv;
  gpointer value;
  char *new_path;
  char **res;
  guint32 num_strings, i;

  g_static_rw_lock_reader_lock (&metatree_lock);

  new_path = meta_journal_reverse_map_path_and_key (tree->journal,
						    path,
						    key,
						    &type, NULL, &value);
  if (new_path == NULL)
    {
      res = NULL;
      if (type == META_KEY_TYPE_STRINGV)
	res = get_stringv_from_journal (value, TRUE);
      goto out;
    }

  data = meta_tree_lookup_data (tree, new_path);
  ent = NULL;
  if (data)
    ent = meta_data_get_key (tree, data, key);

  g_free (new_path);

  if (ent == NULL)
    res = NULL;
  else if ((GUINT32_FROM_BE (ent->key) & KEY_IS_LIST_MASK) == 0)
    res = NULL;
  else
    {
      stringv = verify_array_block (tree, ent->value,
				    sizeof (guint32));
      num_strings = GUINT32_FROM_BE (stringv->num_strings);
      res = g_new (char *, num_strings + 1);
      for (i = 0; i < num_strings; i++)
	res[i] = g_strdup (verify_string (tree, stringv->strings[i]));
      res[i] = NULL;
    }

 out:
  g_static_rw_lock_reader_unlock (&metatree_lock);

  return res;
}

typedef struct {
  char *name;
  guint64 last_changed;
  gboolean has_children;
  gboolean has_data;
  gboolean exists; /* May be true even if deleted is true, if recreated */
  gboolean deleted; /* Was deleted at some point, ignore everything before */

  gboolean reported; /* Set to true when reported to user */
} EnumDirChildInfo;

typedef struct {
  GHashTable *children;
} EnumDirData;


static void
child_info_free (EnumDirChildInfo *info)
{
  g_free (info->name);
  g_free (info);
}

static EnumDirChildInfo *
get_child_info (EnumDirData *data,
		const char *remainder,
		gboolean *direct_child)
{
  EnumDirChildInfo *info;
  const char *slash;
  char *name;

  slash = strchr (remainder, '/');
  if (slash != 0)
    name = g_strndup (remainder, slash - remainder);
  else
    name = g_strdup (remainder);

  *direct_child = slash == NULL;

  info = g_hash_table_lookup (data->children, name);
  if (info == NULL)
    {
      info = g_new0 (EnumDirChildInfo, 1);
      info->name = name;
      g_hash_table_insert (data->children, info->name, info);
    }
  else
    g_free (name);

  return info;
}

static gboolean
enum_dir_iter_key (MetaJournal *journal,
		   MetaJournalEntryType entry_type,
		   const char *path,
		   guint64 mtime,
		   const char *key,
		   gpointer value,
		   char **iter_path,
		   gpointer user_data)
{
  EnumDirData *data = user_data;
  EnumDirChildInfo *info;
  gboolean direct_child;
  const char *remainder;

  /* is this a true child of iter_path, then that may create a child */
  remainder = get_prefix_match (path, *iter_path);
  if (remainder != NULL && *remainder != 0)
    {
      info = get_child_info (data, remainder, &direct_child);

      if (!info->deleted)
	{
	  info->exists = TRUE;
	  if (info->last_changed == 0)
	    info->last_changed = mtime;
	  info->has_children |= !direct_child;
	  info->has_data |=
	    direct_child && entry_type != JOURNAL_OP_UNSET_KEY;
	}
    }

  return TRUE; /* continue */
}

static gboolean
enum_dir_iter_path (MetaJournal *journal,
		    MetaJournalEntryType entry_type,
		    const char *path,
		    guint64 mtime,
		    const char *source_path,
		    char **iter_path,
		    gpointer user_data)
{
  EnumDirData *data = user_data;
  EnumDirChildInfo *info;
  gboolean direct_child;
  const char *remainder;
  char *old_path;

  /* Is path a true child of iter_path */
  remainder = get_prefix_match (path, *iter_path);
  if (remainder != NULL && *remainder != 0)
    {
      info = get_child_info (data, remainder, &direct_child);

	/* copy destination a true child, that creates a child */
      if (entry_type == JOURNAL_OP_COPY_PATH)
	{
	  if (!info->deleted)
	    {
	      info->exists = TRUE;
	      if (info->last_changed == 0)
		info->last_changed = mtime;
	      info->has_children = TRUE;
	      info->has_data = TRUE;
	    }
	}
      else if (entry_type == JOURNAL_OP_REMOVE_PATH &&
	       direct_child)
	{
	  info->deleted = TRUE;
	}
    }

  /* is this a parent of the iter path */
  remainder = get_prefix_match (*iter_path, path);
  if (remainder != NULL)
    {
      /* path is affected as a child of this node */
      if (entry_type == JOURNAL_OP_REMOVE_PATH)
	return FALSE; /* stop iterating */
      else if (entry_type == JOURNAL_OP_COPY_PATH)
	{
	  old_path = *iter_path;
	  *iter_path = g_build_filename (source_path, remainder, NULL);
	  g_free (old_path);
	  return TRUE; /* Continue, with new path */
	}
    }

  return TRUE;
}

static gboolean
enumerate_dir (MetaTree *tree,
	       MetaFileDir *dir,
	       GHashTable *children,
	       meta_tree_dir_enumerate_callback callback,
	       gpointer user_data)
{
  guint32 i, num_children;
  MetaFileDirEnt *dirent;
  EnumDirChildInfo *info;
  char *dirent_name;
  gboolean has_children;
  gboolean has_data;
  guint64 last_changed;

  num_children = GUINT32_FROM_BE (dir->num_children);
  for (i = 0; i < num_children; i++)
    {
      dirent = &dir->children[i];
      dirent_name = verify_string (tree, dirent->name);
      if (dirent_name == NULL)
	continue;

      last_changed = get_time_t (tree, dirent->last_changed);
      has_children = dirent->children != 0;
      has_data = dirent->metadata != 0;

      info = g_hash_table_lookup (children, dirent_name);
      if (info)
	{
	  if (info->deleted)
	    continue; /* if recreated (i.e. exists == TRUE), report later */

	  info->reported = TRUE;

	  if (info->last_changed != 0)
	    last_changed = MAX (last_changed, info->last_changed);

	  has_children |= info->has_children;
	  has_data |= info->has_data;
	}

      if (!callback (dirent_name,
		     last_changed,
		     has_children,
		     has_data,
		     user_data))
	return FALSE;
    }
  return TRUE;
}

void
meta_tree_enumerate_dir (MetaTree                         *tree,
			 const char                       *path,
			 meta_tree_dir_enumerate_callback  callback,
			 gpointer                          user_data)
{
  EnumDirData data;
  GHashTable *children;
  EnumDirChildInfo *info;
  MetaFileDirEnt *dirent;
  GHashTableIter iter;
  MetaFileDir *dir;
  char *res_path;

  g_static_rw_lock_reader_lock (&metatree_lock);

  data.children = children =
    g_hash_table_new_full (g_str_hash,
			   g_str_equal,
			   NULL,
			   (GDestroyNotify)child_info_free);


  res_path = meta_journal_iterate (tree->journal,
				   path,
				   enum_dir_iter_key,
				   enum_dir_iter_path,
				   &data);

  if (res_path != NULL)
    {
      dirent = meta_tree_lookup (tree, res_path);
      if (dirent != NULL &&
	  dirent->children != 0)
	{
	  dir = verify_children_block (tree, dirent->children);
	  if (dir)
	    {
	      if (!enumerate_dir (tree, dir, children, callback, user_data))
		goto out;
	    }
	}
    }

  g_hash_table_iter_init (&iter, children);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer  *)&info))
    {
      if (info->reported || !info->exists)
	continue;

      if (!callback (info->name,
		     info->last_changed,
		     info->has_children,
		     info->has_data,
		     user_data))
	break;
    }
 out:
  g_free (res_path);
  g_hash_table_destroy (children);
  g_static_rw_lock_reader_unlock (&metatree_lock);
}

typedef struct {
  char *key;

  MetaKeyType type;
  gpointer value;

  gboolean seen; /* We saw this key in the journal */
} EnumKeysInfo;

typedef struct {
  GHashTable *keys;
} EnumKeysData;


static void
key_info_free (EnumKeysInfo *info)
{
  g_free (info->key);
  g_free (info);
}

static EnumKeysInfo *
get_key_info (EnumKeysData *data,
	      const char *key)
{
  EnumKeysInfo *info;

  info = g_hash_table_lookup (data->keys, key);
  if (info == NULL)
    {
      info = g_new0 (EnumKeysInfo, 1);
      info->key = g_strdup (key);
      g_hash_table_insert (data->keys, info->key, info);
    }

  return info;
}

static gboolean
enum_keys_iter_key (MetaJournal *journal,
		    MetaJournalEntryType entry_type,
		    const char *path,
		    guint64 mtime,
		    const char *key,
		    gpointer value,
		    char **iter_path,
		    gpointer user_data)
{
  EnumKeysData *data = user_data;
  EnumKeysInfo *info;

  if (strcmp (path, *iter_path) == 0)
    {
      info = get_key_info (data, key);

      if (!info->seen)
	{
	  info->seen = TRUE;
	  if (entry_type == JOURNAL_OP_UNSET_KEY)
	    info->type = META_KEY_TYPE_NONE;
	  else if (entry_type == JOURNAL_OP_SET_KEY)
	    info->type = META_KEY_TYPE_STRING;
	  else
	    info->type = META_KEY_TYPE_STRINGV;
	  info->value = value;
	}
    }

  return TRUE; /* continue */
}

static gboolean
enum_keys_iter_path (MetaJournal *journal,
		     MetaJournalEntryType entry_type,
		     const char *path,
		     guint64 mtime,
		     const char *source_path,
		     char **iter_path,
		     gpointer user_data)
{
  const char *remainder;
  char *old_path;

  /* is this a parent of the iter path */
  remainder = get_prefix_match (*iter_path, path);
  if (remainder != NULL)
    {
      /* path is affected as a child of this node */
      if (entry_type == JOURNAL_OP_REMOVE_PATH)
	return FALSE; /* stop iterating */
      else if (entry_type == JOURNAL_OP_COPY_PATH)
	{
	  old_path = *iter_path;
	  *iter_path = g_build_filename (source_path, remainder, NULL);
	  g_free (old_path);
	  return TRUE; /* Continue, with new path */
	}
    }

  return TRUE;
}

static gboolean
enumerate_data (MetaTree *tree,
		MetaFileData *data,
		GHashTable *keys,
		meta_tree_keys_enumerate_callback callback,
		gpointer user_data)
{
  guint32 i, j, num_keys, num_strings;
  MetaFileDataEnt *ent;
  EnumKeysInfo *info;
  char *key_name;
  guint32 key_id;
  MetaKeyType type;
  gpointer value;
  MetaFileStringv *stringv;
  gpointer free_me;
  char **strv;
  char *strv_static[10];

  num_keys = GUINT32_FROM_BE (data->num_keys);
  for (i = 0; i < num_keys; i++)
    {
      ent = &data->keys[i];

      key_id = GUINT32_FROM_BE (ent->key) & ~KEY_IS_LIST_MASK;
      if (GUINT32_FROM_BE (ent->key) & KEY_IS_LIST_MASK)
	type = META_KEY_TYPE_STRINGV;
      else
	type = META_KEY_TYPE_STRING;

      if (key_id >= tree->num_attributes)
	continue;

      key_name = tree->attributes[key_id];
      if (key_name == NULL)
	continue;

      info = g_hash_table_lookup (keys, key_name);
      if (info)
	continue; /* overridden, handle later */

      free_me = NULL;
      if (type == META_KEY_TYPE_STRING)
	value = verify_string (tree, ent->value);
      else
	{
	  stringv = verify_array_block (tree, ent->value,
					sizeof (guint32));
	  num_strings = GUINT32_FROM_BE (stringv->num_strings);

	  if (num_strings < 10)
	    strv = strv_static;
	  else
	    {
	      strv = g_new (char *, num_strings + 1);
	      free_me = (gpointer)strv;
	    }

	  for (j = 0; j < num_strings; j++)
	    strv[j] = verify_string (tree, stringv->strings[j]);
	  strv[j] = NULL;

	  value = strv;
	}

      if (!callback (key_name,
		     type,
		     value,
		     user_data))
	return FALSE;

      g_free (free_me);
    }
  return TRUE;
}

void
meta_tree_enumerate_keys (MetaTree                         *tree,
			  const char                       *path,
			  meta_tree_keys_enumerate_callback callback,
			  gpointer                          user_data)
{
  EnumKeysData keydata;
  GHashTable *keys;
  EnumKeysInfo *info;
  MetaFileData *data;
  GHashTableIter iter;
  char *res_path;

  g_static_rw_lock_reader_lock (&metatree_lock);

  keydata.keys = keys =
    g_hash_table_new_full (g_str_hash,
			   g_str_equal,
			   NULL,
			   (GDestroyNotify)key_info_free);


  res_path = meta_journal_iterate (tree->journal,
				   path,
				   enum_keys_iter_key,
				   enum_keys_iter_path,
				   &keydata);

  if (res_path != NULL)
    {
      data = meta_tree_lookup_data (tree, res_path);
      if (data != NULL)
	{
	  if (!enumerate_data (tree, data, keys, callback, user_data))
	    goto out;
	}
    }

  g_hash_table_iter_init (&iter, keys);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer  *)&info))
    {
      gpointer value;

      if (info->type == META_KEY_TYPE_NONE)
	continue;

      if (info->type == META_KEY_TYPE_STRING)
	value = info->value;
      else
	{
	  g_assert (info->type == META_KEY_TYPE_STRINGV);
	  value = get_stringv_from_journal (info->value, FALSE);
	}

      if (!callback (info->key,
		     info->type,
		     value,
		     user_data))
	break;

      if (info->type == META_KEY_TYPE_STRINGV)
	g_free (value);

    }
 out:
  g_free (res_path);
  g_hash_table_destroy (keys);
  g_static_rw_lock_reader_unlock (&metatree_lock);
}


static void
copy_tree_to_builder (MetaTree *tree,
		      MetaFileDirEnt *dirent,
		      MetaFile *builder_file)
{
  MetaFile *builder_child;
  MetaFileData *data;
  MetaFileDataEnt *ent;
  MetaFileDir *dir;
  MetaFileDirEnt *child_dirent;
  MetaKeyType type;
  char *child_name, *key_name, *value;
  guint32 i, num_keys, num_children, j;
  guint32 key_id;

  /* Copy metadata */
  data = verify_metadata_block (tree, dirent->metadata);
  if (data)
    {
      num_keys = GUINT32_FROM_BE (data->num_keys);
      for (i = 0; i < num_keys; i++)
	{
	  ent = &data->keys[i];

	  key_id = GUINT32_FROM_BE (ent->key) & ~KEY_IS_LIST_MASK;
	  if (GUINT32_FROM_BE (ent->key) & KEY_IS_LIST_MASK)
	    type = META_KEY_TYPE_STRINGV;
	  else
	    type = META_KEY_TYPE_STRING;

	  if (key_id >= tree->num_attributes)
	    continue;

	  key_name = tree->attributes[key_id];
	  if (key_name == NULL)
	    continue;

	  if (type == META_KEY_TYPE_STRING)
	    {
	      value = verify_string (tree, ent->value);
	      if (value)
		metafile_key_set_value (builder_file,
					key_name, value);
	    }
	  else
	    {
	      MetaFileStringv *stringv;
	      guint32 num_strings;
	      char *str;

	      stringv = verify_array_block (tree, ent->value,
					    sizeof (guint32));

	      if (stringv)
		{
		  metafile_key_list_set (builder_file, key_name);

		  num_strings = GUINT32_FROM_BE (stringv->num_strings);
		  for (j = 0; j < num_strings; j++)
		    {
		      str = verify_string (tree, stringv->strings[j]);
		      if (str)
			metafile_key_list_add (builder_file,
					       key_name, str);
		    }
		}
	    }
	}
    }

  /* Copy last changed time */
  builder_file->last_changed = get_time_t (tree, dirent->last_changed);

  /* Copy children */
  if (dirent->children != 0 &&
      (dir = verify_children_block (tree, dirent->children)) != NULL)
    {
      num_children = GUINT32_FROM_BE (dir->num_children);
      for (i = 0; i < num_children; i++)
	{
	  child_dirent = &dir->children[i];
	  child_name = verify_string (tree, child_dirent->name);
	  if (child_name != NULL)
	    {
	      builder_child = metafile_new (child_name, builder_file);
	      copy_tree_to_builder (tree, child_dirent, builder_child);
	    }
	}
    }
}

static void
apply_journal_to_builder (MetaTree *tree,
			  MetaBuilder *builder)
{
  MetaJournal *journal;
  MetaJournalEntry *entry;
  guint32 *sizep;
  guint64 mtime;
  char *journal_path, *journal_key, *source_path;
  char *value;
  char **strv;
  MetaFile *file;
  int i;

  journal = tree->journal;

  entry = journal->first_entry;
  while (entry < journal->last_entry)
    {
      mtime = GUINT64_FROM_BE (entry->mtime);
      journal_path = &entry->path[0];

      switch (entry->entry_type)
	{
	case JOURNAL_OP_SET_KEY:
	  journal_key = get_next_arg (journal_path);
	  value = get_next_arg (journal_key);
	  file = meta_builder_lookup (builder, journal_path, TRUE);
	  metafile_key_set_value (file,
				  journal_key,
				  value);
	  metafile_set_mtime (file, mtime);
	  break;
	case JOURNAL_OP_SETV_KEY:
	  journal_key = get_next_arg (journal_path);
	  value = get_next_arg (journal_key);
	  strv = get_stringv_from_journal (value, FALSE);
	  file = meta_builder_lookup (builder, journal_path, TRUE);

	  metafile_key_list_set (file, journal_key);
	  for (i = 0; strv[i] != NULL; i++)
	    metafile_key_list_add (file, journal_key, strv[i]);

	  g_free (strv);
	  metafile_set_mtime (file, mtime);
	  break;
	case JOURNAL_OP_UNSET_KEY:
	  journal_key = get_next_arg (journal_path);
	  file = meta_builder_lookup (builder, journal_path, FALSE);
	  if (file)
	    {
	      metafile_key_unset (file, journal_key);
	      metafile_set_mtime (file, mtime);
	    }
	  break;
	case JOURNAL_OP_COPY_PATH:
	  source_path = get_next_arg (journal_path);
	  meta_builder_copy (builder,
			     source_path,
			     journal_path,
			     mtime);
	  break;
	case JOURNAL_OP_REMOVE_PATH:
	  meta_builder_remove (builder,
			       journal_path,
			       mtime);
	  break;
	default:
	  break;
	}

      sizep = (guint32 *)entry;
      entry = (MetaJournalEntry *)((char *)entry + GUINT32_FROM_BE (*(sizep)));
    }
}


/* Needs write lock */
static gboolean
meta_tree_flush_locked (MetaTree *tree)
{
  MetaBuilder *builder;
  gboolean res;

  builder = meta_builder_new ();

  copy_tree_to_builder (tree, tree->root, builder->root);

  if (tree->journal)
    apply_journal_to_builder (tree, builder);

  res = meta_builder_write (builder,
			    meta_tree_get_filename (tree));
  if (res)
    meta_tree_refresh_locked (tree);

  meta_builder_free (builder);

  return res;
}

gboolean
meta_tree_flush (MetaTree *tree)
{
  gboolean res;

  g_static_rw_lock_writer_lock (&metatree_lock);
  res = meta_tree_flush_locked (tree);
  g_static_rw_lock_writer_unlock (&metatree_lock);
  return res;
}

gboolean
meta_tree_unset (MetaTree                         *tree,
		 const char                       *path,
		 const char                       *key)
{
  GString *entry;
  guint64 mtime;
  gboolean res;

  g_static_rw_lock_writer_lock (&metatree_lock);

  if (tree->journal == NULL ||
      !tree->journal->journal_valid)
    {
      res = FALSE;
      goto out;
    }

  mtime = time (NULL);

  entry = meta_journal_entry_new_unset (mtime, path, key);

  res = TRUE;
 retry:
  if (!meta_journal_add_entry (tree->journal, entry))
    {
      if (meta_tree_flush_locked (tree))
	goto retry;

      res = FALSE;
    }

  g_string_free (entry, TRUE);

 out:
  g_static_rw_lock_writer_unlock (&metatree_lock);
  return res;
}

gboolean
meta_tree_set_string (MetaTree                         *tree,
		      const char                       *path,
		      const char                       *key,
		      const char                       *value)
{
  GString *entry;
  guint64 mtime;
  gboolean res;

  g_static_rw_lock_writer_lock (&metatree_lock);

  if (tree->journal == NULL ||
      !tree->journal->journal_valid)
    {
      res = FALSE;
      goto out;
    }

  mtime = time (NULL);

  entry = meta_journal_entry_new_set (mtime, path, key, value);

  res = TRUE;
 retry:
  if (!meta_journal_add_entry (tree->journal, entry))
    {
      if (meta_tree_flush_locked (tree))
	goto retry;

      res = FALSE;
    }

  g_string_free (entry, TRUE);

 out:
  g_static_rw_lock_writer_unlock (&metatree_lock);
  return res;
}

gboolean
meta_tree_set_stringv (MetaTree                         *tree,
		       const char                       *path,
		       const char                       *key,
		       char                            **value)
{
  GString *entry;
  guint64 mtime;
  gboolean res;

  g_static_rw_lock_writer_lock (&metatree_lock);

  if (tree->journal == NULL ||
      !tree->journal->journal_valid)
    {
      res = FALSE;
      goto out;
    }

  mtime = time (NULL);

  entry = meta_journal_entry_new_setv (mtime, path, key, value);

  res = TRUE;
 retry:
  if (!meta_journal_add_entry (tree->journal, entry))
    {
      if (meta_tree_flush_locked (tree))
	goto retry;

      res = FALSE;
    }

  g_string_free (entry, TRUE);

 out:
  g_static_rw_lock_writer_unlock (&metatree_lock);
  return res;
}

gboolean
meta_tree_remove (MetaTree *tree,
		  const char *path)
{
  GString *entry;
  guint64 mtime;
  gboolean res;

  g_static_rw_lock_writer_lock (&metatree_lock);

  if (tree->journal == NULL ||
      !tree->journal->journal_valid)
    {
      res = FALSE;
      goto out;
    }

  mtime = time (NULL);

  entry = meta_journal_entry_new_remove (mtime, path);

  res = TRUE;
 retry:
  if (!meta_journal_add_entry (tree->journal, entry))
    {
      if (meta_tree_flush_locked (tree))
	goto retry;

      res = FALSE;
    }

  g_string_free (entry, TRUE);

 out:
  g_static_rw_lock_writer_unlock (&metatree_lock);
  return res;
}

gboolean
meta_tree_copy (MetaTree                         *tree,
		const char                       *src,
		const char                       *dest)
{
  GString *entry;
  guint64 mtime;
  gboolean res;

  g_static_rw_lock_writer_lock (&metatree_lock);

  if (tree->journal == NULL ||
      !tree->journal->journal_valid)
    {
      res = FALSE;
      goto out;
    }

  mtime = time (NULL);

  entry = meta_journal_entry_new_copy (mtime, src, dest);

  res = TRUE;
 retry:
  if (!meta_journal_add_entry (tree->journal, entry))
    {
      if (meta_tree_flush_locked (tree))
	goto retry;

      res = FALSE;
    }

  g_string_free (entry, TRUE);

 out:
  g_static_rw_lock_writer_unlock (&metatree_lock);
  return res;
}

static char *
canonicalize_filename (const char *filename)
{
  char *canon, *start, *p, *q;
  char *cwd;
  int i;

  if (!g_path_is_absolute (filename))
    {
      cwd = g_get_current_dir ();
      canon = g_build_filename (cwd, filename, NULL);
      g_free (cwd);
    }
  else
    canon = g_strdup (filename);

  start = (char *)g_path_skip_root (canon);

  if (start == NULL)
    {
      /* This shouldn't really happen, as g_get_current_dir() should
	 return an absolute pathname, but bug 573843 shows this is
	 not always happening */
      g_free (canon);
      return g_build_filename (G_DIR_SEPARATOR_S, filename, NULL);
    }

  /* POSIX allows double slashes at the start to
   * mean something special (as does windows too).
   * So, "//" != "/", but more than two slashes
   * is treated as "/".
   */
  i = 0;
  for (p = start - 1;
       (p >= canon) &&
	 G_IS_DIR_SEPARATOR (*p);
       p--)
    i++;
  if (i > 2)
    {
      i -= 1;
      start -= i;
      memmove (start, start+i, strlen (start+i)+1);
    }

  p = start;
  while (*p != 0)
    {
      if (p[0] == '.' && (p[1] == 0 || G_IS_DIR_SEPARATOR (p[1])))
	{
	  memmove (p, p+1, strlen (p+1)+1);
	}
      else if (p[0] == '.' && p[1] == '.' && (p[2] == 0 || G_IS_DIR_SEPARATOR (p[2])))
	{
	  q = p + 2;
	  /* Skip previous separator */
	  p = p - 2;
	  if (p < start)
	    p = start;
	  while (p > start && !G_IS_DIR_SEPARATOR (*p))
	    p--;
	  if (G_IS_DIR_SEPARATOR (*p))
	    *p++ = G_DIR_SEPARATOR;
	  memmove (p, q, strlen (q)+1);
	}
      else
	{
	  /* Skip until next separator */
	  while (*p != 0 && !G_IS_DIR_SEPARATOR (*p))
	    p++;

	  if (*p != 0)
	    {
	      /* Canonicalize one separator */
	      *p++ = G_DIR_SEPARATOR;
	    }
	}

      /* Remove additional separators */
      q = p;
      while (*q && G_IS_DIR_SEPARATOR (*q))
	q++;

      if (p != q)
	memmove (p, q, strlen (q)+1);
    }

  /* Remove trailing slashes */
  if (p > start && G_IS_DIR_SEPARATOR (*(p-1)))
    *(p-1) = 0;

  return canon;
}

/* Invariant: If link is canonical, so is result */
static char *
follow_symlink (const char *link)
{
  char *resolved, *canonical, *parent;
  char symlink_value[4096];
  ssize_t res;

  res = readlink (link, symlink_value, sizeof (symlink_value) - 1);

  if (res == -1)
    return g_strdup (link);
  symlink_value[res] = 0;

  if (g_path_is_absolute (symlink_value))
    return canonicalize_filename (symlink_value);
  else
    {
      parent = g_path_get_dirname (link);
      resolved = g_build_filename (parent, symlink_value, NULL);
      g_free (parent);

      canonical = canonicalize_filename (resolved);

      g_free (resolved);

      return canonical;
    }
}

/* Returns parent path or NULL if none */
static char *
get_dirname (const char *path)
{
  char *parent;

  parent = g_path_get_dirname (path);
  if (strcmp (parent, ".") == 0 ||
      strcmp (parent, path) == 0)
    {
      g_free (parent);
      return NULL;
    }

  return parent;
}

/* Invariant: If path is canonical, so is result */
static void
follow_symlink_recursively (char **path,
			    dev_t *path_dev)
{
  char *tmp;
  struct stat path_stat;
  int num_recursions;

  num_recursions = 0;
  do {
    if (g_lstat (*path, &path_stat) != 0)
      {
	*path_dev = 0;
	return;
      }

    if (S_ISLNK (path_stat.st_mode))
      {
	tmp = *path;
	*path = follow_symlink (*path);
	g_free (tmp);
      }

    num_recursions++;
    if (num_recursions > 12)
      break;
  } while (S_ISLNK (path_stat.st_mode));

  *path_dev = path_stat.st_dev;
}

struct _MetaLookupCache {
  char *last_parent;
  char *last_parent_expanded;
  dev_t last_parent_dev;
  char *last_parent_mountpoint;
  char *last_parent_mountpoint_extra_prefix;

  dev_t last_device;
  char *last_device_tree;
};

#ifdef HAVE_LIBUDEV

struct udev *udev;
G_LOCK_DEFINE_STATIC (udev);

static char *
get_tree_from_udev (MetaLookupCache *cache,
		    dev_t devnum)
{
  struct udev_device *dev;
  const char *uuid, *label;
  char *res;

  G_LOCK (udev);

  if (udev == NULL)
    udev = udev_new ();

  dev = udev_device_new_from_devnum (udev, 'b', devnum);
  uuid = udev_device_get_property_value (dev, "ID_FS_UUID_ENC");

  res = NULL;
  if (uuid)
    {
      res = g_strconcat ("uuid-", uuid, NULL);
    }
  else
    {
      label = udev_device_get_property_value (dev, "ID_FS_LABEL_ENC");

      if (label)
	res = g_strconcat ("label-", label, NULL);
    }

  udev_device_unref (dev);

  G_UNLOCK (udev);

  return res;
}
#endif

static const char *
get_tree_for_device (MetaLookupCache *cache,
		     dev_t device)
{
#ifdef HAVE_LIBUDEV
  if (device != cache->last_device)
    {
      cache->last_device = device;
      g_free (cache->last_device_tree);
      cache->last_device_tree = get_tree_from_udev (cache, device);
    }

  return cache->last_device_tree;
#endif
  return NULL;
}


#ifdef __linux__

typedef struct {
  char *mountpoint;
  char *root;
} MountinfoEntry;

static gboolean mountinfo_initialized = FALSE;
static int mountinfo_fd = -1;
static MountinfoEntry *mountinfo_roots = NULL;
G_LOCK_DEFINE_STATIC (mountinfo);

/* We want to avoid mmap and stat as these are not ideal
   operations for a proc file */
static char *
read_contents (int fd)
{
  char *data;
  gsize len;
  gsize bytes_read;

  len = 4096;
  data = g_malloc (len);

  bytes_read = 0;
  while (1)
    {
      gssize rc;

      if (len - bytes_read < 100)
	{
	  len = len + 4096;
	  data = g_realloc (data, len);
	}

      rc = read (fd, data + bytes_read,
		 len - bytes_read);
      if (rc < 0)
	{
	  if (errno != EINTR)
	    {
	      g_free (data);
	      return NULL;
	    }
	}
      else if (rc == 0)
	break;
      else
	bytes_read += rc;
    }

  /* zero terminate */
  if (len - bytes_read < 1)
    data = g_realloc (data, bytes_read + 1);
  data[bytes_read] = 0;

  return (char *)data;
}

static char *
mountinfo_unescape (const char *escaped)
{
  char *res, *s;
  char c;
  gsize len;

  s = strchr (escaped, ' ');
  if (s)
    len = s - escaped;
  else
    len = strlen (escaped);
  res = malloc (len + 1);
  s = res;

  while (*escaped != 0 && *escaped != ' ')
    {
      if (*escaped == '\\')
	{
	  escaped++;
	  c = *escaped++ - '0';
	  c <<= 3;
	  c |= *escaped++ - '0';
	  c <<= 3;
	  c |= *escaped++ - '0';
	}
      else
	c = *escaped++;
      *s++ = c;
    }
  *s = 0;
  return res;
}

static MountinfoEntry *
parse_mountinfo (const char *contents)
{
  GArray *a;
  const char *line;
  const char *line_root;
  const char *line_mountpoint;

  a = g_array_new (TRUE, TRUE, sizeof (MountinfoEntry));

  line = contents;
  while (line != NULL && *line != 0)
    {
      /* parent id */
      line = strchr (line, ' ');
      line_mountpoint = NULL;
      if (line)
	{
	  /* major:minor */
	  line = strchr (line+1, ' ');
	  if (line)
	    {
	      /* root */
	      line = strchr (line+1, ' ');
	      line_root = line + 1;
	      if (line)
		{
		  /* mountpoint */
		  line = strchr (line+1, ' ');
		  line_mountpoint = line + 1;
		}
	    }
	}

      if (line_mountpoint && !(line_root[0] == '/' && line_root[1] == ' '))
	{
	  MountinfoEntry new_entry;

	  new_entry.mountpoint = mountinfo_unescape (line_mountpoint);
	  new_entry.root = mountinfo_unescape (line_root);

	  g_array_append_val (a, new_entry);
	}

      line = strchr (line, '\n');
      if (line)
	line++;
    }

  return (MountinfoEntry *)g_array_free (a, FALSE);
}

static void
free_mountinfo (void)
{
  int i;

  if (mountinfo_roots)
    {
      for (i = 0; mountinfo_roots[i].mountpoint != NULL; i++)
	{
	  g_free (mountinfo_roots[i].mountpoint);
	  g_free (mountinfo_roots[i].root);
	}
      g_free (mountinfo_roots);
      mountinfo_roots = NULL;
    }
}

static void
update_mountinfo (void)
{
  char *contents;
  int res;
  gboolean first;
  GPollFD pfd;

  first = FALSE;
  if (!mountinfo_initialized)
    {
      mountinfo_initialized = TRUE;
      mountinfo_fd = open ("/proc/self/mountinfo", O_RDONLY);
      first = TRUE;
    }

  if (mountinfo_fd == -1)
    return;

  if (!first)
    {
      pfd.fd = mountinfo_fd;
      pfd.events = G_IO_IN | G_IO_OUT | G_IO_PRI;
      pfd.revents = 0;
      res = g_poll (&pfd, 1, 0);
      if (res == 0)
	return;
    }

  free_mountinfo ();
  contents = read_contents (mountinfo_fd);
  lseek (mountinfo_fd, SEEK_SET, 0);
  if (contents)
    {
      mountinfo_roots = parse_mountinfo (contents);
      g_free (contents);
    }
}

static char *
find_mountinfo_root_for_mountpoint (const char *mountpoint)
{
  char *res;
  int i;

  res = NULL;

  G_LOCK (mountinfo);

  update_mountinfo ();

  if (mountinfo_roots)
    {
      for (i = 0; mountinfo_roots[i].mountpoint != NULL; i++)
	{
	  if (strcmp (mountinfo_roots[i].mountpoint, mountpoint) == 0)
	    {
	      res = g_strdup (mountinfo_roots[i].root);
	      break;
	    }
	}
    }

  G_UNLOCK (mountinfo);

  return res;
}

#endif


static char *
get_extra_prefix_for_mount (const char *mountpoint)
{
#ifdef __linux__
  return find_mountinfo_root_for_mountpoint (mountpoint);
#endif
  return NULL;
}

static dev_t
get_devnum (const char *path)
{
  struct stat path_stat;

  if (g_lstat (path, &path_stat) != 0)
    return 0;

  return path_stat.st_dev;
}

/* Expands symlinks for parents and look for a mountpoint
 * file is symlink expanded and canonical
 */
static const char *
find_mountpoint_for (MetaLookupCache *cache,
		     const char *file,
		     dev_t       dev,
		     char      **prefix_out)
{
  char *first_dir, *dir, *last;
  const char *prefix;
  dev_t dir_dev;

  first_dir = get_dirname (file);
  if (first_dir == NULL)
    {
      *prefix_out = g_strdup ("/");
      return "/";
    }

  g_assert (cache->last_parent_expanded != NULL);
  g_assert (strcmp (cache->last_parent_expanded, first_dir) == 0);

  if (cache->last_parent_mountpoint != NULL)
    goto out; /* Cache hit! */

  dir = g_strdup (first_dir);
  last = g_strdup (file);
  while (1)
    {
      if (dir)
	dir_dev = get_devnum (dir);
      if (dir == NULL ||
	  dev != dir_dev)
	{
	  g_free (dir);
	  cache->last_parent_mountpoint = last;
	  cache->last_parent_mountpoint_extra_prefix = get_extra_prefix_for_mount (last);
	  break;
	}

      g_free (last);
      last = dir;
      dir = get_dirname (last);
    }

 out:
  g_free (first_dir);

  prefix = file + strlen (cache->last_parent_mountpoint);
  if (*prefix == 0)
    prefix = "/";

  if (cache->last_parent_mountpoint_extra_prefix)
    *prefix_out = g_build_filename (cache->last_parent_mountpoint_extra_prefix, prefix, NULL);
  else
    *prefix_out = g_strdup (prefix);

  return cache->last_parent_mountpoint;
}

/* Resolves all symlinks, including the ones for basename.
 * Invariant: If path is canonical, so is result.
 */
static char *
expand_all_symlinks (const char *path,
		     dev_t      *dev_out)
{
  char *parent, *parent_expanded;
  char *basename, *res;
  dev_t dev;
  char *path_copy;

  path_copy = g_strdup (path);
  follow_symlink_recursively (&path_copy, &dev);
  if (dev_out)
    *dev_out = dev;

  parent = get_dirname (path_copy);
  if (parent)
    {
      parent_expanded = expand_all_symlinks (parent, NULL);
      basename = g_path_get_basename (path_copy);
      res = g_build_filename (parent_expanded, basename, NULL);
      g_free (parent_expanded);
      g_free (basename);
      g_free (parent);
      g_free (path_copy);
    }
  else
    res = path_copy;

  return res;
}

MetaLookupCache *
meta_lookup_cache_new (void)
{
  MetaLookupCache *cache;

  cache = g_new0 (MetaLookupCache, 1);

  return cache;
}

void
meta_lookup_cache_free (MetaLookupCache *cache)
{
  g_free (cache->last_parent);
  g_free (cache->last_parent_expanded);
  g_free (cache->last_parent_mountpoint);
  g_free (cache->last_parent_mountpoint_extra_prefix);
  g_free (cache->last_device_tree);
  g_free (cache);
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

struct HomedirData {
  dev_t device;
  char *expanded_path;
};

static char *
expand_parents (MetaLookupCache *cache,
		const char *path,
		dev_t *parent_dev_out)
{
  char *parent;
  char *basename, *res;
  char *path_copy;
  dev_t parent_dev;

  path_copy = canonicalize_filename (path);
  parent = get_dirname (path_copy);
  if (parent == NULL)
    {
      *parent_dev_out = 0;
      return path_copy;
    }

  if (cache->last_parent == NULL ||
      strcmp (cache->last_parent, parent) != 0)
    {
      g_free (cache->last_parent);
      g_free (cache->last_parent_expanded);
      cache->last_parent = parent;
      cache->last_parent_expanded = expand_all_symlinks (parent, &parent_dev);
      cache->last_parent_dev = parent_dev;
      g_free (cache->last_parent_mountpoint);
      cache->last_parent_mountpoint = NULL;
      g_free (cache->last_parent_mountpoint_extra_prefix);
      cache->last_parent_mountpoint_extra_prefix = NULL;
   }
  else
    g_free (parent);

  *parent_dev_out = cache->last_parent_dev;
  basename = g_path_get_basename (path_copy);
  g_free (path_copy);
  res = g_build_filename (cache->last_parent_expanded, basename, NULL);
  g_free (basename);

  return res;
}

MetaTree *
meta_lookup_cache_lookup_path (MetaLookupCache *cache,
			       const char *filename,
			       guint64 device,
			       gboolean for_write,
			       char **tree_path)
{
  const char *mountpoint;
  const char *treename;
  char *prefix;
  char *expanded;
  static struct HomedirData homedir_data_storage;
  static gsize homedir_datap = 0;
  struct HomedirData *homedir_data;
  MetaTree *tree;
  dev_t parent_dev;

  if (g_once_init_enter (&homedir_datap))
    {
      char *e;
      struct stat statbuf;

      g_stat (g_get_home_dir(), &statbuf);
      homedir_data_storage.device = statbuf.st_dev;
      e = canonicalize_filename (g_get_home_dir());
      homedir_data_storage.expanded_path = expand_all_symlinks (e, NULL);
      g_free (e);
      g_once_init_leave (&homedir_datap, (gsize)&homedir_data_storage);
    }
  homedir_data = (struct HomedirData *)homedir_datap;

  /* Canonicalized form with all symlinks expanded in parents */
  expanded = expand_parents (cache, filename, &parent_dev);

  if (device == 0) /* Unknown, use same as parent */
    device = parent_dev;

  if (homedir_data->device == device &&
      path_has_prefix (expanded, homedir_data->expanded_path))
    {
      treename = "home";
      prefix = expanded + strlen (homedir_data->expanded_path);
      if (*prefix == 0)
	prefix = g_strdup ("/");
      else
	prefix = g_strdup (prefix);
      goto found;
    }

  treename = get_tree_for_device (cache, device);

  if (treename)
    {
      mountpoint = find_mountpoint_for (cache,
					expanded,
					device,
					&prefix);

      if (mountpoint == NULL ||
	  strcmp (mountpoint, "/") == 0)
	{
	  /* Fall back to root */
	  g_free (prefix);
	  treename = NULL;
	}
    }

  if (!treename)
    {
      treename = "root";
      prefix = g_strdup (expanded);
    }

 found:
  g_free (expanded);
  tree = meta_tree_lookup_by_name (treename, for_write);
  if (tree)
    {
      *tree_path = prefix;
      return tree;
    }

  g_free (prefix);
  return NULL;
}
