#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include "gfilelocal.h"
#include "gfileinfolocal.h"
#include "gfileenumeratorlocal.h"
#include "gfileinputstreamlocal.h"
#include "gfileoutputstreamlocal.h"
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>

static void g_file_local_file_iface_init (GFileIface       *iface);

struct _GFileLocal
{
  GObject parent_instance;

  char *filename;
};

G_DEFINE_TYPE_WITH_CODE (GFileLocal, g_file_local, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						g_file_local_file_iface_init))

static void
g_file_local_finalize (GObject *object)
{
  GFileLocal *local;

  local = G_FILE_LOCAL (object);

  g_free (local->filename);
  
  if (G_OBJECT_CLASS (g_file_local_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_local_parent_class)->finalize) (object);
}

static void
g_file_local_class_init (GFileLocalClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_file_local_finalize;
}

static void
g_file_local_init (GFileLocal *local)
{
}

GFile *
g_file_local_new (const char *filename)
{
  GFileLocal *local;
  char *non_root;
  int len;

  local = g_object_new (G_TYPE_FILE_LOCAL, NULL);
  local->filename = g_strdup (filename);

  /* Remove any trailing slashes */
  non_root = (char *)g_path_skip_root (local->filename);
  if (non_root != NULL)
    {
      len = strlen (non_root);
      while (len > 0 &&
	     G_IS_DIR_SEPARATOR (non_root[len-1]))
	{
	  non_root[len-1] = 0;
	  len--;
	}
    }
  
  return G_FILE (local);
}

static gboolean
g_file_local_is_native (GFile *file)
{
  return TRUE;
}

static char *
g_file_local_get_path (GFile *file)
{
  return g_strdup (G_FILE_LOCAL (file)->filename);
}

static char *
g_file_local_get_uri (GFile *file)
{
  return g_filename_to_uri (G_FILE_LOCAL (file)->filename, NULL, NULL);
}

static gboolean
get_filename_charset (const gchar **filename_charset)
{
  const gchar **charsets;
  gboolean is_utf8;
  
  is_utf8 = g_get_filename_charsets (&charsets);

  if (filename_charset)
    *filename_charset = charsets[0];
  
  return is_utf8;
}

static gboolean
name_is_valid_for_display (const char *string,
			   gboolean is_valid_utf8)
{
  char c;
  
  if (!is_valid_utf8 &&
      !g_utf8_validate (string, -1, NULL))
    return FALSE;

  while ((c = *string++) != 0)
    {
      if (g_ascii_iscntrl(c))
	return FALSE;
    }

  return TRUE;
}

static char *
g_file_local_get_parse_name (GFile *file)
{
  const char *filename;
  char *parse_name;
  const gchar *charset;
  char *utf8_filename;
  char *roundtripped_filename;
  gboolean free_utf8_filename;
  gboolean is_valid_utf8;

  filename = G_FILE_LOCAL (file)->filename;
  if (get_filename_charset (&charset))
    {
      utf8_filename = (char *)filename;
      free_utf8_filename = FALSE;
      is_valid_utf8 = FALSE; /* Can't guarantee this */
    }
  else
    {
      utf8_filename = g_convert (filename, -1, 
				 "UTF-8", charset, NULL, NULL, NULL);
      free_utf8_filename = TRUE;
      is_valid_utf8 = TRUE;

      if (utf8_filename != NULL)
	{
	  /* Make sure we can roundtrip: */
	  roundtripped_filename = g_convert (utf8_filename, -1,
					     charset, "UTF-8", NULL, NULL, NULL);
	  
	  if (roundtripped_filename == NULL ||
	      strcmp (utf8_filename, roundtripped_filename) != 0)
	    {
	      g_free (utf8_filename);
	      utf8_filename = NULL;
	    }
	}
    }


  if (utf8_filename != NULL &&
      name_is_valid_for_display (utf8_filename, is_valid_utf8))
    {
      if (free_utf8_filename)
	parse_name = utf8_filename;
      else
	parse_name = g_strdup (utf8_filename);
    }
  else
    {
      parse_name = g_filename_to_uri (filename, NULL, NULL);
      if (free_utf8_filename)
	g_free (utf8_filename);
    }
  
  return parse_name;
}

static GFile *
g_file_local_get_parent (GFile *file)
{
  GFileLocal *local = G_FILE_LOCAL (file);
  const char *non_root;
  char *dirname;
  GFile *parent;

  /* Check for root */
  non_root = g_path_skip_root (local->filename);
  if (*non_root == 0)
    return NULL;

  dirname = g_path_get_dirname (local->filename);
  parent = g_file_local_new (dirname);
  g_free (dirname);
  return parent;
}

static GFile *
g_file_local_copy (GFile *file)
{
  GFileLocal *local = G_FILE_LOCAL (file);

  return g_file_local_new (local->filename);
}


static GFile *
g_file_local_get_child (GFile *file,
			const char *name)
{
  GFileLocal *local = G_FILE_LOCAL (file);
  char *filename;
  GFile *child;

  filename = g_build_filename (local->filename, name, NULL);

  child = g_file_local_new (filename);
  g_free (filename);
  
  return child;
}

static GFileEnumerator *
g_file_local_enumerate_children (GFile      *file,
				 GFileInfoRequestFlags requested,
				 const char *attributes,
				 gboolean follow_symlinks)
{
  GFileLocal *local = G_FILE_LOCAL (file);
  return g_file_enumerator_local_new (local->filename,
				       requested, attributes,
				       follow_symlinks);
}

static GFileInfo *
g_file_local_get_info (GFile                *file,
		       GFileInfoRequestFlags requested,
		       const char           *attributes,
		       gboolean              follow_symlinks,
		       GCancellable         *cancellable,
		       GError              **error)
{
  GFileLocal *local = G_FILE_LOCAL (file);
  GFileInfo *info;
  GFileAttributeMatcher *matcher;
  char *basename;

  matcher = g_file_attribute_matcher_new (attributes);
  
  basename = g_path_get_basename (local->filename);
  
  info = g_file_info_local_get (basename, local->filename,
				 requested, matcher, follow_symlinks,
				 error);
  
  g_free (basename);

  g_file_attribute_matcher_free (matcher);

  return info;
}

static GFileInputStream *
g_file_local_read (GFile *file,
		   GCancellable *cancellable,
		   GError **error)
{
  GFileLocal *local = G_FILE_LOCAL (file);
  int fd;
  
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }

  fd = g_open (local->filename, O_RDONLY, 0);
  if (fd == -1)
    {
      g_set_error (error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error opening file %s: %s"),
		   local->filename, g_strerror (errno));
      return NULL;
    }
  
  return g_file_input_stream_local_new (fd);
}

static GFileOutputStream *
g_file_local_append_to (GFile *file,
			GCancellable *cancellable,
			GError **error)
{
  return g_file_output_stream_local_append (G_FILE_LOCAL (file)->filename,
					    cancellable, error);
}

static GFileOutputStream *
g_file_local_create (GFile *file,
		     GCancellable *cancellable,
		     GError **error)
{
  return g_file_output_stream_local_create (G_FILE_LOCAL (file)->filename,
					    cancellable, error);
}

static GFileOutputStream *
g_file_local_replace (GFile *file,
		      time_t mtime,
		      gboolean make_backup,
		      GCancellable *cancellable,
		      GError **error)
{
  return g_file_output_stream_local_replace (G_FILE_LOCAL (file)->filename,
					     mtime, make_backup,
					     cancellable, error);
}

static void
g_file_local_file_iface_init (GFileIface *iface)
{
  iface->copy = g_file_local_copy;
  iface->is_native = g_file_local_is_native;
  iface->get_path = g_file_local_get_path;
  iface->get_uri = g_file_local_get_uri;
  iface->get_parse_name = g_file_local_get_parse_name;
  iface->get_parent = g_file_local_get_parent;
  iface->get_child = g_file_local_get_child;
  iface->enumerate_children = g_file_local_enumerate_children;
  iface->get_info = g_file_local_get_info;
  iface->read = g_file_local_read;
  iface->append_to = g_file_local_append_to;
  iface->create = g_file_local_create;
  iface->replace = g_file_local_replace;
}
