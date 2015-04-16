/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include <unistd.h>
#include <gio/gfiledescriptorbased.h>

#include "gvfsdocumentoutputstream.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

struct _GVfsDocumentOutputStreamPrivate {
  char *etag;
  int fd;
  guint32 id;
  char *doc_handle;
};

static void       g_file_descriptor_based_iface_init   (GFileDescriptorBasedIface *iface);

#define gvfs_document_output_stream_get_type _gvfs_document_output_stream_get_type
G_DEFINE_TYPE_WITH_CODE (GVfsDocumentOutputStream, gvfs_document_output_stream, G_TYPE_FILE_OUTPUT_STREAM,
                         G_ADD_PRIVATE (GVfsDocumentOutputStream)
			 G_IMPLEMENT_INTERFACE (G_TYPE_FILE_DESCRIPTOR_BASED,
						g_file_descriptor_based_iface_init))



static gssize     gvfs_document_output_stream_write        (GOutputStream         *stream,
								 const void            *buffer,
								 gsize                  count,
								 GCancellable          *cancellable,
								 GError               **error);
static gboolean   gvfs_document_output_stream_close        (GOutputStream         *stream,
								 GCancellable          *cancellable,
								 GError               **error);
static GFileInfo *gvfs_document_output_stream_query_info   (GFileOutputStream     *stream,
								 const char            *attributes,
								 GCancellable          *cancellable,
								 GError               **error);
static char *     gvfs_document_output_stream_get_etag     (GFileOutputStream     *stream);
static goffset    gvfs_document_output_stream_tell         (GFileOutputStream     *stream);
static gboolean   gvfs_document_output_stream_can_seek     (GFileOutputStream     *stream);
static gboolean   gvfs_document_output_stream_seek         (GFileOutputStream     *stream,
								 goffset                offset,
								 GSeekType              type,
								 GCancellable          *cancellable,
								 GError               **error);
static gboolean   gvfs_document_output_stream_can_truncate (GFileOutputStream     *stream);
static gboolean   gvfs_document_output_stream_truncate     (GFileOutputStream     *stream,
								 goffset                size,
								 GCancellable          *cancellable,
								 GError               **error);
static int        gvfs_document_output_stream_get_fd       (GFileDescriptorBased  *stream);


static void
gvfs_document_output_stream_finalize (GObject *object)
{
  GVfsDocumentOutputStream *file;
  
  file = GVFS_DOCUMENT_OUTPUT_STREAM (object);
  
  g_free (file->priv->doc_handle);
  g_free (file->priv->etag);

  G_OBJECT_CLASS (gvfs_document_output_stream_parent_class)->finalize (object);
}

static void
gvfs_document_output_stream_class_init (GVfsDocumentOutputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GOutputStreamClass *stream_class = G_OUTPUT_STREAM_CLASS (klass);
  GFileOutputStreamClass *file_stream_class = G_FILE_OUTPUT_STREAM_CLASS (klass);

  gobject_class->finalize = gvfs_document_output_stream_finalize;

  stream_class->write_fn = gvfs_document_output_stream_write;
  stream_class->close_fn = gvfs_document_output_stream_close;
  file_stream_class->query_info = gvfs_document_output_stream_query_info;
  file_stream_class->get_etag = gvfs_document_output_stream_get_etag;
  file_stream_class->tell = gvfs_document_output_stream_tell;
  file_stream_class->can_seek = gvfs_document_output_stream_can_seek;
  file_stream_class->seek = gvfs_document_output_stream_seek;
  file_stream_class->can_truncate = gvfs_document_output_stream_can_truncate;
  file_stream_class->truncate_fn = gvfs_document_output_stream_truncate;
}

static void
g_file_descriptor_based_iface_init (GFileDescriptorBasedIface *iface)
{
  iface->get_fd = gvfs_document_output_stream_get_fd;
}

static void
gvfs_document_output_stream_init (GVfsDocumentOutputStream *stream)
{
  stream->priv = gvfs_document_output_stream_get_instance_private (stream);
}

static gssize
gvfs_document_output_stream_write (GOutputStream  *stream,
					const void     *buffer,
					gsize           count,
					GCancellable   *cancellable,
					GError        **error)
{
  GVfsDocumentOutputStream *file;
  gssize res;

  file = GVFS_DOCUMENT_OUTPUT_STREAM (stream);

  while (1)
    {
      if (g_cancellable_set_error_if_cancelled (cancellable, error))
	return -1;
      res = write (file->priv->fd, buffer, count);
      if (res == -1)
	{
          int errsv = errno;

	  if (errsv == EINTR)
	    continue;
	  
	  g_set_error (error, G_IO_ERROR,
		       g_io_error_from_errno (errsv),
		       _("Error writing to file: %s"),
		       g_strerror (errsv));
	}
      
      break;
    }
  
  return res;
}

static GVariant *
sync_document_call (GVfsDocumentOutputStream *stream,
		    const char *method,
		    GVariant *parameters,
		    const GVariantType  *reply_type,
		    GUnixFDList  **out_fd_list,
		    GCancellable *cancellable,
		    GError **error)
{
  GVariant *res;
  GDBusConnection *bus;
  GUnixFDList *ret_fd_list;
  GVariant *reply, *fd_v;
  char *path;
  int handle, fd;

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, error);
  if (bus == NULL)
    return NULL;

  path = g_build_filename ("/org/freedesktop/portal/document", stream->priv->doc_handle, NULL);

  res = g_dbus_connection_call_with_unix_fd_list_sync (bus,
						       "org.freedesktop.portal.DocumentPortal",
						       path,
						       "org.freedesktop.portal.Document",
						       method,
						       parameters, reply_type,
						       G_DBUS_CALL_FLAGS_NONE,
						       -1,
						       NULL,
						       out_fd_list,
						       cancellable,
						       error);

  g_object_unref (bus);
  return res;
}

static gboolean
gvfs_document_output_stream_close (GOutputStream  *stream,
					GCancellable   *cancellable,
					GError        **error)
{
  GVfsDocumentOutputStream *file;
  gboolean failed;
  GVariant *reply;
  
  file = GVFS_DOCUMENT_OUTPUT_STREAM (stream);

  failed = g_cancellable_set_error_if_cancelled (cancellable, error);

  /* Always close, even if cancelled */
  if (!g_close (file->priv->fd, NULL) && !failed)
    {
      int errsv = errno;
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errsv),
		   _("Error closing file: %s"),
		   g_strerror (errsv));
    }

  if (failed)
    {
      reply = sync_document_call (file, "AbortUpdate",
				  g_variant_new ("(u)", file->priv->id),
				  G_VARIANT_TYPE("()"),
				  NULL,
				  NULL, NULL);
      g_variant_unref (reply);
      return FALSE;
    }

  /* TODO: What if this is cancelled? Do we leak the update? */
  reply = sync_document_call (file, "FinishUpdate",
			      g_variant_new ("(u)", file->priv->id),
			      G_VARIANT_TYPE("()"),
			      NULL,
			      cancellable, error);
  if (reply == NULL)
    return FALSE;

  g_variant_unref (reply);

  return TRUE;
}

static char *
gvfs_document_output_stream_get_etag (GFileOutputStream *stream)
{
  GVfsDocumentOutputStream *file;

  file = GVFS_DOCUMENT_OUTPUT_STREAM (stream);
  
  return g_strdup (file->priv->etag);
}

static goffset
gvfs_document_output_stream_tell (GFileOutputStream *stream)
{
  GVfsDocumentOutputStream *file;
  off_t pos;

  file = GVFS_DOCUMENT_OUTPUT_STREAM (stream);
  
  pos = lseek (file->priv->fd, 0, SEEK_CUR);

  if (pos == (off_t)-1)
    return 0;
  
  return pos;
}

static gboolean
gvfs_document_output_stream_can_seek (GFileOutputStream *stream)
{
  GVfsDocumentOutputStream *file;
  off_t pos;

  file = GVFS_DOCUMENT_OUTPUT_STREAM (stream);
  
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
gvfs_document_output_stream_seek (GFileOutputStream  *stream,
				       goffset             offset,
				       GSeekType           type,
				       GCancellable       *cancellable,
				       GError            **error)
{
  GVfsDocumentOutputStream *file;
  off_t pos;

  file = GVFS_DOCUMENT_OUTPUT_STREAM (stream);

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

static gboolean
gvfs_document_output_stream_can_truncate (GFileOutputStream *stream)
{
  /* We can't truncate pipes and stuff where we can't seek */
  return gvfs_document_output_stream_can_seek (stream);
}

static gboolean
gvfs_document_output_stream_truncate (GFileOutputStream  *stream,
					   goffset             size,
					   GCancellable       *cancellable,
					   GError            **error)
{
  GVfsDocumentOutputStream *file;
  int res;

  file = GVFS_DOCUMENT_OUTPUT_STREAM (stream);

 restart:
  res = ftruncate (file->priv->fd, size);
  
  if (res == -1)
    {
      int errsv = errno;

      if (errsv == EINTR)
	{
	  if (g_cancellable_set_error_if_cancelled (cancellable, error))
	    return FALSE;
	  goto restart;
	}

      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errsv),
		   _("Error truncating file: %s"),
		   g_strerror (errsv));
      return FALSE;
    }
  
  return TRUE;
}


static GFileInfo *
gvfs_document_output_stream_query_info (GFileOutputStream  *stream,
					     const char         *attributes,
					     GCancellable       *cancellable,
					     GError            **error)
{
  GVfsDocumentOutputStream *file;
  GFileInfo *info;

  file = GVFS_DOCUMENT_OUTPUT_STREAM (stream);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;

  info = g_file_info_new ();
  
  return info;
}

GFileOutputStream *
gvfs_document_output_stream_new (const char *handle,
				       guint32   id,
				       int fd)
{
  GVfsDocumentOutputStream *stream;

  stream = g_object_new (GVFS_TYPE_DOCUMENT_OUTPUT_STREAM, NULL);
  stream->priv->doc_handle = g_strdup (handle);
  stream->priv->id = id;
  stream->priv->fd = fd;

  return G_FILE_OUTPUT_STREAM (stream);
}

static int
gvfs_document_output_stream_get_fd (GFileDescriptorBased *fd_based)
{
  GVfsDocumentOutputStream *stream = GVFS_DOCUMENT_OUTPUT_STREAM (fd_based);

  return stream->priv->fd;
}
