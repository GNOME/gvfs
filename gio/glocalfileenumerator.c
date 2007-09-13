#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <glocalfileenumerator.h>
#include <glocalfileinfo.h>

  /* TODO:
   *  It would be nice to use the dirent->d_type to check file type without
   *  needing to stat each files on linux and other systems that support it.
   *  (question: does that following symlink or not?)
   */
  

struct _GLocalFileEnumerator
{
  GFileEnumerator parent;

  GFileAttributeMatcher *matcher;
  GDir *dir;
  char *filename;
  char *attributes;
  GFileGetInfoFlags flags;

  gboolean got_parent_info;
  GLocalParentFileInfo parent_info;
  
  gboolean follow_symlinks;
};

G_DEFINE_TYPE (GLocalFileEnumerator, g_local_file_enumerator, G_TYPE_FILE_ENUMERATOR);

static GFileInfo *g_local_file_enumerator_next_file (GFileEnumerator  *enumerator,
						     GCancellable     *cancellable,
						     GError          **error);
static gboolean   g_local_file_enumerator_stop      (GFileEnumerator  *enumerator,
						     GCancellable     *cancellable,
						     GError          **error);


static void
g_local_file_enumerator_finalize (GObject *object)
{
  GLocalFileEnumerator *local;

  local = G_LOCAL_FILE_ENUMERATOR (object);

  g_free (local->filename);
  g_file_attribute_matcher_free (local->matcher);
  if (local->dir)
    {
      g_dir_close (local->dir);
      local->dir = NULL;
    }
  
  if (G_OBJECT_CLASS (g_local_file_enumerator_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_local_file_enumerator_parent_class)->finalize) (object);
}


static void
g_local_file_enumerator_class_init (GLocalFileEnumeratorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS (klass);
  
  gobject_class->finalize = g_local_file_enumerator_finalize;

  enumerator_class->next_file = g_local_file_enumerator_next_file;
  enumerator_class->stop = g_local_file_enumerator_stop;
}

static void
g_local_file_enumerator_init (GLocalFileEnumerator *local)
{
}

GFileEnumerator *
g_local_file_enumerator_new (const char *filename,
			     const char *attributes,
			     GFileGetInfoFlags flags,
			     GCancellable *cancellable,
			     GError **error)
{
  GLocalFileEnumerator *local;
  GDir *dir;

  dir = g_dir_open (filename, 0, error);
  if (dir == NULL)
    return NULL;
  
  local = g_object_new (G_TYPE_LOCAL_FILE_ENUMERATOR, NULL);

  local->dir = dir;
  local->filename = g_strdup (filename);
  local->matcher = g_file_attribute_matcher_new (attributes);
  local->flags = flags;
  
  return G_FILE_ENUMERATOR (local);
}

static GFileInfo *
g_local_file_enumerator_next_file (GFileEnumerator *enumerator,
				   GCancellable     *cancellable,
				   GError **error)
{
  GLocalFileEnumerator *local = G_LOCAL_FILE_ENUMERATOR (enumerator);
  const char *filename;
  char *path;
  GFileInfo *info;
  GError *my_error = NULL;

  if (!local->got_parent_info)
    {
      _g_local_file_info_get_parent_info (local->filename, local->matcher, &local->parent_info);
      local->got_parent_info = TRUE;
    }
  
 next_file:
  
  filename = g_dir_read_name (local->dir);
  if (filename == NULL)
    return NULL;

  path = g_build_filename (local->filename, filename, NULL);
  info = _g_local_file_info_get (filename, path,
				 local->matcher,
				 local->flags,
				 &local->parent_info,
				 &my_error); 
  g_free (path);
  
  if (info == NULL)
    {
      /* Failed to get info */
      /* If the file does not exist there might have been a race where
       * the file was removed between the readdir and the stat, so we
       * ignore the file. */
      if (my_error->domain == G_IO_ERROR &&
	  my_error->code == G_IO_ERROR_NOT_FOUND)
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
g_local_file_enumerator_stop (GFileEnumerator *enumerator,
			      GCancellable     *cancellable,
			      GError          **error)
{
  GLocalFileEnumerator *local = G_LOCAL_FILE_ENUMERATOR (enumerator);

  if (local->dir)
    {
      g_dir_close (local->dir);
      local->dir = NULL;
    }

  return TRUE;
}


