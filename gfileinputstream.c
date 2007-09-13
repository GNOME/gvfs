#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gfileinputstream.h>

G_DEFINE_TYPE (GFileInputStream, g_file_input_stream, G_TYPE_INPUT_STREAM);

static GInputStreamClass *parent_class = NULL;

struct _GFileInputStreamPrivate {
  int dummy;
};

static void
g_file_input_stream_class_init (GFileInputStreamClass *klass)
{
  parent_class = g_type_class_peek_parent (klass);
  
  g_type_class_add_private (klass, sizeof (GFileInputStreamPrivate));
}

static void
g_file_input_stream_init (GFileInputStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
					      G_TYPE_FILE_INPUT_STREAM,
					      GFileInputStreamPrivate);
}

GFileInfo *
g_file_input_stream_get_file_info (GFileInputStream  *stream,
				   GError           **error)
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
  
  class = G_FILE_INPUT_STREAM_GET_CLASS (stream);
  if (class->get_file_info)
    info = class->get_file_info (stream, error);
  else
    g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_NOT_SUPPORTED,
		 _("Stream doesn't support get_file_info"));
  
  g_input_stream_set_pending (input_stream, FALSE);
  
  return info;
}
