#include <config.h>

#include <string.h>
#include <unistd.h>

#include "gfilesimple.h"
#include "glocalfileinputstream.h"
#include "glocalfileoutputstream.h"
#include <glib/gi18n-lib.h>

static void g_file_simple_file_iface_init (GFileIface       *iface);


G_DEFINE_TYPE_WITH_CODE (GFileSimple, g_file_simple, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						g_file_simple_file_iface_init))

static void
g_file_simple_finalize (GObject *object)
{
  GFileSimple *simple;

  simple = G_FILE_SIMPLE (object);

  g_free (simple->filename);
  
  if (G_OBJECT_CLASS (g_file_simple_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_simple_parent_class)->finalize) (object);
}

static void
g_file_simple_class_init (GFileSimpleClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_file_simple_finalize;
}

static void
g_file_simple_init (GFileSimple *simple)
{
}

GFile *
g_file_simple_new (const char *filename)
{
  GFileSimple *simple;
  char *non_root;
  int len;

  simple = g_object_new (G_TYPE_FILE_SIMPLE, NULL);
  simple->filename = g_strdup (filename);

  /* Remove any trailing slashes */
  non_root = (char *)g_path_skip_root (simple->filename);
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
  
  return G_FILE (simple);
}

gboolean
g_file_simple_is_native (GFile *file)
{
  return TRUE;
}

char *
g_file_simple_get_path (GFile *file)
{
  return g_strdup (G_FILE_SIMPLE (file)->filename);
}

char *
g_file_simple_get_uri (GFile *file)
{
  return g_filename_to_uri (G_FILE_SIMPLE (file)->filename, NULL, NULL);
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

  while ((c = *string) != 0)
    {
      if (g_ascii_iscntrl(c))
	return FALSE;
    }

  return TRUE;
}

char *
g_file_simple_get_parse_name (GFile *file)
{
  const char *filename;
  char *parse_name;
  const gchar *charset;
  char *utf8_filename;
  char *roundtripped_filename;
  gboolean free_utf8_filename;
  gboolean is_valid_utf8;

  filename = G_FILE_SIMPLE (file)->filename;
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

GFile *
g_file_simple_get_parent (GFile *file)
{
  GFileSimple *simple = G_FILE_SIMPLE (file);
  const char *non_root;
  char *dirname;
  GFile *parent;

  /* Check for root */
  non_root = g_path_skip_root (simple->filename);
  if (*non_root == 0)
    return NULL;

  dirname = g_path_get_dirname (simple->filename);
  parent = g_file_simple_new (dirname);
  g_free (dirname);
  return parent;
}

GFile *
g_file_simple_copy (GFile *file)
{
  GFileSimple *simple = G_FILE_SIMPLE (file);

  return g_file_simple_new (simple->filename);
}


GFile *
g_file_simple_get_child (GFile *file,
			 const char *name)
{
  GFileSimple *simple = G_FILE_SIMPLE (file);
  char *filename;
  GFile *child;

  filename = g_build_filename (simple->filename, name, NULL);

  child = g_file_simple_new (filename);
  g_free (filename);
  
  return child;
}

GFileEnumerator *
g_file_simple_enumerate_children (GFile      *file,
				  GFileInfoRequestFlags requested,
				  const char           *attributes)
{
  /* TODO */
  return NULL;
}

static gchar *
read_link (const gchar *full_name)
{
	gchar *buffer;
	guint size;

	size = 256;
	buffer = g_malloc (size);
          
	while (1) {
		int read_size;

                read_size = readlink (full_name, buffer, size);
		if (read_size < 0) {
			g_free (buffer);
			return NULL;
		}
                if (read_size < size) {
			buffer[read_size] = 0;
			return buffer;
		}
                size *= 2;
		buffer = g_realloc (buffer, size);
	}
}


GFileInfo *
g_file_simple_get_info (GFile                *file,
			GFileInfoRequestFlags requested,
			const char           *attributes,
			gboolean              follow_symlinks)
{
  GFileSimple *simple = G_FILE_SIMPLE (file);
  GFileInfo *info;
  struct stat statbuf;

  info = g_file_info_new ();
  
  if (requested && G_FILE_INFO_REQUEST_FLAGS_FROM_STAT_MASK)
    {
      if (follow_symlinks)
	stat (simple->filename, &statbuf);
      else
	lstat (simple->filename, &statbuf);

      g_file_info_set_from_stat (info, requested, &statbuf);
    }

  if (requested && G_FILE_INFO_NAME)
    {
      char *basename = g_path_get_basename (simple->filename);
      g_file_info_set_name (info, basename);
      g_free (basename);
    }

  if (requested && G_FILE_INFO_SYMLINK_TARGET)
    {
      char *link = read_link (simple->filename);
      g_file_info_set_symlink_target (info, link);
      g_free (link);
    }

  if (requested && G_FILE_INFO_ACCESS_RIGHTS)
    {
      /* TODO */
    }
  
  if (requested && G_FILE_INFO_DISPLAY_NAME)
    {
      /* TODO */
    }
  
  if (requested && G_FILE_INFO_EDIT_NAME)
    {
      /* TODO */
    }

  if (requested && G_FILE_INFO_MIME_TYPE)
    {
      /* TODO */
    }
  
  if (requested && G_FILE_INFO_ICON)
    {
      /* TODO */
    }
  
  return info;
}

GFileInputStream *
g_file_simple_read (GFile *file)
{
  return g_local_file_input_stream_new (G_FILE_SIMPLE (file)->filename);
}

GFileOutputStream *
g_file_simple_append_to (GFile *file)
{
  return g_local_file_output_stream_new (G_FILE_SIMPLE (file)->filename,
					 G_OUTPUT_STREAM_OPEN_MODE_APPEND);
}

GFileOutputStream *
g_file_simple_create (GFile *file)
{
  return g_local_file_output_stream_new (G_FILE_SIMPLE (file)->filename,
					 G_OUTPUT_STREAM_OPEN_MODE_CREATE);
}

GFileOutputStream *
g_file_simple_replace (GFile *file,
		       time_t mtime,
		       gboolean  make_backup)
{
  GFileOutputStream *out;

  out = g_local_file_output_stream_new (G_FILE_SIMPLE (file)->filename,
					G_OUTPUT_STREAM_OPEN_MODE_REPLACE);

  g_local_file_output_stream_set_original_mtime (G_LOCAL_FILE_OUTPUT_STREAM (out),
						 mtime);
  g_local_file_output_stream_set_create_backup (G_LOCAL_FILE_OUTPUT_STREAM (out),
						make_backup);
  return out;
}

static void
g_file_simple_file_iface_init (GFileIface *iface)
{
  iface->copy = g_file_simple_copy;
  iface->is_native = g_file_simple_is_native;
  iface->get_path = g_file_simple_get_path;
  iface->get_uri = g_file_simple_get_uri;
  iface->get_parse_name = g_file_simple_get_parse_name;
  iface->get_parent = g_file_simple_get_parent;
  iface->get_child = g_file_simple_get_child;
  iface->enumerate_children = g_file_simple_enumerate_children;
  iface->get_info = g_file_simple_get_info;
  iface->read = g_file_simple_read;
  iface->append_to = g_file_simple_append_to;
  iface->create = g_file_simple_create;
  iface->replace = g_file_simple_replace;
}
