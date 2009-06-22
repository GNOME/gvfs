#include <glib.h>

typedef struct _MetaTree MetaTree;

typedef enum {
  META_KEY_TYPE_NONE,
  META_KEY_TYPE_STRING,
  META_KEY_TYPE_STRINGV
} MetaKeyType;

typedef gboolean (*meta_tree_dir_enumerate_callback) (const char *entry,
						      guint64 last_changed,
						      gboolean has_children,
						      gboolean has_data,
						      gpointer user_data);

typedef gboolean (*meta_tree_keys_enumerate_callback) (const char *key,
						       MetaKeyType type,
						       gpointer value,
						       gpointer user_data);

void      meta_tree_free    (MetaTree   *tree);
MetaTree *meta_tree_open    (const char *filename,
			     gboolean    for_write);
void      meta_tree_refresh (MetaTree   *tree); /* May invalidates all strings */


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
					const char                      **value);
