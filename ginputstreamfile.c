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
#include <ginputstreamfile.h>

G_DEFINE_TYPE (GInputStreamFile, g_input_stream_file, G_TYPE_INPUT_STREAM);

static GInputStreamClass *parent_class = NULL;

struct _GInputStreamFilePrivate {
  char *filename;
  int fd;
};

static gssize   g_input_stream_file_read  (GInputStream  *stream,
					   void          *buffer,
					   gsize          count,
					   GError       **error);
static gssize   g_input_stream_file_skip  (GInputStream  *stream,
					   gsize          count,
					   GError       **error);
static gboolean g_input_stream_file_close (GInputStream  *stream,
					   GError       **error);

static void
g_input_stream_file_finalize (GObject *object)
{
  GInputStreamFile *file;
  
  file = G_INPUT_STREAM_FILE (object);
  
  g_free (file->priv->filename);
  
  if (G_OBJECT_CLASS (parent_class)->finalize)
    (*G_OBJECT_CLASS (parent_class)->finalize) (object);
}

static void
g_input_stream_file_class_init (GInputStreamFileClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);
  
  parent_class = g_type_class_peek_parent (klass);
  
  g_type_class_add_private (klass, sizeof (GInputStreamFilePrivate));
  
  gobject_class->finalize = g_input_stream_file_finalize;

  stream_class->read = g_input_stream_file_read;
  stream_class->skip = g_input_stream_file_skip;
  stream_class->close = g_input_stream_file_close;
}

static void
g_input_stream_file_init (GInputStreamFile *info)
{
  info->priv = G_TYPE_INSTANCE_GET_PRIVATE (info,
					    G_TYPE_INPUT_STREAM_FILE,
					    GInputStreamFilePrivate);
}

GInputStream *
g_input_stream_file_new (const char *filename)
{
  GInputStreamFile *stream;

  stream = g_object_new (G_TYPE_INPUT_STREAM_FILE, NULL);

  stream->priv->filename = g_strdup (filename);
  stream->priv->fd = -1;
  
  return G_INPUT_STREAM (stream);
}

int
g_input_stream_file_get_fd (GInputStream *stream)
{
  GInputStreamFile *file;
  
  file = G_INPUT_STREAM_FILE (stream);
  
  return file->priv->fd;
}

static gboolean
g_input_stream_file_open (GInputStreamFile *file,
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
g_input_stream_file_read (GInputStream *stream,
			  void         *buffer,
			  gsize         count,
			  GError      **error)
{
  GInputStreamFile *file;
  gssize res;

  file = G_INPUT_STREAM_FILE (stream);

  if (!g_input_stream_file_open (file, error))
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
g_input_stream_file_skip (GInputStream *stream,
			  gsize         count,
			  GError      **error)
{
  off_t res, start;
  GInputStreamFile *file;

  file = G_INPUT_STREAM_FILE (stream);
  
  if (!g_input_stream_file_open (file, error))
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
g_input_stream_file_close (GInputStream *stream,
			   GError      **error)
{
  GInputStreamFile *file;
  int res;

  file = G_INPUT_STREAM_FILE (stream);

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
