#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gfileoutputstream.h>
#include <gseekable.h>

static void     g_file_output_stream_seekable_iface_init (GSeekableIface  *iface);
static goffset  g_file_output_stream_tell                (GSeekable       *seekable);
static gboolean g_file_output_stream_can_seek            (GSeekable       *seekable);
static gboolean g_file_output_stream_seek                (GSeekable       *seekable,
							  goffset          offset,
							  GSeekType        type,
							  GCancellable    *cancellable,
							  GError         **error);
static gboolean g_file_output_stream_can_truncate        (GSeekable       *seekable);
static gboolean g_file_output_stream_truncate            (GSeekable       *seekable,
							  goffset          offset,
							  GCancellable    *cancellable,
							  GError         **error);

G_DEFINE_TYPE_WITH_CODE (GFileOutputStream, g_file_output_stream, G_TYPE_OUTPUT_STREAM,
			 G_IMPLEMENT_INTERFACE (G_TYPE_SEEKABLE,
						g_file_output_stream_seekable_iface_init));

struct _GFileOutputStreamPrivate {
  guint get_final_mtime : 1;
  GTimeVal final_mtime;
};

static void
g_file_output_stream_class_init (GFileOutputStreamClass *klass)
{
  g_type_class_add_private (klass, sizeof (GFileOutputStreamPrivate));
}

static void
g_file_output_stream_seekable_iface_init (GSeekableIface *iface)
{
  iface->tell = g_file_output_stream_tell;
  iface->can_seek = g_file_output_stream_can_seek;
  iface->seek = g_file_output_stream_seek;
  iface->can_truncate = g_file_output_stream_can_truncate;
  iface->truncate = g_file_output_stream_truncate;
}

static void
g_file_output_stream_init (GFileOutputStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
					      G_TYPE_FILE_OUTPUT_STREAM,
					      GFileOutputStreamPrivate);
}

GFileInfo *
g_file_output_stream_get_file_info (GFileOutputStream      *stream,
				    char                   *attributes,
				    GCancellable           *cancellable,
				    GError                **error)
{
  GFileOutputStreamClass *class;
  GOutputStream *output_stream;
  GFileInfo *info;
  
  g_return_val_if_fail (G_IS_FILE_OUTPUT_STREAM (stream), NULL);
  g_return_val_if_fail (stream != NULL, NULL);
  
  output_stream = G_OUTPUT_STREAM (stream);
  
  if (g_output_stream_is_closed (output_stream))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		   _("Stream is already closed"));
      return NULL;
    }
  
  if (g_output_stream_has_pending (output_stream))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return NULL;
    }
      
  info = NULL;
  
  g_output_stream_set_pending (output_stream, TRUE);
  
  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  class = G_FILE_OUTPUT_STREAM_GET_CLASS (stream);
  if (class->get_file_info)
    info = class->get_file_info (stream, attributes, cancellable, error);
  else
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		 _("Stream doesn't support get_file_info"));
  
  if (cancellable)
    g_pop_current_cancellable (cancellable);
  
  g_output_stream_set_pending (output_stream, FALSE);
  
  return info;
}

void
g_file_output_stream_set_should_get_final_mtime (GFileOutputStream  *stream,
						 gboolean           get_final_mtime)
{
  g_return_if_fail (G_IS_FILE_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  stream->priv->get_final_mtime = get_final_mtime;
}

gboolean
g_file_output_stream_get_should_get_final_mtime (GFileOutputStream  *stream)
{
  g_return_val_if_fail (G_IS_FILE_OUTPUT_STREAM (stream), FALSE);
  g_return_val_if_fail (stream != NULL, FALSE);

  return stream->priv->get_final_mtime;
}

void
g_file_output_stream_get_final_mtime (GFileOutputStream  *stream,
				      GTimeVal *final_mtime)
{
  g_return_if_fail (G_IS_FILE_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  *final_mtime = stream->priv->final_mtime;
}

void
g_file_output_stream_set_final_mtime (GFileOutputStream  *stream,
				      GTimeVal *final_mtime)
{
  g_return_if_fail (G_IS_FILE_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  stream->priv->final_mtime = *final_mtime;
}

static goffset
g_file_output_stream_tell (GSeekable *seekable)
{
  GFileOutputStream *file;
  GFileOutputStreamClass *class;
  goffset offset;

  file = G_FILE_OUTPUT_STREAM (seekable);
  class = G_FILE_OUTPUT_STREAM_GET_CLASS (file);

  offset = 0;
  if (class->tell)
    offset = class->tell (file);

  return offset;
}

static gboolean
g_file_output_stream_can_seek (GSeekable *seekable)
{
  GFileOutputStream *file;
  GFileOutputStreamClass *class;
  gboolean can_seek;

  file = G_FILE_OUTPUT_STREAM (seekable);
  class = G_FILE_OUTPUT_STREAM_GET_CLASS (file);

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
g_file_output_stream_seek (GSeekable  *seekable,
			   goffset     offset,
			   GSeekType   type,
			   GCancellable  *cancellable,
			   GError    **error)
{
  GFileOutputStream *file;
  GFileOutputStreamClass *class;
  GOutputStream *output_stream;
  gboolean res;

  output_stream = G_OUTPUT_STREAM (seekable);
  file = G_FILE_OUTPUT_STREAM (seekable);
  class = G_FILE_OUTPUT_STREAM_GET_CLASS (file);

  if (g_output_stream_is_closed (output_stream))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		   _("Stream is already closed"));
      return FALSE;
    }
  
  if (g_output_stream_has_pending (output_stream))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return FALSE;
    }
  
  if (!class->seek)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		   _("Seek not supported on stream"));
      return FALSE;
    }

  g_output_stream_set_pending (output_stream, TRUE);
  
  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  res = class->seek (file, offset, type, cancellable, error);
  
  if (cancellable)
    g_pop_current_cancellable (cancellable);

  g_output_stream_set_pending (output_stream, FALSE);
  
  return res;
}

static gboolean
g_file_output_stream_can_truncate (GSeekable  *seekable)
{
  GFileOutputStream *file;
  GFileOutputStreamClass *class;
  gboolean can_truncate;

  file = G_FILE_OUTPUT_STREAM (seekable);
  class = G_FILE_OUTPUT_STREAM_GET_CLASS (file);

  can_truncate = FALSE;
  if (class->truncate)
    {
      can_truncate = TRUE;
      if (class->can_truncate)
	can_truncate = class->can_truncate (file);
    }
  
  return can_truncate;
}

static gboolean
g_file_output_stream_truncate (GSeekable     *seekable,
			       goffset        size,
			       GCancellable  *cancellable,
			       GError       **error)
{
  GFileOutputStream *file;
  GFileOutputStreamClass *class;
  GOutputStream *output_stream;
  gboolean res;

  output_stream = G_OUTPUT_STREAM (seekable);
  file = G_FILE_OUTPUT_STREAM (seekable);
  class = G_FILE_OUTPUT_STREAM_GET_CLASS (file);

  if (g_output_stream_is_closed (output_stream))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
		   _("Stream is already closed"));
      return FALSE;
    }
  
  if (g_output_stream_has_pending (output_stream))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return FALSE;
    }
  
  if (!class->truncate)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		   _("Truncate not supported on stream"));
      return FALSE;
    }

  g_output_stream_set_pending (output_stream, TRUE);
  
  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  res = class->truncate (file, size, cancellable, error);
  
  if (cancellable)
    g_pop_current_cancellable (cancellable);

  g_output_stream_set_pending (output_stream, FALSE);
  
  return res;
}
