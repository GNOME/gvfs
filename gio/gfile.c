#include <config.h>
#include <string.h>
#include <sys/types.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#include "gfile.h"
#include "gvfs.h"
#include "gioscheduler.h"
#include <glib/gi18n-lib.h>
#include <glocalfile.h>
#include "gsimpleasyncresult.h"

static void g_file_base_init (gpointer g_class);
static void g_file_class_init (gpointer g_class,
			       gpointer class_data);

static void              g_file_real_read_async  (GFile                *file,
						  int                   io_priority,
						  GCancellable         *cancellable,
						  GAsyncReadyCallback   callback,
						  gpointer              user_data);
static GFileInputStream *g_file_real_read_finish (GFile                *file,
						  GAsyncResult         *res,
						  GError              **error);

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
  iface->read_finish = g_file_real_read_finish;
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
  
  if (G_TYPE_FROM_INSTANCE (file1) != G_TYPE_FROM_INSTANCE (file2))
    return FALSE;

  iface = G_FILE_GET_IFACE (file1);
  
  return (* iface->equal) (file1, file2);
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
g_file_get_child_for_display_name (GFile *file,
				   const char *display_name,
				   GError **error)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_child_for_display_name) (file, display_name, error);
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
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
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
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_info) (file, attributes, flags, cancellable, error);
}

GFileInfo *
g_file_get_filesystem_info (GFile *file,
			    const char *attributes,
			    GCancellable *cancellable,
			    GError **error)
{
  GFileIface *iface;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_filesystem_info) (file, attributes, cancellable, error);
}

/* Fails on directories */
GFileInputStream *
g_file_read (GFile *file,
	     GCancellable *cancellable,
	     GError **error)
{
  GFileIface *iface;
  
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
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
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
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
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
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
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->replace) (file, mtime, make_backup, cancellable, error);
}

void
g_file_read_async (GFile                  *file,
		   int                     io_priority,
		   GCancellable           *cancellable,
		   GAsyncReadyCallback     callback,
		   gpointer                user_data)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);
  (* iface->read_async) (file,
			 io_priority,
			 cancellable,
			 callback,
			 user_data);
}

GFileInputStream *
g_file_read_finish (GFile                  *file,
		    GAsyncResult           *res,
		    GError                **error)
{
  GFileIface *iface;

  if (G_IS_SIMPLE_ASYNC_RESULT (res))
    {
      GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
      if (g_simple_async_result_propagate_error (simple, error))
	return NULL;
    }
  
  iface = G_FILE_GET_IFACE (file);
  return (* iface->read_finish) (file, res, error);
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
  GError *my_error;
  GFileType file_type;

  my_error = NULL;
  in = (GInputStream *)g_file_read (source, cancellable, &my_error);
  if (in == NULL) {
    if (my_error->domain == G_IO_ERROR && my_error->code == G_IO_ERROR_IS_DIRECTORY)
      {
	g_error_free (my_error);
	my_error = NULL;
	
	/* The source is a directory, don't fail with WOULD_RECURSE immediately, as
	   that is less useful to the app. Better check for errors on the target instead. */
	
	info = g_file_get_info (destination, G_FILE_ATTRIBUTE_STD_TYPE,
				G_FILE_GET_INFO_NOFOLLOW_SYMLINKS,
				cancellable, &my_error);
	if (info != NULL) {
	  file_type = g_file_info_get_file_type (info);
	  g_object_unref (info);

	  if (flags & G_FILE_COPY_OVERWRITE)
	    {
	      if (file_type == G_FILE_TYPE_DIRECTORY)
		{
		  g_set_error (error, G_IO_ERROR, G_IO_ERROR_IS_DIRECTORY,
			     _("Can't copy over directory"));
		  return FALSE;
		}
	    }
	  else
	    {
	      g_set_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
			   _("Target file exists"));
	      return FALSE;
	    }
	} else {
	  /* Error getting info from target, return that error (except for NOT_FOUND, which is no error here) */
	  if (my_error->domain != G_IO_ERROR && my_error->code != G_IO_ERROR_NOT_FOUND)
	    {
	      g_propagate_error (error, my_error);
	      return FALSE;
	    }
	  g_error_free (my_error);
	}
	g_set_error (error, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE,
		     _("Can't recursively copy directory"));
	return FALSE;
      }
    else
      g_propagate_error (error, my_error);
    return FALSE;
  }

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

/* Errors:

   source    dest    flags   res
    -        *       *       G_IO_ERROR_NOT_FOUND
    file     -       *       ok
    file     *       0       G_IO_ERROR_EXISTS
    file     file    overwr  ok
    file     dir     overwr  G_IO_ERROR_IS_DIRECTORY
    
    dir      -       *       G_IO_ERROR_WOULD_RECURSE
    dir      *       0       G_IO_ERROR_EXISTS
    dir      dir     overwr  G_IO_ERROR_IS_DIRECTORY
    dir      file    overwr  G_IO_ERROR_WOULD_RECURSE
 */
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
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
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
	  
	  if (my_error->domain != G_IO_ERROR || my_error->code != G_IO_ERROR_NOT_SUPPORTED)
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

/* Errors:

   source    dest    flags   res
    -        *       *       G_IO_ERROR_NOT_FOUND
    file     -       *       ok
    file     *       0       G_IO_ERROR_EXISTS
    file     file    overwr  ok
    file     dir     overwr  G_IO_ERROR_IS_DIRECTORY
    
    dir      -       *       ok || G_IO_ERROR_WOULD_RECURSE
    dir      *       0       G_IO_ERROR_EXISTS
    dir      dir     overwr  G_IO_ERROR_IS_DIRECTORY
    dir      file    overwr  ok || G_IO_ERROR_WOULD_RECURSE
 */
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
  GFileAttributeInfoList *list;
  GFileInfo *info;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
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
	  
	  if (my_error->domain != G_IO_ERROR || my_error->code != G_IO_ERROR_NOT_SUPPORTED)
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

  info = g_file_get_info (source, "*", 0, cancellable, NULL);

  if (info != NULL)
    {
      const GFileAttributeValue *value;
      int i;
      
      list = g_file_query_settable_attributes (destination, cancellable, NULL);
      if (list)
	{
	  for (i = 0; i < list->n_infos; i++)
	    {
	      const char *name = list->infos[i].name;

	      /* Symlink target is already copied in copy */
	      if (strcmp (name, G_FILE_ATTRIBUTE_STD_SYMLINK_TARGET) == 0)
		continue;

	      /* TODO: What about order, e,g. must change uid + gid before mode.. */
	      
	      value = g_file_info_get_attribute (info, 
						 name);

	      /* Ignore errors */
	      g_file_set_attribute (destination,
				    name, value,
				    0, cancellable,
				    NULL);
	      
	    }
	  g_file_attribute_info_list_free (list);      
	}
      
      list = g_file_query_writable_namespaces (destination, cancellable, NULL);  
      if (list)
	{
	  /* TODO: Try to list and copy writable namespaces */
	  
	  g_file_attribute_info_list_free (list);      
	}
      
      g_object_unref (info);
    }

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
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return FALSE;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->make_directory) (file, cancellable, error);
}

gboolean
g_file_make_symbolic_link (GFile *file,
			   const char *symlink_value,
			   GCancellable *cancellable,
			   GError **error)
{
  GFileIface *iface;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return FALSE;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->make_symbolic_link) (file, symlink_value, cancellable, error);
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
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return FALSE;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->delete_file) (file, cancellable, error);
}

gboolean
g_file_trash (GFile *file,
	      GCancellable *cancellable,
	      GError **error)
{
  GFileIface *iface;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return FALSE;
    }
  
  iface = G_FILE_GET_IFACE (file);

  if (iface->trash == NULL)
    {
      g_set_error (error,
		   G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		   _("Trash not supported"));
      return FALSE;
    }
  
  return (* iface->trash) (file, cancellable, error);
}

GFile *
g_file_set_display_name (GFile                  *file,
			 const char             *display_name,
			 GCancellable           *cancellable,
			 GError                **error)
{
  GFileIface *iface;

  if (strchr (display_name, '/') != NULL)
    {
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_INVALID_ARGUMENT,
		   _("File names cannot contain '/'"));
      return FALSE;
    }
  
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return FALSE;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->set_display_name) (file, display_name, cancellable, error);
}
/*
  Returns the type and full attribute name of all the attributes that
  the file can set. This doesn't mean setting it will always succeed though,
  you might get an access failure, or some specific file may not support a
  specific attribute
*/
GFileAttributeInfoList *
g_file_query_settable_attributes (GFile                      *file,
				  GCancellable               *cancellable,
				  GError                    **error)
{
  GFileIface *iface;
  GError *my_error;
  GFileAttributeInfoList *list;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return FALSE;
    }
  
  iface = G_FILE_GET_IFACE (file);

  if (iface->query_settable_attributes == NULL)
    return g_file_attribute_info_list_new ();

  my_error = NULL;
  list = (* iface->query_settable_attributes) (file, cancellable, &my_error);
  
  if (list == NULL)
    {
      if (my_error->domain == G_IO_ERROR && my_error->code == G_IO_ERROR_NOT_SUPPORTED)
	{
	  list = g_file_attribute_info_list_new ();
	  g_error_free (my_error);
	}
      else
	g_propagate_error (error, my_error);
    }
  
  return list;
}

/* Returns the list of attribute namespaces where the user can create
   their own attribute names, such as extended attributes */
GFileAttributeInfoList *
g_file_query_writable_namespaces (GFile                      *file,
				  GCancellable               *cancellable,
				  GError                    **error)
{
  GFileIface *iface;
  GError *my_error;
  GFileAttributeInfoList *list;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return FALSE;
    }
  
  iface = G_FILE_GET_IFACE (file);

  if (iface->query_writable_namespaces == NULL)
    return g_file_attribute_info_list_new ();

  my_error = NULL;
  list = (* iface->query_writable_namespaces) (file, cancellable, &my_error);
  
  if (list == NULL)
    {
      if (my_error->domain == G_IO_ERROR && my_error->code == G_IO_ERROR_NOT_SUPPORTED)
	{
	  list = g_file_attribute_info_list_new ();
	  g_error_free (my_error);
	}
      else
	g_propagate_error (error, my_error);
    }
  
  return list;
}

gboolean
g_file_set_attribute (GFile                  *file,
		      const char             *attribute,
		      const GFileAttributeValue    *value,
		      GFileGetInfoFlags       flags,
		      GCancellable           *cancellable,
		      GError                **error)
{
  GFileIface *iface;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_IO_ERROR,
		   G_IO_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return FALSE;
    }
  
  iface = G_FILE_GET_IFACE (file);

  return (* iface->set_attribute) (file, attribute, value, flags, cancellable, error);
}

gboolean
g_file_set_attribute_string (GFile                  *file,
			     const char             *attribute,
			     const char             *value,
			     GFileGetInfoFlags       flags,
			     GCancellable           *cancellable,
			     GError                **error)
{
  GFileAttributeValue v;
  v.type = G_FILE_ATTRIBUTE_TYPE_STRING;
  v.u.string = (char *)value;
  return g_file_set_attribute (file, attribute, &v, flags, cancellable, error);
}

gboolean
g_file_set_attribute_byte_string  (GFile                  *file,
				   const char             *attribute,
				   const char             *value,
				   GFileGetInfoFlags       flags,
				   GCancellable           *cancellable,
				   GError                **error)
{
  GFileAttributeValue v;
  v.type = G_FILE_ATTRIBUTE_TYPE_BYTE_STRING;
  v.u.string = (char *)value;
  return g_file_set_attribute (file, attribute, &v, flags, cancellable, error);
}

gboolean
g_file_set_attribute_uint32 (GFile                  *file,
			     const char             *attribute,
			     guint32                 value,
			     GFileGetInfoFlags       flags,
			     GCancellable           *cancellable,
			     GError                **error)
{
  GFileAttributeValue v;
  v.type = G_FILE_ATTRIBUTE_TYPE_UINT32;
  v.u.uint32 = value;
  return g_file_set_attribute (file, attribute, &v, flags, cancellable, error);
}

gboolean
g_file_set_attribute_int32 (GFile                  *file,
			    const char             *attribute,
			    gint32                  value,
			    GFileGetInfoFlags       flags,
			    GCancellable           *cancellable,
			    GError                **error)
{
  GFileAttributeValue v;
  v.type = G_FILE_ATTRIBUTE_TYPE_INT32;
  v.u.int32 = value;
  return g_file_set_attribute (file, attribute, &v, flags, cancellable, error);
}

gboolean
g_file_set_attribute_uint64 (GFile                  *file,
			     const char             *attribute,
			     guint64                 value,
			     GFileGetInfoFlags       flags,
			     GCancellable           *cancellable,
			     GError                **error)
 {
  GFileAttributeValue v;
  v.type = G_FILE_ATTRIBUTE_TYPE_UINT64;
  v.u.uint64 = value;
  return g_file_set_attribute (file, attribute, &v, flags, cancellable, error);
}

gboolean
g_file_set_attribute_int64 (GFile                  *file,
			    const char             *attribute,
			    gint64                  value,
			    GFileGetInfoFlags       flags,
			    GCancellable           *cancellable,
			    GError                **error)
{
 GFileAttributeValue v;
  v.type = G_FILE_ATTRIBUTE_TYPE_INT64;
  v.u.int64 = value;
  return g_file_set_attribute (file, attribute, &v, flags, cancellable, error);
}

void
g_file_mount_mountable (GFile                  *file,
			GMountOperation        *mount_operation,
			GCancellable           *cancellable,
			GAsyncReadyCallback     callback,
			gpointer                user_data)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);
  (* iface->mount_mountable) (file,
			      mount_operation,
			      cancellable,
			      callback,
			      user_data);
  
}

GFile *
g_file_mount_mountable_finish (GFile                  *file,
			       GAsyncResult           *result,
			       GError                **error)
{
  GFileIface *iface;

  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return NULL;
    }
  
  iface = G_FILE_GET_IFACE (file);
  return (* iface->mount_mountable_finish) (file, result, error);
}

void
g_file_unmount_mountable (GFile                  *file,
			  GCancellable           *cancellable,
			  GAsyncReadyCallback     callback,
			  gpointer                user_data)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);
  (* iface->unmount_mountable) (file,
				cancellable,
				callback,
				user_data);
}

gboolean
g_file_unmount_mountable_finish   (GFile                  *file,
				   GAsyncResult           *result,
				   GError                **error)
{
  GFileIface *iface;

  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return FALSE;
    }
  
  iface = G_FILE_GET_IFACE (file);
  return (* iface->unmount_mountable_finish) (file, result, error);
}

void
g_file_eject_mountable (GFile                  *file,
			GCancellable           *cancellable,
			GAsyncReadyCallback     callback,
			gpointer                user_data)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);
  (* iface->eject_mountable) (file,
			      cancellable,
			      callback,
			      user_data);
}

gboolean
g_file_eject_mountable_finish (GFile                  *file,
			       GAsyncResult           *result,
			       GError                **error)
{
  GFileIface *iface;

  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return FALSE;
    }
  
  iface = G_FILE_GET_IFACE (file);
  return (* iface->eject_mountable_finish) (file, result, error);
}

GDirectoryMonitor*
g_file_monitor_directory (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->monitor_dir) (file);
}

GFileMonitor*
g_file_monitor_file (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->monitor_file) (file);
}


/********************************************
 *   Default implementation of async ops    *
 ********************************************/

static void
open_read_async_thread (GSimpleAsyncResult *res,
			GObject *object,
			GCancellable *cancellable)
{
  GFileIface *iface;
  GFileInputStream *stream;
  GError *error = NULL;

  iface = G_FILE_GET_IFACE (object);

  stream = iface->read (G_FILE (object), cancellable, &error);

  if (stream == NULL)
    {
      g_simple_async_result_set_from_error (res, error);
      g_error_free (error);
    }
  else
    g_simple_async_result_set_op_res_gpointer (res, stream, g_object_unref);
}

static void
g_file_real_read_async (GFile                  *file,
			int                     io_priority,
			GCancellable           *cancellable,
			GAsyncReadyCallback     callback,
			gpointer                user_data)
{
  GSimpleAsyncResult *res;
  
  res = g_simple_async_result_new (G_OBJECT (file), callback, user_data, g_file_real_read_async);
  
  g_simple_async_result_run_in_thread (res, open_read_async_thread, io_priority, cancellable);
  g_object_unref (res);
}

static GFileInputStream *
g_file_real_read_finish (GFile                  *file,
			 GAsyncResult           *res,
			 GError                **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  gpointer op;

  g_assert (g_simple_async_result_get_source_tag (simple) == g_file_real_read_async);

  op = g_simple_async_result_get_op_res_gpointer (simple);
  if (op)
    return g_object_ref (op);
  
  return NULL;
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

void
g_mount_for_location (GFile                  *location,
		      GMountOperation        *mount_operation,
		      GCancellable           *cancellable,
		      GAsyncReadyCallback     callback,
		      gpointer                user_data)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (location);

  if (iface->mount_for_location == NULL)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (location),
					   callback, user_data,
					   G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
					   _("volume doesn't implement mount"));
      
      return;
    }
  
  return (* iface->mount_for_location) (location, mount_operation, cancellable, callback, user_data);

}

gboolean
g_mount_for_location_finish (GFile                  *location,
			     GAsyncResult           *result,
			     GError                **error)
{
  GFileIface *iface;

  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return FALSE;
    }
  
  iface = G_FILE_GET_IFACE (location);

  return (* iface->mount_for_location_finish) (location, result, error);
}

/********************************************
 *   Utility functions                      *
 ********************************************/

#define GET_CONTENT_BLOCK_SIZE 8192

gboolean
g_file_load_contents (GFile                *file,
		      GCancellable         *cancellable,
		      char                **contents,
		      gsize                *length,
		      GError              **error)
{
  GFileInputStream *in;
  GByteArray *content;
  gsize pos;
  gssize res;

  in = g_file_read (file,
		    cancellable,
		    error);
  if (in == NULL)
    return FALSE;

  content = g_byte_array_new ();
  pos = 0;
  
  g_byte_array_set_size (content, pos + GET_CONTENT_BLOCK_SIZE + 1);
  while ((res = g_input_stream_read (G_INPUT_STREAM (in),
				     content->data + pos,
				     GET_CONTENT_BLOCK_SIZE,
				     cancellable, error)) > 0)
    {
      pos += res;
      g_byte_array_set_size (content, pos + GET_CONTENT_BLOCK_SIZE + 1);
    }

  /* Ignore errors on close */
  g_input_stream_close (G_INPUT_STREAM (in), cancellable, NULL);
  
  if (res < 0)
    {
      /* error is set already */
      g_byte_array_free (content, TRUE);
      return FALSE;
    }

  if (length)
    *length = pos;

  /* Zero terminate (we got an extra byte allocated for this */
  content->data[pos] = 0;
  
  *contents = (char *)g_byte_array_free (content, FALSE);
  
  return TRUE;
}

typedef struct {
  GFile *file;
  GError *error;
  GCancellable *cancellable;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GByteArray *content;
  gsize pos;
} LoadContentsData;


static void
load_contents_data_free (LoadContentsData *data)
{
  if (data->error)
    g_error_free (data->error);
  if (data->cancellable)
    g_object_unref (data->cancellable);
  if (data->content)
    g_byte_array_free (data->content, TRUE);
  g_object_unref (data->file);
  g_free (data);
}

static void
load_contents_close_callback (GObject *obj,
			     GAsyncResult *close_res,
			     gpointer user_data)
{
  GInputStream *stream = G_INPUT_STREAM (obj);
  LoadContentsData *data = user_data;
  GSimpleAsyncResult *res;

  /* Ignore errors here, we're only reading anyway */
  g_input_stream_close_finish (stream, close_res, NULL);
  g_object_unref (stream);

  res = g_simple_async_result_new (G_OBJECT (data->file),
				   data->callback,
				   data->user_data,
				   g_file_load_contents_async);
  g_simple_async_result_set_op_res_gpointer (res, data, (GDestroyNotify)load_contents_data_free);
  g_simple_async_result_complete (res);
  g_object_unref (res);
}

static void
load_contents_read_callback (GObject *obj,
			    GAsyncResult *read_res,
			    gpointer user_data)
{
  GInputStream *stream = G_INPUT_STREAM (obj);
  LoadContentsData *data = user_data;
  GError *error = NULL;
  gssize read_size;

  read_size = g_input_stream_read_finish (stream, read_res, &error);

  if (read_size <= 0) 
    {
      /* Error or EOF, close the file */
      if (read_size < 0)
	data->error = error;
      g_input_stream_close_async (stream, 0,
				  data->cancellable,
				  load_contents_close_callback, data);
    }
  else if (read_size > 0)
    {
      data->pos += read_size;
      
      g_byte_array_set_size (data->content,
			     data->pos + GET_CONTENT_BLOCK_SIZE);
      g_input_stream_read_async (stream,
				 data->content->data + data->pos,
				 GET_CONTENT_BLOCK_SIZE,
				 0,
				 data->cancellable,
				 load_contents_read_callback,
				 data);
    }
}

static void
load_contents_open_callback (GObject *obj,
			    GAsyncResult *open_res,
			    gpointer user_data)
{
  GFile *file = G_FILE (obj);
  GFileInputStream *stream;
  LoadContentsData *data = user_data;
  GError *error = NULL;
  GSimpleAsyncResult *res;

  stream = g_file_read_finish (file, open_res, &error);

  if (stream)
    {
      g_byte_array_set_size (data->content,
			     data->pos + GET_CONTENT_BLOCK_SIZE);
      g_input_stream_read_async (G_INPUT_STREAM (stream),
				 data->content->data + data->pos,
				 GET_CONTENT_BLOCK_SIZE,
				 0,
				 data->cancellable,
				 load_contents_read_callback,
				 data);
      
    }
  else
    {
      res = g_simple_async_result_new_from_error (G_OBJECT (data->file),
						  data->callback,
						  data->user_data,
						  error);
      g_simple_async_result_complete (res);
      g_error_free (error);
      load_contents_data_free (data);
      g_object_unref (res);
    }
}

void
g_file_load_contents_async (GFile                *file,
			   GCancellable         *cancellable,
			   GAsyncReadyCallback   callback,
			   gpointer              user_data)
{
  LoadContentsData *data;

  data = g_new0 (LoadContentsData, 1);

  if (cancellable)
    data->cancellable = g_object_ref (cancellable);
  data->callback = callback;
  data->user_data = user_data;
  data->content = g_byte_array_new ();
  data->file = g_object_ref (file);

  g_file_read_async (file,
		     0,
		     cancellable,
		     load_contents_open_callback,
		     data);
}

gboolean
g_file_load_contents_finish (GFile                *file,
			    GAsyncResult         *res,
			    char                **contents,
			    gsize                *length,
			    GError              **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  LoadContentsData *data;

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;
  
  g_assert (g_simple_async_result_get_source_tag (simple) == g_file_load_contents_async);
  
  data = g_simple_async_result_get_op_res_gpointer (simple);

  if (data->error)
    {
      g_propagate_error (error, data->error);
      data->error = NULL;
      *contents = NULL;
      if (length)
	*length = 0;
      return FALSE;
    }

  if (length)
    *length = data->pos;

  /* Zero terminate */
  g_byte_array_set_size (data->content,
			 data->pos + 1);
  data->content->data[data->pos] = 0;
  
  *contents = (char *)g_byte_array_free (data->content, FALSE);
  data->content = NULL;

  return TRUE;
}

gboolean
g_file_replace_contents (GFile                *file,
			 const char           *contents,
			 gsize                 length,
			 time_t                mtime,
			 gboolean              make_backup,
			 GCancellable         *cancellable,
			 GError              **error)
{
  GFileOutputStream *out;
  gsize pos, remainder;
  gssize res;

  out = g_file_replace (file,
			mtime,
			make_backup,
			cancellable,
			error);
  if (out == NULL)
    return FALSE;

  pos = 0;
  remainder = length;
  while (remainder > 0 &&
	 (res = g_output_stream_write (G_OUTPUT_STREAM (out),
				       contents + pos,
				       MIN (remainder, GET_CONTENT_BLOCK_SIZE),
				       cancellable,
				       error)) > 0)
    {
      pos += res;
      remainder -= res;
    }
  
  if (remainder > 0 && res < 0)
    {
      /* Ignore errors on close */
      g_output_stream_close (G_OUTPUT_STREAM (out), cancellable, NULL);
      
      /* error is set already */
      return FALSE;
    }
  
  if (!g_output_stream_close (G_OUTPUT_STREAM (out), cancellable, error))
    return FALSE;
  
  return TRUE;
}
