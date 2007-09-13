#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gfileenumeratorlocal.h>
#include <gfileinfolocal.h>

  /* TODO:
   *  It would be nice to use the dirent->d_type to check file type without
   *  needing to stat each files on linux and other systems that support it.
   *  (question: does that following symlink or not?)
   */
  

struct _GFileEnumeratorLocal
{
  GFileEnumerator parent;

  GFileAttributeMatcher *matcher;
  GDir *dir;
  char *filename;
  GFileInfoRequestFlags requested;
  char *attributes;
  gboolean follow_symlinks;
};

G_DEFINE_TYPE (GFileEnumeratorLocal, g_file_enumerator_local, G_TYPE_FILE_ENUMERATOR);

static GFileInfo *g_file_enumerator_local_next_file (GFileEnumerator  *enumerator,
						     GCancellable     *cancellable,
						     GError          **error);
static gboolean   g_file_enumerator_local_stop      (GFileEnumerator  *enumerator,
						     GCancellable     *cancellable,
						     GError          **error);


static void
g_file_enumerator_local_finalize (GObject *object)
{
  GFileEnumeratorLocal *local;

  local = G_FILE_ENUMERATOR_LOCAL (object);

  g_free (local->filename);
  g_file_attribute_matcher_free (local->matcher);
  if (local->dir)
    {
      g_dir_close (local->dir);
      local->dir = NULL;
    }
  
  if (G_OBJECT_CLASS (g_file_enumerator_local_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_enumerator_local_parent_class)->finalize) (object);
}


static void
g_file_enumerator_local_class_init (GFileEnumeratorLocalClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS (klass);
  
  gobject_class->finalize = g_file_enumerator_local_finalize;

  enumerator_class->next_file = g_file_enumerator_local_next_file;
  enumerator_class->stop = g_file_enumerator_local_stop;
}

static void
g_file_enumerator_local_init (GFileEnumeratorLocal *local)
{
}

GFileEnumerator *
g_file_enumerator_local_new (const char *filename,
			     GFileInfoRequestFlags requested,
			     const char *attributes,
			     gboolean follow_symlinks,
			     GCancellable *cancellable,
			     GError **error)
{
  GFileEnumeratorLocal *local;
  GDir *dir;

  dir = g_dir_open (filename, 0, error);
  if (dir == NULL)
    return NULL;
  
  local = g_object_new (G_TYPE_FILE_ENUMERATOR_LOCAL, NULL);

  local->dir = dir;
  local->filename = g_strdup (filename);
  local->requested = requested;
  local->matcher = g_file_attribute_matcher_new (attributes);
  local->follow_symlinks = follow_symlinks;
  
  return G_FILE_ENUMERATOR (local);
}

static GFileInfo *
g_file_enumerator_local_next_file (GFileEnumerator *enumerator,
				   GCancellable     *cancellable,
				   GError **error)
{
  GFileEnumeratorLocal *local = G_FILE_ENUMERATOR_LOCAL (enumerator);
  const char *filename;
  char *path;
  GFileInfo *info;
  GError *my_error = NULL;
  
 next_file:
  
  filename = g_dir_read_name (local->dir);
  if (filename == NULL)
    return NULL;

  path = g_build_filename (local->filename, filename, NULL);
  info = g_file_info_local_get (filename, path,
				local->requested,
				local->matcher,
				local->follow_symlinks,
				&my_error); 
  g_free (path);
  
  if (info == NULL)
    {
      /* Failed to get info */
      /* If the file does not exist there might have been a race where
       * the file was removed between the readdir and the stat, so we
       * ignore the file. */
      if (my_error->domain == G_FILE_ERROR &&
	  my_error->code == G_FILE_ERROR_NOENT)
	{
	  g_error_free (my_error);
	  goto next_file;
	}
      else
	g_propagate_error (error, my_error);
    }

  return info;
}

static gboolean
g_file_enumerator_local_stop (GFileEnumerator *enumerator,
			      GCancellable     *cancellable,
			      GError          **error)
{
  GFileEnumeratorLocal *local = G_FILE_ENUMERATOR_LOCAL (enumerator);

  if (local->dir)
    {
      g_dir_close (local->dir);
      local->dir = NULL;
    }

  return TRUE;
}


