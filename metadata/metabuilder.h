#include <glib.h>

typedef struct _MetaBuilder MetaBuilder;
typedef struct _MetaFile MetaFile;
typedef struct _MetaData MetaData;

struct _MetaBuilder {
  MetaFile *root;

  guint32 root_pointer;
  gint64 time_t_base;
};

struct _MetaFile {
  char *name;
  GList *children;
  gint64 last_changed;
  GList *data;

  guint32 metadata_pointer;
  guint32 children_pointer;
};

struct _MetaData {
  char *key;
  gboolean is_list;
  char *value;
  GList *values;
};

MetaBuilder *meta_builder_new       (void);
void         meta_builder_print     (MetaBuilder *builder);
MetaFile *   meta_builder_lookup    (MetaBuilder *builder,
				     const char  *path,
				     gboolean     create);
gboolean     meta_builder_write     (MetaBuilder *builder,
				     const char  *filename);
MetaFile *   metafile_new           (const char  *name,
				     MetaFile    *parent);
MetaFile *   metafile_lookup_child  (MetaFile    *metafile,
				     const char  *name,
				     gboolean     create);
MetaData *   metafile_key_lookup    (MetaFile    *file,
				     const char  *key,
				     gboolean     create);
void         metafile_key_set_value (MetaFile    *metafile,
				     const char  *key,
				     const char  *value);
void         metafile_key_list_add  (MetaFile    *metafile,
				     const char  *key,
				     const char  *value);
