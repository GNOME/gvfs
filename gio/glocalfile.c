#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "glocalfile.h"
#include "glocalfileinfo.h"
#include "glocalfileenumerator.h"
#include "glocalfileinputstream.h"
#include "glocalfileoutputstream.h"
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>

static void g_local_file_file_iface_init (GFileIface       *iface);

struct _GLocalFile
{
  GObject parent_instance;

  char *filename;
};

G_DEFINE_TYPE_WITH_CODE (GLocalFile, g_local_file, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE,
						g_local_file_file_iface_init))

static void
g_local_file_finalize (GObject *object)
{
  GLocalFile *local;

  local = G_LOCAL_FILE (object);

  g_free (local->filename);
  
  if (G_OBJECT_CLASS (g_local_file_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_local_file_parent_class)->finalize) (object);
}

static void
g_local_file_class_init (GLocalFileClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_local_file_finalize;
}

static void
g_local_file_init (GLocalFile *local)
{
}


static char *
canonicalize_filename (const char *filename)
{
  char *canon, *start, *p, *q;
  char *cwd;
  
  if (!g_path_is_absolute (filename))
    {
      cwd = g_get_current_dir ();
      canon = g_build_filename (cwd, filename, NULL);
      g_free (cwd);
    }
  else
    canon = g_strdup (filename);

  start = (char *)g_path_skip_root (canon);

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

GFile *
g_local_file_new (const char *filename)
{
  GLocalFile *local;

  local = g_object_new (G_TYPE_LOCAL_FILE, NULL);
  local->filename = canonicalize_filename (filename);
  
  return G_FILE (local);
}

static gboolean
g_local_file_is_native (GFile *file)
{
  return TRUE;
}

static char *
g_local_file_get_basename (GFile *file)
{
  return g_path_get_basename (G_LOCAL_FILE (file)->filename);
}

static char *
g_local_file_get_path (GFile *file)
{
  return g_strdup (G_LOCAL_FILE (file)->filename);
}

static char *
g_local_file_get_uri (GFile *file)
{
  return g_filename_to_uri (G_LOCAL_FILE (file)->filename, NULL, NULL);
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
g_local_file_get_parse_name (GFile *file)
{
  const char *filename;
  char *parse_name;
  const gchar *charset;
  char *utf8_filename;
  char *roundtripped_filename;
  gboolean free_utf8_filename;
  gboolean is_valid_utf8;

  filename = G_LOCAL_FILE (file)->filename;
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
g_local_file_get_parent (GFile *file)
{
  GLocalFile *local = G_LOCAL_FILE (file);
  const char *non_root;
  char *dirname;
  GFile *parent;

  /* Check for root */
  non_root = g_path_skip_root (local->filename);
  if (*non_root == 0)
    return NULL;

  dirname = g_path_get_dirname (local->filename);
  parent = g_local_file_new (dirname);
  g_free (dirname);
  return parent;
}

static GFile *
g_local_file_dup (GFile *file)
{
  GLocalFile *local = G_LOCAL_FILE (file);

  return g_local_file_new (local->filename);
}

static guint
g_local_file_hash (GFile *file)
{
  GLocalFile *local = G_LOCAL_FILE (file);
  
  return g_str_hash (local->filename);
}

static gboolean
g_local_file_equal (GFile *file1,
		    GFile *file2)
{
  GLocalFile *local1 = G_LOCAL_FILE (file1);
  GLocalFile *local2;

  if (!G_IS_LOCAL_FILE (file2))
    return FALSE;
  
  local2 = G_LOCAL_FILE (file2);

  return g_str_equal (local1->filename, local2->filename);
}

static GFile *
g_local_file_resolve_relative (GFile *file,
			       const char *relative_path)
{
  GLocalFile *local = G_LOCAL_FILE (file);
  char *filename;
  GFile *child;

  if (g_path_is_absolute (relative_path))
    return g_local_file_new (relative_path);
  
  filename = g_build_filename (local->filename, relative_path, NULL);
  child = g_local_file_new (filename);
  g_free (filename);
  
  return child;
}

static GFileEnumerator *
g_local_file_enumerate_children (GFile      *file,
				 const char *attributes,
				 GFileGetInfoFlags flags,
				 GCancellable *cancellable,
				 GError **error)
{
  GLocalFile *local = G_LOCAL_FILE (file);
  return g_local_file_enumerator_new (local->filename,
				      attributes, flags,
				      cancellable, error);
}

static GFileInfo *
g_local_file_get_info (GFile                *file,
		       const char           *attributes,
		       GFileGetInfoFlags     flags,
		       GCancellable         *cancellable,
		       GError              **error)
{
  GLocalFile *local = G_LOCAL_FILE (file);
  GFileInfo *info;
  GFileAttributeMatcher *matcher;
  char *basename;

  matcher = g_file_attribute_matcher_new (attributes);
  
  basename = g_path_get_basename (local->filename);
  
  info = g_local_file_info_get (basename, local->filename,
				matcher, flags,
				error);
  
  g_free (basename);

  g_file_attribute_matcher_free (matcher);

  return info;
}

static GFileInputStream *
g_local_file_read (GFile *file,
		   GCancellable *cancellable,
		   GError **error)
{
  GLocalFile *local = G_LOCAL_FILE (file);
  int fd;
  
  fd = g_open (local->filename, O_RDONLY, 0);
  if (fd == -1)
    {
      g_set_error (error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error opening file %s: %s"),
		   local->filename, g_strerror (errno));
      return NULL;
    }
  
  return g_local_file_input_stream_new (fd);
}

static GFileOutputStream *
g_local_file_append_to (GFile *file,
			GCancellable *cancellable,
			GError **error)
{
  return g_local_file_output_stream_append (G_LOCAL_FILE (file)->filename,
					    cancellable, error);
}

static GFileOutputStream *
g_local_file_create (GFile *file,
		     GCancellable *cancellable,
		     GError **error)
{
  return g_local_file_output_stream_create (G_LOCAL_FILE (file)->filename,
					    cancellable, error);
}

static GFileOutputStream *
g_local_file_replace (GFile *file,
		      time_t mtime,
		      gboolean make_backup,
		      GCancellable *cancellable,
		      GError **error)
{
  return g_local_file_output_stream_replace (G_LOCAL_FILE (file)->filename,
					     mtime, make_backup,
					     cancellable, error);
}


static gboolean
g_local_file_delete (GFile *file,
		     GCancellable *cancellable,
		     GError **error)
{
  GLocalFile *local = G_LOCAL_FILE (file);
  int res;
  
  res = g_unlink (local->filename);

  /* Linux returns EISDIR in the case of unlinking a directory.
     Posix specifies that the unlink of the directorey may succeed,
     otherwise it should return EPERM */
  if (res == -1 &&
      (errno == EISDIR || errno == EPERM))
    res = g_rmdir (local->filename);

  if (res == -1)
    {
      g_set_error (error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error removing file %s: %s"),
		   local->filename, g_strerror (errno));
      return FALSE;
    }
  
  return TRUE;
}

static gboolean
g_local_file_copy (GFile                *source,
		   GFile                *destination,
		   GFileCopyFlags        flags,
		   GCancellable         *cancellable,
		   GFileProgressCallback progress_callback,
		   gpointer              progress_callback_data,
		   GError              **error)
{
  /* Fall back to default copy */
  g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_NOT_SUPPORTED, "Copy not supported");
  return FALSE;
}

static gboolean
g_local_file_move (GFile                *source,
		   GFile                *destination,
		   GFileCopyFlags        flags,
		   GCancellable         *cancellable,
		   GFileProgressCallback progress_callback,
		   gpointer              progress_callback_data,
		   GError              **error)
{
  GLocalFile *local_source = G_LOCAL_FILE (source);
  GLocalFile *local_destination = G_LOCAL_FILE (destination);
  struct stat statbuf;
  gboolean destination_exist;
  char *backup_name; 

  destination_exist = FALSE;
  if ((flags & G_FILE_COPY_OVERWRITE) == 0 ||
      (flags & G_FILE_COPY_BACKUP))
    {
      if (!(g_stat (local_destination->filename, &statbuf) == -1 &&
	    errno == ENOENT))
	destination_exist = TRUE;
    }

  if ((flags & G_FILE_COPY_OVERWRITE) == 0 && destination_exist)
    {
      g_set_error (error,
		   G_FILE_ERROR,
		   G_FILE_ERROR_EXIST,
		   _("Target file already exists"));
      return FALSE;
    }
  
  if (flags & G_FILE_COPY_BACKUP && destination_exist)
    {
      backup_name = g_strconcat (local_destination->filename, "~", NULL);
      if (rename (local_destination->filename, backup_name) == -1)
	{
      	  g_set_error (error,
		       G_VFS_ERROR,
		       G_VFS_ERROR_CANT_CREATE_BACKUP,
		       _("Backup file creation failed"));
	  g_free (backup_name);
	  return FALSE;
	}
      g_free (backup_name);
    }

  if (rename (local_source->filename, local_destination->filename) == -1)
    {
      if (errno == EXDEV)
	goto fallback;
      else
	g_set_error (error, G_FILE_ERROR,
		     g_file_error_from_errno (errno),
		     _("Error moving file: %s"),
		     g_strerror (errno));
      return FALSE;

    }
  return TRUE;

 fallback:

  if (!g_file_copy (source, destination, G_FILE_COPY_OVERWRITE, cancellable,
		    progress_callback, progress_callback_data,
		    error))
    return FALSE;

  /* Try to inherit source permissions, etc */
  if (g_stat (local_source->filename, &statbuf) != -1)
    {
      chown (local_destination->filename, statbuf.st_uid, statbuf.st_gid);
      chmod (local_destination->filename, statbuf.st_mode);
    }

  /* TODO: Inherit xattrs */
  
  return g_file_delete (source, cancellable, error);
}


static void
g_local_file_mount (GFile *file,
		    GMountOperation *mount_operation)
{
  /* Always just ok... */
  g_signal_emit_by_name (mount_operation, "done", TRUE, NULL);
}

static void
g_local_file_file_iface_init (GFileIface *iface)
{
  iface->dup = g_local_file_dup;
  iface->hash = g_local_file_hash;
  iface->equal = g_local_file_equal;
  iface->is_native = g_local_file_is_native;
  iface->get_basename = g_local_file_get_basename;
  iface->get_path = g_local_file_get_path;
  iface->get_uri = g_local_file_get_uri;
  iface->get_parse_name = g_local_file_get_parse_name;
  iface->get_parent = g_local_file_get_parent;
  iface->resolve_relative = g_local_file_resolve_relative;
  iface->enumerate_children = g_local_file_enumerate_children;
  iface->get_info = g_local_file_get_info;
  iface->read = g_local_file_read;
  iface->append_to = g_local_file_append_to;
  iface->create = g_local_file_create;
  iface->replace = g_local_file_replace;
  iface->delete_file = g_local_file_delete;
  iface->copy = g_local_file_copy;
  iface->move = g_local_file_move;
  iface->mount = g_local_file_mount;
}
