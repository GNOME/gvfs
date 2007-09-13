#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gfileenumeratorsimple.h>
#include <gfileinfosimple.h>

G_DEFINE_TYPE (GFileEnumeratorSimple, g_file_enumerator_simple, G_TYPE_FILE_ENUMERATOR);

  /* TODO:
   *  It would be nice to use the dirent->d_type to check file type without
   *  needing to stat each files on linux and other systems that support it.
   *  (question: does that following symlink or not?)
   */
  

struct _GFileEnumeratorSimple
{
  GFileEnumerator parent;

  GDir *dir;
  char *filename;
  GFileInfoRequestFlags requested;
  char *attributes;
  gboolean follow_symlinks;
};

static void
g_file_enumerator_simple_finalize (GObject *object)
{
  GFileEnumeratorSimple *simple;

  simple = G_FILE_ENUMERATOR_SIMPLE (object);

  g_free (simple->filename);
  g_free (simple->attributes);
  
  if (simple->open_error)
    g_error_free (simple->open_error);
  
  if (G_OBJECT_CLASS (g_file_enumerator_simple_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_enumerator_simple_parent_class)->finalize) (object);
}


static void
g_file_enumerator_simple_class_init (GFileEnumeratorSimpleClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS (klass);
  
  gobject_class->finalize = g_file_enumerator_simple_finalize;
}

static void
g_file_enumerator_simple_init (GFileEnumeratorSimple *simple)
{
}

GFileEnumerator *
g_file_enumerator_simple_new (const char *filename,
			      GFileInfoRequestFlags requested,
			      const char *attributes,
			      gboolean follow_symlinks)
{
  GFileEnumeratorSimple *simple;

  simple = g_object_new (G_TYPE_FILE_ENUMERATOR_SIMPLE, NULL);

  simple->filename = g_strdup (filename);
  simple->requested = requested;
  simple->attributes = g_strdup (attributes);
  simple->follow_symlinks = follow_symlinks;
  
  return G_FILE_ENUMERATOR_SIMPLE (simple);
}

static gboolean
g_file_enumerator_simple_open_dir (GFileEnumeratorSimple *simple, GError **error)
{
  if (simple->dir != NULL)
    return TRUE;
  
  simple->dir = g_dir_open (simple->filename, error);
  return simple->dir != NULL;
}

GFileInfo *
g_file_enumerator_simple_next_file (GFileEnumerator *enumerator,
				    GError **error)
{
  GFileEnumeratorSimple *simple = G_FILE_ENUMERATOR_SIMPLE (enumerator);
  const char *filename;
  char *path;
  GFileInfo *info;
  
  if (!g_file_enumerator_simple_open_dir (simple, error))
    return NULL;

  filename = g_dir_read_name (simple->dir);
  if (filename == NULL)
    {
      if (error)
	*error = NULL;
      return NULL
    }

  info = g_file_info_new ();
  g_file_info_set_name (info, filename);
  
  /* Avoid stat in trivial case */
  if (simple->requested != G_FILE_INFO_NAME ||
      simple->attributes != NULL)
    {
      path = g_build_filename (simple->filename, filename, NULL);
      
      g_file_info_simple_get (path, info,
			      requested, attributes, follow_symlinks);
      
      g_free (path);
    }

  return info;
}

void
g_file_enumerator_simple_stop (GFileEnumerator *enumerator)
{
  GFileEnumeratorSimple *simple = G_FILE_ENUMERATOR_SIMPLE (enumerator);

  g_dir_close (simple->dir);
  simple->dir = NULL;
}


