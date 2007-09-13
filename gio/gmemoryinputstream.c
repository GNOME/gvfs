#include <config.h>
#include <glib/gi18n-lib.h>


#include "gmemoryinputstream.h"
#include "ginputstream.h"
#include "gseekable.h"
#include "string.h"
#include "gsimpleasyncresult.h"

struct _GMemoryInputStreamPrivate {

  const guint8 *buffer;      
  gsize   pos;
  gsize   len;

};

static gssize   g_memory_input_stream_read         (GInputStream         *stream,
						    void                 *buffer,
						    gsize                 count,
						    GCancellable         *cancellable,
						    GError              **error);
static gssize   g_memory_input_stream_skip         (GInputStream         *stream,
						    gsize                 count,
						    GCancellable         *cancellable,
						    GError              **error);
static gboolean g_memory_input_stream_close        (GInputStream         *stream,
						    GCancellable         *cancellable,
						    GError              **error);
static void     g_memory_input_stream_read_async   (GInputStream         *stream,
						    void                 *buffer,
						    gsize                 count,
						    int                   io_priority,
						    GCancellable         *cancellable,
						    GAsyncReadyCallback   callback,
						    gpointer              user_data);
static gssize   g_memory_input_stream_read_finish  (GInputStream         *stream,
						    GAsyncResult         *result,
						    GError              **error);
static void     g_memory_input_stream_skip_async   (GInputStream         *stream,
						    gsize                 count,
						    int                   io_priority,
						    GCancellable         *cancellabl,
						    GAsyncReadyCallback   callback,
						    gpointer              datae);
static gssize   g_memory_input_stream_skip_finish  (GInputStream         *stream,
						    GAsyncResult         *result,
						    GError              **error);
static void     g_memory_input_stream_close_async  (GInputStream         *stream,
						    int                   io_priority,
						    GCancellable         *cancellabl,
						    GAsyncReadyCallback   callback,
						    gpointer              data);
static gboolean g_memory_input_stream_close_finish (GInputStream         *stream,
						    GAsyncResult         *result,
						    GError              **error);

static void     g_memory_input_stream_seekable_iface_init (GSeekableIface  *iface);
static goffset  g_memory_input_stream_tell                (GSeekable       *seekable);
static gboolean g_memory_input_stream_can_seek            (GSeekable       *seekable);
static gboolean g_memory_input_stream_seek                (GSeekable       *seekable,
                                                           goffset          offset,
                                                           GSeekType        type,
                                                           GCancellable    *cancellable,
                                                           GError         **error);
static gboolean g_memory_input_stream_can_truncate        (GSeekable       *seekable);
static gboolean g_memory_input_stream_truncate            (GSeekable       *seekable,
                                                           goffset          offset,
                                                           GCancellable    *cancellable,
                                                           GError         **error);

G_DEFINE_TYPE_WITH_CODE (GMemoryInputStream, g_memory_input_stream, G_TYPE_INPUT_STREAM,
                         G_IMPLEMENT_INTERFACE (G_TYPE_SEEKABLE,
                                                g_memory_input_stream_seekable_iface_init))


static void
g_memory_input_stream_class_init (GMemoryInputStreamClass *klass)
{
  GInputStreamClass *istream_class;

  g_type_class_add_private (klass, sizeof (GMemoryInputStreamPrivate));

  istream_class = G_INPUT_STREAM_CLASS (klass);
  istream_class->read  = g_memory_input_stream_read;
  istream_class->skip  = g_memory_input_stream_skip;
  istream_class->close = g_memory_input_stream_close;

  istream_class->read_async  = g_memory_input_stream_read_async;
  istream_class->read_finish  = g_memory_input_stream_read_finish;
  istream_class->skip_async  = g_memory_input_stream_skip_async;
  istream_class->skip_finish  = g_memory_input_stream_skip_finish;
  istream_class->close_async = g_memory_input_stream_close_async;
  istream_class->close_finish = g_memory_input_stream_close_finish;
}

static void
g_memory_input_stream_seekable_iface_init (GSeekableIface *iface)
{
  iface->tell         = g_memory_input_stream_tell;
  iface->can_seek     = g_memory_input_stream_can_seek;
  iface->seek         = g_memory_input_stream_seek;
  iface->can_truncate = g_memory_input_stream_can_truncate;
  iface->truncate     = g_memory_input_stream_truncate;
}


static void
g_memory_input_stream_init (GMemoryInputStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
                                              G_TYPE_MEMORY_INPUT_STREAM,
                                              GMemoryInputStreamPrivate);
}

GInputStream *
g_memory_input_stream_from_data (const void *data, gsize len)
{
  GInputStream *stream;
  GMemoryInputStream *memory_stream;

  g_return_val_if_fail (data != NULL, NULL);

  stream = g_object_new (G_TYPE_MEMORY_INPUT_STREAM, NULL);
  memory_stream = G_MEMORY_INPUT_STREAM (stream);

  memory_stream->priv->buffer = data;
  memory_stream->priv->len = len;

  return stream;

}

static gssize
g_memory_input_stream_read (GInputStream *stream,
                            void         *buffer,
                            gsize         count,
                            GCancellable *cancellable,
                            GError      **error)
{
  GMemoryInputStream *memory_stream;
  GMemoryInputStreamPrivate * priv;

  memory_stream = G_MEMORY_INPUT_STREAM (stream);
  priv = memory_stream->priv;

  count = MIN (count, priv->len - priv->pos);
  memcpy (buffer, priv->buffer + priv->pos, count);
  priv->pos += count;

  return count;
}
static gssize
g_memory_input_stream_skip (GInputStream              *stream,
                            gsize                      count,
                            GCancellable              *cancellable,
                            GError                   **error)
{
  GMemoryInputStream *memory_stream;
  GMemoryInputStreamPrivate *priv;

  memory_stream = G_MEMORY_INPUT_STREAM (stream);
  priv = memory_stream->priv;

  count = MIN (count, priv->len - priv->pos);
  priv->pos += count;

  return count;
 

}

static gboolean
g_memory_input_stream_close (GInputStream *stream,
                             GCancellable *cancellable,
                             GError      **error)
{
  return TRUE;
}

static void
g_memory_input_stream_read_async (GInputStream              *stream,
                                  void                      *buffer,
                                  gsize                      count,
                                  int                        io_priority,
                                  GCancellable              *cancellable,
                                  GAsyncReadyCallback        callback,
                                  gpointer                   user_data)
{
  GSimpleAsyncResult *simple;
  gssize nread;

  nread =  g_memory_input_stream_read (stream,	buffer, count, cancellable, NULL);
  simple = g_simple_async_result_new (G_OBJECT (stream),
				      callback,
				      user_data,
				      g_memory_input_stream_read_async);
  g_simple_async_result_set_op_res_gssize (simple, nread);
  g_simple_async_result_complete_in_idle (simple);
  g_object_unref (simple);
}

static gssize
g_memory_input_stream_read_finish (GInputStream              *stream,
				   GAsyncResult              *result,
				   GError                   **error)
{
  GSimpleAsyncResult *simple;
  gssize nread;

  simple = G_SIMPLE_ASYNC_RESULT (result);
  g_assert (g_simple_async_result_get_source_tag (simple) == g_memory_input_stream_read_async);
  
  nread = g_simple_async_result_get_op_res_gssize (simple);
  return nread;
}

static void
g_memory_input_stream_skip_async (GInputStream              *stream,
                                  gsize                      count,
                                  int                        io_priority,
                                  GCancellable              *cancellable,
                                  GAsyncReadyCallback        callback,
                                  gpointer                   user_data)
{
  GSimpleAsyncResult *simple;
  gssize nskipped;

  nskipped = g_memory_input_stream_skip (stream, count, cancellable, NULL);
  simple = g_simple_async_result_new (G_OBJECT (stream),
				      callback,
				      user_data,
				      g_memory_input_stream_skip_async);
  g_simple_async_result_set_op_res_gssize (simple, nskipped);
  g_simple_async_result_complete_in_idle (simple);
  g_object_unref (simple);
}

static gssize
g_memory_input_stream_skip_finish (GInputStream              *stream,
				   GAsyncResult              *result,
				   GError                   **error)
{
  GSimpleAsyncResult *simple;
  gssize nskipped;

  simple = G_SIMPLE_ASYNC_RESULT (result);
  g_assert (g_simple_async_result_get_source_tag (simple) == g_memory_input_stream_skip_async);
  
  nskipped = g_simple_async_result_get_op_res_gssize (simple);
  return nskipped;
}

static void
g_memory_input_stream_close_async (GInputStream              *stream,
                                   int                        io_priority,
                                   GCancellable              *cancellable,
                                   GAsyncReadyCallback        callback,
                                   gpointer                   user_data)
{
  GSimpleAsyncResult *simple;
  
  simple = g_simple_async_result_new (G_OBJECT (stream),
				      callback,
				      user_data,
				      g_memory_input_stream_close_async);
  g_simple_async_result_complete_in_idle (simple);
  g_object_unref (simple);
}

static gboolean
g_memory_input_stream_close_finish (GInputStream              *stream,
				    GAsyncResult              *result,
				    GError                   **error)
{
  return TRUE;
}

static goffset
g_memory_input_stream_tell (GSeekable *seekable)
{
  GMemoryInputStream *memory_stream;
  GMemoryInputStreamPrivate * priv;

  memory_stream = G_MEMORY_INPUT_STREAM (seekable);
  priv = memory_stream->priv;

  return priv->pos;
}

static
gboolean g_memory_input_stream_can_seek (GSeekable *seekable)
{
  return TRUE;
}

static gboolean
g_memory_input_stream_seek (GSeekable       *seekable,
                            goffset          offset,
                            GSeekType        type,
                            GCancellable    *cancellable,
                            GError         **error)
{
  GMemoryInputStream *memory_stream;
  GMemoryInputStreamPrivate * priv;
  goffset absolute;

  memory_stream = G_MEMORY_INPUT_STREAM (seekable);
  priv = memory_stream->priv;

  switch (type) {

    case G_SEEK_CUR:
      absolute = priv->pos + offset;
      break;

    case G_SEEK_SET:
      absolute = offset;
      break;

    case G_SEEK_END:
      absolute = priv->len + offset;
      break;
  
    default:
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_ARGUMENT,
                   "Invalid GSeekType supplied");

      return FALSE;
  }

  if (absolute < 0 || absolute > priv->len)
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_INVALID_ARGUMENT,
                   "Invalid seek request");
      return FALSE;
    }

  priv->pos = absolute;

  return TRUE;
}

static gboolean
g_memory_input_stream_can_truncate (GSeekable *seekable)
{
  return FALSE;
}

static gboolean
g_memory_input_stream_truncate (GSeekable      *seekable,
                                goffset          offset,
                                GCancellable    *cancellable,
                                GError         **error)
{
  g_set_error (error,
               G_IO_ERROR,
               G_IO_ERROR_NOT_SUPPORTED,
               "Cannot seek on GMemoryInputStream");
  return FALSE;
}

// vim: ts=2 sw=2 et

