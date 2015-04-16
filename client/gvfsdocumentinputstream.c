#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include <glib.h>
#include <glib/gstdio.h>
#include "gvfsdocumentinputstream.h"

#include <unistd.h>
#include "glib-unix.h"
#include <gio/gfiledescriptorbased.h>
#include <glib/gi18n-lib.h>


struct _GVfsDocumentInputStreamPrivate {
  int fd;
};

static void       g_file_descriptor_based_iface_init   (GFileDescriptorBasedIface *iface);

#define gvfs_document_input_stream_get_type _gvfs_document_input_stream_get_type
G_DEFINE_TYPE_WITH_CODE (GVfsDocumentInputStream, gvfs_document_input_stream, G_TYPE_FILE_INPUT_STREAM,
                         G_ADD_PRIVATE (GVfsDocumentInputStream)
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE_DESCRIPTOR_BASED,
						g_file_descriptor_based_iface_init))

static gssize     gvfs_document_input_stream_read       (GInputStream      *stream,
							void              *buffer,
							gsize              count,
							GCancellable      *cancellable,
							GError           **error);
static gssize     gvfs_document_input_stream_skip       (GInputStream      *stream,
							gsize              count,
							GCancellable      *cancellable,
							GError           **error);
static gboolean   gvfs_document_input_stream_close      (GInputStream      *stream,
							GCancellable      *cancellable,
							GError           **error);
static goffset    gvfs_document_input_stream_tell       (GFileInputStream  *stream);
static gboolean   gvfs_document_input_stream_can_seek   (GFileInputStream  *stream);
static gboolean   gvfs_document_input_stream_seek       (GFileInputStream  *stream,
							goffset            offset,
							GSeekType          type,
							GCancellable      *cancellable,
							GError           **error);
static GFileInfo *gvfs_document_input_stream_query_info (GFileInputStream  *stream,
							const char        *attributes,
							GCancellable      *cancellable,
							GError           **error);
static int        gvfs_document_input_stream_get_fd     (GFileDescriptorBased *stream);

static void
gvfs_document_input_stream_class_init (GVfsDocumentInputStreamClass *klass)
{
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);
  GFileInputStreamClass *file_stream_class = G_FILE_INPUT_STREAM_CLASS (klass);

  stream_class->read_fn = gvfs_document_input_stream_read;
  stream_class->skip = gvfs_document_input_stream_skip;
  stream_class->close_fn = gvfs_document_input_stream_close;
  file_stream_class->tell = gvfs_document_input_stream_tell;
  file_stream_class->can_seek = gvfs_document_input_stream_can_seek;
  file_stream_class->seek = gvfs_document_input_stream_seek;
  file_stream_class->query_info = gvfs_document_input_stream_query_info;
}

static void
g_file_descriptor_based_iface_init (GFileDescriptorBasedIface *iface)
{
  iface->get_fd = gvfs_document_input_stream_get_fd;
}

static void
gvfs_document_input_stream_init (GVfsDocumentInputStream *info)
{
  info->priv = gvfs_document_input_stream_get_instance_private (info);
}

GFileInputStream *
_gvfs_document_input_stream_new (int fd)
{
  GVfsDocumentInputStream *stream;

  stream = g_object_new (GVFS_TYPE_DOCUMENT_INPUT_STREAM, NULL);
  stream->priv->fd = fd;
  
  return G_FILE_INPUT_STREAM (stream);
}

static gssize
gvfs_document_input_stream_read (GInputStream  *stream,
				 void          *buffer,
				 gsize          count,
				 GCancellable  *cancellable,
				 GError       **error)
{
  GVfsDocumentInputStream *file;
  gssize res;

  file = GVFS_DOCUMENT_INPUT_STREAM (stream);

  res = -1;
  while (1)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
	break;
      res = read (file->priv->fd, buffer, count);
      if (res == -1)
	{
          int errsv = errno;

	  if (errsv == EINTR)
	    continue;
	  
	  g_set_error (error, G_IO_ERROR,
		       g_io_error_from_errno (errsv),
		       _("Error reading from file: %s"),
		       g_strerror (errsv));
	}
      
      break;
    }
  
  return res;
}

static gssize
gvfs_document_input_stream_skip (GInputStream  *stream,
				 gsize          count,
				 GCancellable  *cancellable,
				 GError       **error)
{
  off_t start, end;
  GVfsDocumentInputStream *file;

  file = GVFS_DOCUMENT_INPUT_STREAM (stream);
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return -1;
  
  start = lseek (file->priv->fd, 0, SEEK_CUR);
  if (start == -1)
    {
      int errsv = errno;

      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errsv),
		   _("Error seeking in file: %s"),
		   g_strerror (errsv));
      return -1;
    }
  
  end = lseek (file->priv->fd, 0, SEEK_END);
  if (end == -1)
    {
      int errsv = errno;

      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errsv),
		   _("Error seeking in file: %s"),
		   g_strerror (errsv));
      return -1;
    }

  if (end - start > count)
    {
      end = lseek (file->priv->fd, count - (end - start), SEEK_CUR);
      if (end == -1)
	{
	  int errsv = errno;

	  g_set_error (error, G_IO_ERROR,
		       g_io_error_from_errno (errsv),
		       _("Error seeking in file: %s"),
		       g_strerror (errsv));
	  return -1;
	}
    }

  return end - start;
}

static gboolean
gvfs_document_input_stream_close (GInputStream  *stream,
				  GCancellable  *cancellable,
				  GError       **error)
{
  GVfsDocumentInputStream *file;

  file = GVFS_DOCUMENT_INPUT_STREAM (stream);

  if (!g_close (file->priv->fd, NULL))
    {
      int errsv = errno;
      
      g_set_error (error, G_IO_ERROR,
                   g_io_error_from_errno (errsv),
                   _("Error closing file: %s"),
                   g_strerror (errsv));
      return FALSE;
    }

  return TRUE;
}


static goffset
gvfs_document_input_stream_tell (GFileInputStream *stream)
{
  GVfsDocumentInputStream *file;
  off_t pos;

  file = GVFS_DOCUMENT_INPUT_STREAM (stream);
  
  pos = lseek (file->priv->fd, 0, SEEK_CUR);

  if (pos == (off_t)-1)
    return 0;
  
  return pos;
}

static gboolean
gvfs_document_input_stream_can_seek (GFileInputStream *stream)
{
  GVfsDocumentInputStream *file;
  off_t pos;

  file = GVFS_DOCUMENT_INPUT_STREAM (stream);
  
  pos = lseek (file->priv->fd, 0, SEEK_CUR);

  if (pos == (off_t)-1 && errno == ESPIPE)
    return FALSE;
  
  return TRUE;
}

static int
seek_type_to_lseek (GSeekType type)
{
  switch (type)
    {
    default:
    case G_SEEK_CUR:
      return SEEK_CUR;
      
    case G_SEEK_SET:
      return SEEK_SET;
      
    case G_SEEK_END:
      return SEEK_END;
    }
}

static gboolean
gvfs_document_input_stream_seek (GFileInputStream  *stream,
				 goffset            offset,
				 GSeekType          type,
				 GCancellable      *cancellable,
				 GError           **error)
{
  GVfsDocumentInputStream *file;
  off_t pos;

  file = GVFS_DOCUMENT_INPUT_STREAM (stream);

  pos = lseek (file->priv->fd, offset, seek_type_to_lseek (type));

  if (pos == (off_t)-1)
    {
      int errsv = errno;

      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errsv),
		   _("Error seeking in file: %s"),
		   g_strerror (errsv));
      return FALSE;
    }
  
  return TRUE;
}

static GFileInfo *
gvfs_document_input_stream_query_info (GFileInputStream  *stream,
				       const char        *attributes,
				       GCancellable      *cancellable,
				       GError           **error)
{
  GVfsDocumentInputStream *file;
  GFileInfo *info;

  file = GVFS_DOCUMENT_INPUT_STREAM (stream);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;

  info = g_file_info_new ();
  
  return info;
}

static int
gvfs_document_input_stream_get_fd (GFileDescriptorBased *fd_based)
{
  GVfsDocumentInputStream *stream = GVFS_DOCUMENT_INPUT_STREAM (fd_based);
  return stream->priv->fd;
}
