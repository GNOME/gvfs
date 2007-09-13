#include <config.h>
#include <string.h>
#include <sys/types.h>
#include <pwd.h>
#include "gfile.h"
#include "gvfs.h"
#include "gioscheduler.h"
#include <glib/gi18n-lib.h>
#include <glocalfile.h>

static void g_file_base_init (gpointer g_class);
static void g_file_class_init (gpointer g_class,
			       gpointer class_data);


static void g_file_real_read_async (GFile                  *file,
				    int                     io_priority,
				    GFileReadCallback       callback,
				    gpointer                callback_data,
				    GCancellable           *cancellable);

GType
g_file_get_type (void)
{
  static GType file_type = 0;

  if (! file_type)
    {
      static const GTypeInfo file_info =
      {
        sizeof (GFileIface), /* class_size */
	g_file_base_init,   /* base_init */
	NULL,		/* base_finalize */
	g_file_class_init,
	NULL,		/* class_finalize */
	NULL,		/* class_data */
	0,
	0,              /* n_preallocs */
	NULL
      };

      file_type =
	g_type_register_static (G_TYPE_INTERFACE, I_("GFile"),
				&file_info, 0);

      g_type_interface_add_prerequisite (file_type, G_TYPE_OBJECT);
    }

  return file_type;
}

static void
g_file_class_init (gpointer g_class,
		   gpointer class_data)
{
  GFileIface *iface = g_class;

  iface->read_async = g_file_real_read_async;
}

static void
g_file_base_init (gpointer g_class)
{
}

gboolean
g_file_is_native (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->is_native) (file);
}

char *
g_file_get_basename (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_basename) (file);
}


char *
g_file_get_path (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_path) (file);
}

char *
g_file_get_uri (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_uri) (file);
}


char *
g_file_get_parse_name (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_parse_name) (file);
}

GFile *
g_file_dup (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->dup) (file);
}

guint
g_file_hash (gconstpointer file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->hash) ((GFile *)file);
}

gboolean
g_file_equal (GFile *file1,
	      GFile *file2)
{
  GFileIface *iface;
  
  /* If file1 is local, pass file2 to handle the
   * case where one file is a GLocalFile and the other
   * is a wrapper of GLocalFile.
   */
  if (G_IS_LOCAL_FILE (file1))
    {
      iface = G_FILE_GET_IFACE (file2);
      
      return (* iface->equal) (file2, file1);
    }
  else
    {
      iface = G_FILE_GET_IFACE (file1);
      
      return (* iface->equal) (file1, file2);
    }
}


GFile *
g_file_get_parent (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_parent) (file);
}

GFile *
g_file_get_child (GFile *file,
		  const char *name)
{
  return g_file_resolve_relative (file, name);
}

GFile *
g_file_resolve_relative (GFile *file,
			 const char *relative_path)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->resolve_relative) (file, relative_path);
}

GFileEnumerator *
g_file_enumerate_children (GFile *file,
			   const char *attributes,
			   GFileGetInfoFlags flags,
			   GCancellable *cancellable,
			   GError **error)
			   
{
  GFileIface *iface;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->enumerate_children) (file, attributes, flags,
					cancellable, error);
}

GFileInfo *
g_file_get_info (GFile *file,
		 const char *attributes,
		 GFileGetInfoFlags flags,
		 GCancellable *cancellable,
		 GError **error)
{
  GFileIface *iface;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_info) (file, attributes, flags, cancellable, error);
}

GFileInputStream *
g_file_read (GFile *file,
	     GCancellable *cancellable,
	     GError **error)
{
  GFileIface *iface;
  
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }

  iface = G_FILE_GET_IFACE (file);

  return (* iface->read) (file, cancellable, error);
}

GFileOutputStream *
g_file_append_to (GFile *file,
		  GCancellable *cancellable,
		  GError **error)
{
  GFileIface *iface;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->append_to) (file, cancellable, error);
}

GFileOutputStream *
g_file_create (GFile *file,
	       GCancellable *cancellable,
	       GError **error)
{
  GFileIface *iface;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->create) (file, cancellable, error);
}

GFileOutputStream *
g_file_replace (GFile *file,
		time_t mtime,
		gboolean  make_backup,
		GCancellable *cancellable,
		GError **error)
{
  GFileIface *iface;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->replace) (file, mtime, make_backup, cancellable, error);
}

void
g_file_read_async (GFile                  *file,
		   int                     io_priority,
		   GFileReadCallback       callback,
		   gpointer                callback_data,
		   GCancellable           *cancellable)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  (* iface->read_async) (file,
			 io_priority,
			 callback,
			 callback_data,
			 cancellable);
}

static gboolean
file_copy_fallback (GFile                  *source,
		    GFile                  *destination,
		    GFileCopyFlags          flags,
		    GCancellable           *cancellable,
		    GFileProgressCallback   progress_callback,
		    gpointer                progress_callback_data,
		    GError                **error)
{
  GInputStream *in;
  GOutputStream *out;
  char buffer[8192], *p;
  gssize n_read, n_written;
  goffset total_size, current_size;
  GFileInfo *info;

  in = (GInputStream *)g_file_read (source, cancellable, error);
  if (in == NULL)
    return FALSE;

  total_size = 0;
  current_size = 0;

  info = g_file_input_stream_get_file_info (G_FILE_INPUT_STREAM (in),
					    G_FILE_ATTRIBUTE_STD_SIZE,
					    cancellable, NULL);
  if (info)
    {
      total_size = g_file_info_get_size (info);
      g_object_unref (info);
    }

  
  if (flags & G_FILE_COPY_OVERWRITE)
    out = (GOutputStream *)g_file_replace (destination, 0, flags & G_FILE_COPY_BACKUP, cancellable, error);
  else
    out = (GOutputStream *)g_file_create (destination, cancellable, error);

  if (out == NULL)
    {
      g_object_unref (in);
      return FALSE;
    }

  do
    {
      n_read = g_input_stream_read (in, buffer, sizeof (buffer), cancellable, error);
      if (n_read == -1)
	goto error;
      if (n_read == 0)
	break;

      current_size += n_read;

      p = buffer;
      while (n_read > 0)
	{
	  n_written = g_output_stream_write (out, p, n_read, cancellable, error);
	  if (n_written == -1)
	    goto error;

	  p += n_written;
	  n_read -= n_read;
	}


      if (progress_callback)
	progress_callback (current_size, total_size, progress_callback_data);
    }
  while (TRUE);

  /* Don't care about errors in source here */
  g_input_stream_close (in, cancellable, NULL);

  /* But write errors on close are bad! */
  if (!g_output_stream_close (out, cancellable, error))
    goto error;

  g_object_unref (in);
  g_object_unref (out);
  
  return TRUE;

 error:
  g_object_unref (in);
  g_object_unref (out);
  return FALSE;
}

gboolean
g_file_copy (GFile                  *source,
	     GFile                  *destination,
	     GFileCopyFlags          flags,
	     GCancellable           *cancellable,
	     GFileProgressCallback   progress_callback,
	     gpointer                progress_callback_data,
	     GError                **error)
{
  GFileIface *iface;
  GError *my_error;
  gboolean res;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return FALSE;
    }
  
  if (G_OBJECT_TYPE (source) == G_OBJECT_TYPE (destination))
    {
      iface = G_FILE_GET_IFACE (source);

      if (iface->copy)
	{
	  my_error = NULL;
	  res = (* iface->copy) (source, destination, flags, cancellable, progress_callback, progress_callback_data, &my_error);
	  
	  if (res)
	    return TRUE;
	  
	  if (my_error->domain != G_VFS_ERROR || my_error->code != G_VFS_ERROR_NOT_SUPPORTED)
	    {
	      g_propagate_error (error, my_error);
	      return FALSE;
	    }
	}
    }

  return file_copy_fallback (source, destination, flags, cancellable,
			     progress_callback, progress_callback_data,
			     error);
}

gboolean
g_file_move (GFile                  *source,
	     GFile                  *destination,
	     GFileCopyFlags          flags,
	     GCancellable           *cancellable,
	     GFileProgressCallback   progress_callback,
	     gpointer                progress_callback_data,
	     GError                **error)
{
  GFileIface *iface;
  GError *my_error;
  gboolean res;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return FALSE;
    }
  
  if (G_OBJECT_TYPE (source) == G_OBJECT_TYPE (destination))
    {
      iface = G_FILE_GET_IFACE (source);

      if (iface->move)
	{
	  my_error = NULL;
	  res = (* iface->move) (source, destination, flags, cancellable, progress_callback, progress_callback_data, &my_error);
	  
	  if (res)
	    return TRUE;
	  
	  if (my_error->domain != G_VFS_ERROR || my_error->code != G_VFS_ERROR_NOT_SUPPORTED)
	    {
	      g_propagate_error (error, my_error);
	      return FALSE;
	    }
	}
    }

  if (!g_file_copy (source, destination, flags, cancellable,
		    progress_callback, progress_callback_data,
		    error))
    return FALSE;

  return g_file_delete (source, cancellable, error);
}

gboolean
g_file_make_directory (GFile *file,
		       GCancellable *cancellable,
		       GError **error)
{
  GFileIface *iface;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return FALSE;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->make_directory) (file, cancellable, error);
}

gboolean
g_file_delete (GFile *file,
	       GCancellable *cancellable,
	       GError **error)
{
  GFileIface *iface;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return FALSE;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->delete_file) (file, cancellable, error);
}

void
g_file_mount (GFile *file,
	      GMountOperation *mount_operation)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  (* iface->mount) (file, mount_operation);
}

/********************************************
 *   Default implementation of async ops    *
 ********************************************/

typedef struct {
  GFile *file;
  GError *error;
  gpointer callback_data;
} AsyncOp;

static void
async_op_free (gpointer data)
{
  AsyncOp *op = data;

  if (op->error)
    g_error_free (op->error);

  g_object_unref (op->file);
  
  g_free (op);
}

typedef struct {
  AsyncOp      op;
  GFileInputStream *res_stream;
  GFileReadCallback callback;
} ReadAsyncOp;

static void
read_op_report (gpointer data)
{
  ReadAsyncOp *op = data;

  op->callback (op->op.file,
		op->res_stream,
		op->op.callback_data,
		op->op.error);
}

static void
read_op_func (GIOJob *job,
	      GCancellable *c,
	      gpointer data)
{
  ReadAsyncOp *op = data;
  GFileIface *iface;

  if (g_cancellable_is_cancelled (c))
    {
      op->res_stream = NULL;
      g_set_error (&op->op.error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
    }
  else
    {
      iface = G_FILE_GET_IFACE (op->op.file);
      op->res_stream = iface->read (op->op.file, c, &op->op.error);
    }

  g_io_job_send_to_mainloop (job, read_op_report,
			     op, async_op_free,
			     FALSE);
}

static void
g_file_real_read_async (GFile                  *file,
			int                     io_priority,
			GFileReadCallback       callback,
			gpointer                callback_data,
			GCancellable           *cancellable)
{
  ReadAsyncOp *op;

  op = g_new0 (ReadAsyncOp, 1);

  op->op.file = g_object_ref (file);
  op->callback = callback;
  op->op.callback_data = callback_data;
  
  g_schedule_io_job (read_op_func, op, NULL, io_priority,
		     cancellable);
}

/********************************************
 *   Default VFS operations                 *
 ********************************************/

GFile *
g_file_get_for_path (const char *path)
{
  return g_vfs_get_file_for_path (g_vfs_get (),
				  path);
}
  
GFile *
g_file_get_for_uri (const char *uri)
{
  return g_vfs_get_file_for_uri (g_vfs_get (),
				 uri);
}
  
GFile *
g_file_parse_name (const char *parse_name)
{
  return g_vfs_parse_name (g_vfs_get (),
			   parse_name);
}

static gboolean
is_valid_scheme_character (char c)
{
  return g_ascii_isalnum (c) || c == '+' || c == '-' || c == '.';
}

static gboolean
has_valid_scheme (const char *uri)
{
  const char *p;
  
  p = uri;
  
  if (!is_valid_scheme_character (*p))
    return FALSE;

  do {
    p++;
  } while (is_valid_scheme_character (*p));

  return *p == ':';
}

GFile *
g_file_get_for_commandline_arg (const char *arg)
{
  GFile *file;
  char *filename;
  char *current_dir;
  
  g_return_val_if_fail (arg != NULL, NULL);
  
  if (g_path_is_absolute (arg))
    return g_file_get_for_path (arg);

  if (has_valid_scheme (arg))
    return g_file_get_for_uri (arg);
    
  current_dir = g_get_current_dir ();
  filename = g_build_filename (current_dir, arg, NULL);
  g_free (current_dir);
  
  file = g_file_get_for_path (filename);
  g_free (filename);
  
  return file;
}
