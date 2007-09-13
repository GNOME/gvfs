#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include "gvfserror.h"
#include "gfileinputstreamlocal.h"
#include "gfileinfolocal.h"


G_DEFINE_TYPE (GFileInputStreamLocal, g_file_input_stream_local, G_TYPE_FILE_INPUT_STREAM);

struct _GFileInputStreamLocalPrivate {
  int fd;
};

static gssize     g_file_input_stream_local_read          (GInputStream           *stream,
							   void                   *buffer,
							   gsize                   count,
							   GCancellable           *cancellable,
							   GError                **error);
static gssize     g_file_input_stream_local_skip          (GInputStream           *stream,
							   gsize                   count,
							   GCancellable           *cancellable,
							   GError                **error);
static gboolean   g_file_input_stream_local_close         (GInputStream           *stream,
							   GCancellable           *cancellable,
							   GError                **error);
static GFileInfo *g_file_input_stream_local_get_file_info (GFileInputStream       *stream,
							   GFileInfoRequestFlags   requested,
							   char                   *attributes,
							   GCancellable           *cancellable,
							   GError                **error);

static void
g_file_input_stream_local_finalize (GObject *object)
{
  GFileInputStreamLocal *file;
  
  file = G_FILE_INPUT_STREAM_LOCAL (object);
  
  if (G_OBJECT_CLASS (g_file_input_stream_local_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_input_stream_local_parent_class)->finalize) (object);
}

static void
g_file_input_stream_local_class_init (GFileInputStreamLocalClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);
  GFileInputStreamClass *file_stream_class = G_FILE_INPUT_STREAM_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GFileInputStreamLocalPrivate));
  
  gobject_class->finalize = g_file_input_stream_local_finalize;

  stream_class->read = g_file_input_stream_local_read;
  stream_class->skip = g_file_input_stream_local_skip;
  stream_class->close = g_file_input_stream_local_close;
  file_stream_class->get_file_info = g_file_input_stream_local_get_file_info;
}

static void
g_file_input_stream_local_init (GFileInputStreamLocal *info)
{
  info->priv = G_TYPE_INSTANCE_GET_PRIVATE (info,
					    G_TYPE_FILE_INPUT_STREAM_LOCAL,
					    GFileInputStreamLocalPrivate);
}

GFileInputStream *
g_file_input_stream_local_new (int fd)
{
  GFileInputStreamLocal *stream;

  stream = g_object_new (G_TYPE_FILE_INPUT_STREAM_LOCAL, NULL);
  stream->priv->fd = fd;
  
  return G_FILE_INPUT_STREAM (stream);
}

static gssize
g_file_input_stream_local_read (GInputStream *stream,
				void         *buffer,
				gsize         count,
				GCancellable *cancellable,
				GError      **error)
{
  GFileInputStreamLocal *file;
  gssize res;

  file = G_FILE_INPUT_STREAM_LOCAL (stream);

  while (1)
    {
      if (g_cancellable_is_cancelled (cancellable))
	{
	  g_set_error (error,
		       G_VFS_ERROR,
		       G_VFS_ERROR_CANCELLED,
		       _("Operation was cancelled"));
	  break;
	}
      res = read (file->priv->fd, buffer, count);
      if (res == -1)
	{
	  if (errno == EINTR)
	    continue;
	  
	  g_set_error (error, G_FILE_ERROR,
		       g_file_error_from_errno (errno),
		       _("Error reading from file: %s"),
		       g_strerror (errno));
	}
      
      break;
    }
  
  return res;
}

static gssize
g_file_input_stream_local_skip (GInputStream *stream,
				gsize         count,
				GCancellable *cancellable,
				GError      **error)
{
  off_t res, start;
  GFileInputStreamLocal *file;

  file = G_FILE_INPUT_STREAM_LOCAL (stream);
  
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return -1;
    }
  
  start = lseek (file->priv->fd, 0, SEEK_CUR);
  if (start == -1)
    {
      g_set_error (error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error seeking in file: %s"),
		   g_strerror (errno));
      return -1;
    }
  
  res = lseek (file->priv->fd, count, SEEK_CUR);
  if (res == -1)
    {
      g_set_error (error, G_FILE_ERROR,
		   g_file_error_from_errno (errno),
		   _("Error seeking in file: %s"),
		   g_strerror (errno));
      return -1;
    }

  return res - start;
}

static gboolean
g_file_input_stream_local_close (GInputStream *stream,
				 GCancellable *cancellable,
				 GError      **error)
{
  GFileInputStreamLocal *file;
  int res;

  file = G_FILE_INPUT_STREAM_LOCAL (stream);

  if (file->priv->fd == -1)
    return TRUE;

  while (1)
    {
      res = close (file->priv->fd);
      if (res == -1)
	{
	  g_set_error (error, G_FILE_ERROR,
		       g_file_error_from_errno (errno),
		       _("Error closing file: %s"),
		       g_strerror (errno));
	}
      break;
    }

  return res != -1;
}

static GFileInfo *
g_file_input_stream_local_get_file_info (GFileInputStream     *stream,
					 GFileInfoRequestFlags requested,
					 char                 *attributes,
					 GCancellable         *cancellable,
					 GError              **error)
{
  GFileInputStreamLocal *file;

  file = G_FILE_INPUT_STREAM_LOCAL (stream);

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
  
  return g_file_info_local_get_from_fd (file->priv->fd,
					requested,
					attributes,
					error);
}
