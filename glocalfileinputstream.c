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
#include <glocalfileinputstream.h>

G_DEFINE_TYPE (GLocalFileInputStream, g_local_file_input_stream, G_TYPE_FILE_INPUT_STREAM);

static GFileInputStreamClass *parent_class = NULL;

struct _GLocalFileInputStreamPrivate {
  char *filename;
  int fd;
};

static gssize   g_local_file_input_stream_read  (GInputStream  *stream,
						 void          *buffer,
						 gsize          count,
						 GError       **error);
static gssize   g_local_file_input_stream_skip  (GInputStream  *stream,
						 gsize          count,
						 GError       **error);
static gboolean g_local_file_input_stream_close (GInputStream  *stream,
						 GError       **error);

static void
g_local_file_input_stream_finalize (GObject *object)
{
  GLocalFileInputStream *file;
  
  file = G_LOCAL_FILE_INPUT_STREAM (object);
  
  g_free (file->priv->filename);
  
  if (G_OBJECT_CLASS (parent_class)->finalize)
    (*G_OBJECT_CLASS (parent_class)->finalize) (object);
}

static void
g_local_file_input_stream_class_init (GLocalFileInputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);
  
  parent_class = g_type_class_peek_parent (klass);
  
  g_type_class_add_private (klass, sizeof (GLocalFileInputStreamPrivate));
  
  gobject_class->finalize = g_local_file_input_stream_finalize;

  stream_class->read = g_local_file_input_stream_read;
  stream_class->skip = g_local_file_input_stream_skip;
  stream_class->close = g_local_file_input_stream_close;
}

static void
g_local_file_input_stream_init (GLocalFileInputStream *info)
{
  info->priv = G_TYPE_INSTANCE_GET_PRIVATE (info,
					    G_TYPE_LOCAL_FILE_INPUT_STREAM,
					    GLocalFileInputStreamPrivate);
}

GFileInputStream *
g_local_file_input_stream_new (const char *filename)
{
  GLocalFileInputStream *stream;

  stream = g_object_new (G_TYPE_LOCAL_FILE_INPUT_STREAM, NULL);

  stream->priv->filename = g_strdup (filename);
  stream->priv->fd = -1;
  
  return G_FILE_INPUT_STREAM (stream);
}

static gboolean
g_local_file_input_stream_open (GLocalFileInputStream *file,
			  GError      **error)
{
  if (file->priv->fd != -1)
    return TRUE;
  
  file->priv->fd = g_open (file->priv->filename, O_RDONLY, 0);
  if (file->priv->fd == -1)
    g_vfs_error_from_errno (error, errno);

  return file->priv->fd != -1;
}
			  

static gssize
g_local_file_input_stream_read (GInputStream *stream,
				void         *buffer,
				gsize         count,
				GError      **error)
{
  GLocalFileInputStream *file;
  gssize res;

  file = G_LOCAL_FILE_INPUT_STREAM (stream);

  if (!g_local_file_input_stream_open (file, error))
    return -1;
  
  while (1)
    {
      res = read (file->priv->fd, buffer, count);
      if (res == -1)
	{
	  if (g_input_stream_is_cancelled (stream))
	    {
	      g_set_error (error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      break;
	    }
	  
	  if (errno == EINTR)
	    continue;
	  
	  g_vfs_error_from_errno (error, errno);
	}
      
      break;
    }
  
  return res;
}

static gssize
g_local_file_input_stream_skip (GInputStream *stream,
				gsize         count,
				GError      **error)
{
  off_t res, start;
  GLocalFileInputStream *file;

  file = G_LOCAL_FILE_INPUT_STREAM (stream);
  
  if (!g_local_file_input_stream_open (file, error))
    return -1;

  start = lseek (file->priv->fd, 0, SEEK_CUR);
  if (start == -1)
    {
      g_vfs_error_from_errno (error, errno);
      return -1;
    }
  
  res = lseek (file->priv->fd, count, SEEK_CUR);
  if (res == -1)
    {
      g_vfs_error_from_errno (error, errno);
      return -1;
    }

  return res - start;
}

static gboolean
g_local_file_input_stream_close (GInputStream *stream,
				 GError      **error)
{
  GLocalFileInputStream *file;
  int res;

  file = G_LOCAL_FILE_INPUT_STREAM (stream);

  if (file->priv->fd == -1)
    return TRUE;

  while (1)
    {
      res = close (file->priv->fd);
      if (res == -1)
	{
	  if (g_input_stream_is_cancelled (stream))
	    {
	      g_set_error (error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      break;
	    }
	  
	  if (errno == EINTR)
	    continue;
	  
	  g_vfs_error_from_errno (error, errno);
	}
      break;
    }

  return res != -1;
}
