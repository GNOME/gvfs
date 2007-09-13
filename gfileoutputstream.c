#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gfileoutputstream.h>

G_DEFINE_TYPE (GFileOutputStream, g_file_output_stream, G_TYPE_OUTPUT_STREAM);

static GOutputStreamClass *parent_class = NULL;

struct _GFileOutputStreamPrivate {
  guint get_final_mtime : 1;
  time_t final_mtime;
};

static void
g_file_output_stream_class_init (GFileOutputStreamClass *klass)
{
  parent_class = g_type_class_peek_parent (klass);
  
  g_type_class_add_private (klass, sizeof (GFileOutputStreamPrivate));
}

static void
g_file_output_stream_init (GFileOutputStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
					      G_TYPE_FILE_OUTPUT_STREAM,
					      GFileOutputStreamPrivate);
}

GFileInfo *
g_file_output_stream_get_file_info (GFileOutputStream  *stream,
				    GError            **error)
{
  GFileOutputStreamClass *class;
  GOutputStream *output_stream;
  GFileInfo *info;
  
  g_return_val_if_fail (G_IS_FILE_OUTPUT_STREAM (stream), NULL);
  g_return_val_if_fail (stream != NULL, NULL);
  
  output_stream = G_OUTPUT_STREAM (stream);
  
  if (g_output_stream_is_closed (output_stream))
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Stream is already closed"));
      return NULL;
    }
  
  if (g_output_stream_has_pending (output_stream))
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return NULL;
    }
      
  info = NULL;
  
  g_output_stream_set_pending (output_stream, TRUE);
  
  class = G_FILE_OUTPUT_STREAM_GET_CLASS (stream);
  if (class->get_file_info)
    info = class->get_file_info (stream, error);
  else
    g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_NOT_SUPPORTED,
		 _("Stream doesn't support get_file_info"));
  
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

time_t
g_file_output_stream_get_final_mtime (GFileOutputStream  *stream)
{
  g_return_val_if_fail (G_IS_FILE_OUTPUT_STREAM (stream), FALSE);
  g_return_val_if_fail (stream != NULL, FALSE);

  return stream->priv->final_mtime;
}

void
g_file_output_stream_set_final_mtime (GFileOutputStream  *stream,
				      time_t             final_mtime)
{
  g_return_if_fail (G_IS_FILE_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  stream->priv->final_mtime = final_mtime;
}
