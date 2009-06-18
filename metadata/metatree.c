#include "config.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>

#include "metatree.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <poll.h>
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
  int refcount;
  char *filename;
  gboolean for_write;

  int fd;
  char *data;
  gsize len;

  guint32 tag;
  gint64 time_t_base;
  MetaFileHeader *header;
  MetaFileDirEnt *root;

  int num_attributes;
  char **attributes;

  MetaJournal *journal;
};

static MetaJournal *meta_journal_open (const char  *filename,
				       gboolean     for_write,
				       guint32      tag);
static void         meta_journal_free (MetaJournal *journal);

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
  if (tree->fd != 0)
    {
      close (tree->fd);
      tree->fd = 0;
    }
}

static gboolean
meta_tree_init (MetaTree *tree)
{
  struct stat statbuf;
  int fd;
  void *data;
  guint32 *attributes;
  int i;

  fd = open (tree->filename, O_RDONLY);
  if (fd == -1)
    return FALSE;

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

  tree->journal = meta_journal_open (tree->filename, tree->for_write, tree->tag);

  /* There is a race with tree replacing, where the journal could have been
     deleted (and the tree replaced) inbetween opening the tree file and the
     journal. However we can detect this case by looking at the tree and see
     if its been rotated, we do this to ensure we have an uptodate tree+journal
     combo. */
  meta_tree_refresh (tree);

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
  tree->refcount = 1;
  tree->filename = g_strdup (filename);
  tree->for_write = for_write;

  if (!meta_tree_init (tree))
    {
      meta_tree_unref (tree);
      return NULL;
    }
  return tree;
}

MetaTree *
meta_tree_ref (MetaTree *tree)
{
  tree->refcount++;
  return tree;
}

void
meta_tree_unref (MetaTree *tree)
{
  tree->refcount--;
  if (tree->refcount == 0)
    {
      meta_tree_clear (tree);
      g_free (tree->filename);
      g_free (tree);
    }
}

void
meta_tree_refresh (MetaTree *tree)
{
  if (tree->header != NULL &&
      GUINT32_FROM_BE (tree->header->rotated) == 0)
    return; /* Got a valid tree and its not rotated */

  if (tree->header)
    meta_tree_clear (tree);
  meta_tree_init (tree);
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

  real_crc32 = crc32 (journal->data + offset + 8, entry_len - 8);
  if (real_crc32 != GUINT32_FROM_BE (entry->crc32))
    return NULL;

  return (MetaJournalEntry *)(journal->data + offset + entry_len);
}

/* Try to validate more entries */
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
  set_uint32 (out, 4, crc32 (out->str + 8, len - 8));
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
meta_journal_open (const char *filename, gboolean for_write, guint32 tag)
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

  fd = open (journal_filename, open_flags);
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
					  const char *key,
					  gpointer value,
					  char **iter_path,
					  gpointer user_data);
typedef gboolean (*journal_path_callback) (MetaJournal *journal,
					   MetaJournalEntryType entry_type,
					   const char *path,
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

  path_copy = g_strdup (path);

  if (journal == NULL)
    return path_copy;

  entry = journal->last_entry;
  while (entry > journal->first_entry)
    {
      sizep = (guint32 *)entry;
      entry = (MetaJournalEntry *)((char *)entry - GUINT32_FROM_BE (*(sizep-1)));

      journal_path = &entry->path[0];

      if (journal_entry_is_key_type (entry) &&
	  key_callback) /* set, setv or unset */
	{
	  journal_key = get_next_arg (journal_path);
	  value = get_next_arg (journal_key);

	  /* Only affects is path is exactly the same */
	  res = key_callback (journal, entry->entry_type,
			      journal_path, journal_key,
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
			       journal_path, source_path,
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
  gpointer value;
} PathKeyData;

static gboolean
journal_iter_key (MetaJournal *journal,
		  MetaJournalEntryType entry_type,
		  const char *path,
		  const char *key,
		  gpointer value,
		  char **iter_path,
		  gpointer user_data)
{
  PathKeyData *data = user_data;

  if (strcmp (path, *iter_path) != 0)
    return TRUE; /* No match, continue */

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

  new_path = meta_journal_reverse_map_path_and_key (tree->journal,
						    path,
						    key,
						    &type, &value);
  if (new_path == NULL)
    return type;

  data = meta_tree_lookup_data (tree, new_path);
  ent = NULL;
  if (data)
    ent = meta_data_get_key (tree, data, key);

  g_free (new_path);

  if (ent == NULL)
    return META_KEY_TYPE_NONE;
  if (GUINT32_FROM_BE (ent->key) & KEY_IS_LIST_MASK)
    return META_KEY_TYPE_STRINGV;
  else
    return META_KEY_TYPE_STRING;
}

guint64
meta_tree_get_last_changed (MetaTree                         *tree,
			    const char                       *path)
{
  /* TODO */
  return 0;
}

char *
meta_tree_lookup_string    (MetaTree                         *tree,
			    const char                       *path,
			    const char                       *key)
{
  MetaFileData *data;
  MetaFileDataEnt *ent;
  MetaKeyType type;
  gpointer value;
  char *new_path;

  new_path = meta_journal_reverse_map_path_and_key (tree->journal,
						    path,
						    key,
						    &type, &value);
  if (new_path == NULL)
    {
      if (type == META_KEY_TYPE_STRING)
	return g_strdup (value);
      return NULL;
    }

  data = meta_tree_lookup_data (tree, new_path);
  ent = NULL;
  if (data)
    ent = meta_data_get_key (tree, data, key);

  g_free (new_path);

  if (ent == NULL)
    return NULL;
  if (ent->key & KEY_IS_LIST_MASK)
    return NULL;
  return verify_string (tree, ent->value);
}

char **
meta_tree_lookup_stringv   (MetaTree                         *tree,
			    const char                       *path,
			    const char                       *key)
{
  /* TODO */
  return NULL;
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
	    info->last_changed = 0; /*TODO*/
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
		info->last_changed = 0; /*TODO*/
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
  g_hash_table_destroy (children);
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
  guint32 i, num_keys;
  MetaFileDataEnt *ent;
  EnumKeysInfo *info;
  char *key_name;
  guint32 key_id;
  MetaKeyType type;
  gpointer value;

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

      if (type == META_KEY_TYPE_STRING)
	value = verify_string (tree, ent->value);
      else
	{
	  value = NULL;
	  g_print ("TODO: Handle stringv metadata from tree\n");
	}

      if (!callback (key_name,
		     type,
		     value,
		     user_data))
	return FALSE;
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
	  value = NULL;
	  g_print ("TODO: Handle stringv metadata from journal\n");
	}

      if (!callback (info->key,
		     info->type,
		     value,
		     user_data))
	break;
    }
 out:
  g_hash_table_destroy (keys);
}

gboolean
meta_tree_flush (MetaTree *tree)
{
  /* TODO: roll over */
  return FALSE;
}

gboolean
meta_tree_unset (MetaTree                         *tree,
		 const char                       *path,
		 const char                       *key)
{
  return FALSE;
}

gboolean
meta_tree_set_string (MetaTree                         *tree,
		      const char                       *path,
		      const char                       *key,
		      const char                       *value)
{
  GString *entry;
  guint64 mtime;

  if (tree->journal == NULL ||
      !tree->journal->journal_valid)
    return FALSE;

  mtime = time (NULL);

  entry = meta_journal_entry_new_set (mtime, path, key, value);

 retry:
  if (!meta_journal_add_entry (tree->journal, entry))
    {
      if (meta_tree_flush (tree))
	goto retry;

      g_string_free (entry, TRUE);
      return FALSE;
    }

  g_string_free (entry, TRUE);
  return TRUE;
}

gboolean
meta_tree_set_stringv (MetaTree                         *tree,
		       const char                       *path,
		       const char                       *key,
		       const char                      **value)
{
  return FALSE;
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
    return NULL;

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
  char *last_parent_mountpoint;

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
  struct pollfd pfd;

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
      pfd.events = POLLIN | POLLOUT | POLLPRI;
      pfd.revents = 0;
      res = poll (&pfd, 1, 0);
      if (res == 0)
	return;
    }

  free_mountinfo ();
  contents = read_contents (mountinfo_fd);
  lseek (mountinfo_fd, SEEK_SET, 0);
  if (contents)
    mountinfo_roots = parse_mountinfo (contents);
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
  char *extra_prefix;

  first_dir = get_dirname (file);

  g_assert (cache->last_parent_expanded != NULL);
  g_assert (strcmp (cache->last_parent_expanded, first_dir) == 0);

  if (cache->last_parent_mountpoint != NULL)
    goto out; /* Cache hit! */

  dir = g_strdup (first_dir);
  last = g_strdup (file);
  while (1)
    {
      dir_dev = get_devnum (dir);
      if (dir == NULL ||
	  dev != dir_dev)
	{
	  g_free (dir);
	  cache->last_parent_mountpoint = last;
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

  extra_prefix = get_extra_prefix_for_mount (cache->last_parent_mountpoint);
  if (extra_prefix)
    {
      *prefix_out = g_build_filename (extra_prefix, prefix, NULL);
      g_free (extra_prefix);
    }
  else
    *prefix_out = g_strdup (prefix);

  return cache->last_parent_mountpoint;
}

/* Resolves all symlinks, including the ones for basename.
 * Invariant: If path is canonical, so is result.
 */
static char *
expand_all_symlinks (const char *path)
{
  char *parent, *parent_expanded;
  char *basename, *res;
  dev_t dev;
  char *path_copy;

  path_copy = g_strdup (path);
  follow_symlink_recursively (&path_copy, &dev);

  parent = get_dirname (path_copy);
  if (parent)
    {
      parent_expanded = expand_all_symlinks (parent);
      basename = g_path_get_basename (path_copy);
      res = g_build_filename (parent_expanded, basename, NULL);
      g_free (parent_expanded);
      g_free (basename);
      g_free (parent);
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
		const char *path)
{
  char *parent;
  char *basename, *res;
  char *path_copy;

  path_copy = canonicalize_filename (path);
  parent = g_path_get_dirname (path_copy);
  if (strcmp (parent, ".") == 0 ||
      strcmp (parent, path_copy) == 0)
    {
      g_free (parent);
      return path_copy;
    }

  if (cache->last_parent == NULL ||
      strcmp (cache->last_parent, parent) != 0)
    {
      g_free (cache->last_parent);
      cache->last_parent = parent;
      cache->last_parent_expanded = expand_all_symlinks (parent);
      g_free (cache->last_parent_mountpoint);
      cache->last_parent_mountpoint = NULL;
   }
  else
    g_free (parent);

  basename = g_path_get_basename (path_copy);
  res = g_build_filename (cache->last_parent_expanded, basename, NULL);
  g_free (basename);

  return res;
}

MetaTree *
meta_lookup_cache_lookup_path (MetaLookupCache *cache,
			       const char *filename,
			       guint64 device)
{
  const char *mountpoint;
  const char *treename;
  char *prefix;
  char *expanded;
  static struct HomedirData homedir_data_storage;
  static gsize homedir_datap = 0;
  struct HomedirData *homedir_data;
  char *extra_prefix;

  if (g_once_init_enter (&homedir_datap))
    {
      char *e;
      struct stat statbuf;

      g_stat (g_get_home_dir(), &statbuf);
      homedir_data_storage.device = statbuf.st_dev;
      e = canonicalize_filename (g_get_home_dir());
      homedir_data_storage.expanded_path = expand_all_symlinks (e);
      g_free (e);
      g_once_init_leave (&homedir_datap, (gsize)&homedir_data_storage);
    }
  homedir_data = (struct HomedirData *)homedir_datap;

  /* Canonicalized form with all symlinks expanded in parents */
  expanded = expand_parents (cache, filename);

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
  g_print ("Found tree %s:%s\n", treename, prefix);

  return NULL;
}
