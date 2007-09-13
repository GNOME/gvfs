#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gfileinputstream.h>
#include <gseekable.h>

static void     g_file_input_stream_seekable_iface_init (GSeekableIface  *iface);
static goffset  g_file_input_stream_tell                (GSeekable       *seekable);
static gboolean g_file_input_stream_can_seek            (GSeekable       *seekable);
static gboolean g_file_input_stream_seek                (GSeekable       *seekable,
							 goffset          offset,
							 GSeekType        type,
							 GCancellable    *cancellable,
							 GError         **error);
static gboolean g_file_input_stream_can_truncate        (GSeekable       *seekable);
static gboolean g_file_input_stream_truncate            (GSeekable       *seekable,
							 goffset          offset,
							 GCancellable    *cancellable,
							 GError         **error);

G_DEFINE_TYPE_WITH_CODE (GFileInputStream, g_file_input_stream, G_TYPE_INPUT_STREAM,
			 G_IMPLEMENT_INTERFACE (G_TYPE_SEEKABLE,
						g_file_input_stream_seekable_iface_init))

struct _GFileInputStreamPrivate {
  guint dummy;
};

static void
g_file_input_stream_class_init (GFileInputStreamClass *klass)
{
  g_type_class_add_private (klass, sizeof (GFileInputStreamPrivate));
}

static void
g_file_input_stream_seekable_iface_init (GSeekableIface *iface)
{
  iface->tell = g_file_input_stream_tell;
  iface->can_seek = g_file_input_stream_can_seek;
  iface->seek = g_file_input_stream_seek;
  iface->can_truncate = g_file_input_stream_can_truncate;
  iface->truncate = g_file_input_stream_truncate;
}

static void
g_file_input_stream_init (GFileInputStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
					      G_TYPE_FILE_INPUT_STREAM,
					      GFileInputStreamPrivate);
}

GFileInfo *
g_file_input_stream_get_file_info (GFileInputStream     *stream,
				   GFileInfoRequestFlags requested,
				   char                 *attributes,
				   GCancellable         *cancellable,
				   GError              **error)
{
  GFileInputStreamClass *class;
  GInputStream *input_stream;
  GFileInfo *info;
  
  g_return_val_if_fail (G_IS_FILE_INPUT_STREAM (stream), NULL);
  g_return_val_if_fail (stream != NULL, NULL);
  
  input_stream = G_INPUT_STREAM (stream);
  
  if (g_input_stream_is_closed (input_stream))
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Stream is already closed"));
      return NULL;
    }
  
  if (g_input_stream_has_pending (input_stream))
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return NULL;
    }
      
  info = NULL;
  
  g_input_stream_set_pending (input_stream, TRUE);

  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  class = G_FILE_INPUT_STREAM_GET_CLASS (stream);
  if (class->get_file_info)
    info = class->get_file_info (stream, requested, attributes, cancellable, error);
  else
    g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_NOT_SUPPORTED,
		 _("Stream doesn't support get_file_info"));

  if (cancellable)
    g_pop_current_cancellable (cancellable);
  
  g_input_stream_set_pending (input_stream, FALSE);
  
  return info;
}

static goffset
g_file_input_stream_tell (GSeekable *seekable)
{
  GFileInputStream *file;
  GFileInputStreamClass *class;
  goffset offset;

  file = G_FILE_INPUT_STREAM (seekable);
  class = G_FILE_INPUT_STREAM_GET_CLASS (file);

  offset = 0;
  if (class->tell)
    offset = class->tell (file);

  return offset;
}

static gboolean
g_file_input_stream_can_seek (GSeekable *seekable)
{
  GFileInputStream *file;
  GFileInputStreamClass *class;
  gboolean can_seek;

  file = G_FILE_INPUT_STREAM (seekable);
  class = G_FILE_INPUT_STREAM_GET_CLASS (file);

  can_seek = FALSE;
  if (class->seek)
    {
      can_seek = TRUE;
      if (class->can_seek)
	can_seek = class->can_seek (file);
    }
  
  return can_seek;
}

static gboolean
g_file_input_stream_seek (GSeekable  *seekable,
			  goffset     offset,
			  GSeekType   type,
			  GCancellable  *cancellable,
			  GError    **error)
{
  GFileInputStream *file;
  GFileInputStreamClass *class;
  GInputStream *input_stream;
  gboolean res;

  input_stream = G_INPUT_STREAM (seekable);
  file = G_FILE_INPUT_STREAM (seekable);
  class = G_FILE_INPUT_STREAM_GET_CLASS (file);

  if (g_input_stream_is_closed (input_stream))
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Stream is already closed"));
      return FALSE;
    }
  
  if (g_input_stream_has_pending (input_stream))
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return FALSE;
    }
  
  if (!class->seek)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_NOT_SUPPORTED,
		   _("Seek not supported on stream"));
      return FALSE;
    }

  g_input_stream_set_pending (input_stream, TRUE);
  
  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  res = class->seek (file, offset, type, cancellable, error);
  
  if (cancellable)
    g_pop_current_cancellable (cancellable);

  g_input_stream_set_pending (input_stream, FALSE);
  
  return res;
}

static gboolean
g_file_input_stream_can_truncate (GSeekable  *seekable)
{
  return FALSE;
}

static gboolean
g_file_input_stream_truncate (GSeekable  *seekable,
			      goffset     offset,
			      GCancellable  *cancellable,
			      GError    **error)
{
  g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_NOT_SUPPORTED,
	       _("Truncate not allowed on input stream"));
  return FALSE;
}
