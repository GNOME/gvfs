#include <config.h>
#include <glib/gi18n-lib.h>


#include "gbufferedinputstream.h"
#include "ginputstream.h"
#include "gsimpleasyncresult.h"
#include <string.h>

/* TODO: Real P_() */
#define P_(_x) (_x)

#define DEFAULT_BUFFER_SIZE 4096

struct _GBufferedInputStreamPrivate {

  guint8 *buffer;
  guint   len;
  guint   pos;
  guint   end;

};

enum {
  PROP_0,
  PROP_BUFSIZE
};

static void g_buffered_input_stream_set_property  (GObject      *object,
                                                   guint         prop_id,
                                                   const GValue *value,
                                                   GParamSpec   *pspec);

static void g_buffered_input_stream_get_property  (GObject      *object,
                                                   guint         prop_id,
                                                   GValue       *value,
                                                   GParamSpec   *pspec);
static void g_buffered_input_stream_finalize      (GObject *object);


static gssize   g_buffered_input_stream_read      (GInputStream         *stream,
                                                   void                 *buffer,
                                                   gsize                 count,
                                                   GCancellable         *cancellable,
                                                   GError              **error);
static void g_buffered_input_stream_read_async    (GInputStream         *stream,
                                                   void                 *buffer,
                                                   gsize                 count,
                                                   int                   io_priority,
                                                   GCancellable         *cancellable,
                                                   GAsyncReadyCallback   callback,
                                                   gpointer              user_data);
static gssize g_buffered_input_stream_read_finish (GInputStream   *stream,
                                                   GAsyncResult   *result,
                                                   GError        **error);

G_DEFINE_TYPE (GBufferedInputStream,
               g_buffered_input_stream,
               G_TYPE_FILTER_INPUT_STREAM)


static void
g_buffered_input_stream_class_init (GBufferedInputStreamClass *klass)
{
  GObjectClass *object_class;
  GInputStreamClass *istream_class;

  g_type_class_add_private (klass, sizeof (GBufferedInputStreamPrivate));

  object_class = G_OBJECT_CLASS (klass);
  object_class->get_property = g_buffered_input_stream_get_property;
  object_class->set_property = g_buffered_input_stream_set_property;
  object_class->finalize     = g_buffered_input_stream_finalize;

  istream_class = G_INPUT_STREAM_CLASS (klass);
  istream_class->read = g_buffered_input_stream_read;
  istream_class->read_async  = g_buffered_input_stream_read_async;
  istream_class->read_finish = g_buffered_input_stream_read_finish;

  g_object_class_install_property (object_class,
                                   PROP_BUFSIZE,
                                   g_param_spec_uint ("buffer-size",
                                                      P_("Buffer Size"),
                                                      P_("The size of the backend buffer"),
                                                      1,
                                                      G_MAXUINT,
                                                      DEFAULT_BUFFER_SIZE,
                                                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                                                      G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));


}

static void
g_buffered_input_stream_set_property (GObject         *object,
                                      guint            prop_id,
                                      const GValue    *value,
                                      GParamSpec      *pspec)
{
  GBufferedInputStreamPrivate *priv;
  GBufferedInputStream        *bstream;
  gsize                        size;

  bstream = G_BUFFERED_INPUT_STREAM (object);
  priv = bstream->priv;

  switch (prop_id) 
    {
    case PROP_BUFSIZE:
      size = g_value_get_uint (value);
      priv->len = size;
      priv->pos = 0;
      priv->buffer = g_malloc (priv->len);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }

}

static void
g_buffered_input_stream_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  GBufferedInputStreamPrivate *priv;
  GBufferedInputStream        *bstream;

  bstream = G_BUFFERED_INPUT_STREAM (object);
  priv = bstream->priv;

  switch (prop_id)
    { 
    
    case PROP_BUFSIZE:
      g_value_set_uint (value, priv->len);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }

}

static void
g_buffered_input_stream_finalize (GObject *object)
{
  GBufferedInputStreamPrivate *priv;
  GBufferedInputStream        *stream;

  stream = G_BUFFERED_INPUT_STREAM (object);
  priv = stream->priv;

  g_free (priv->buffer);

  if (G_OBJECT_CLASS (g_buffered_input_stream_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_buffered_input_stream_parent_class)->finalize) (object);
}

static void
g_buffered_input_stream_init (GBufferedInputStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
                                              G_TYPE_BUFFERED_INPUT_STREAM,
                                              GBufferedInputStreamPrivate);
}


GInputStream *
g_buffered_input_stream_new (GInputStream *base_stream)
{
  GInputStream *stream;

  g_assert (base_stream != NULL);

  stream = g_object_new (G_TYPE_BUFFERED_INPUT_STREAM,
                         "base-stream", base_stream,
                         NULL);

  return stream;
}

GInputStream *
g_buffered_input_stream_new_sized (GInputStream *base_stream,
                                   guint         size)
{
  GInputStream *stream;

  g_assert (base_stream != NULL);

  stream = g_object_new (G_TYPE_BUFFERED_INPUT_STREAM,
                         "base-stream", base_stream,
                         "buffer-size", size,
                         NULL);

  return stream;
}

static gboolean
refill_buffer (GBufferedInputStream   *stream,
               GCancellable           *cancellable,
               GError                **error)
{
  GBufferedInputStreamPrivate *priv;
  GInputStream *base_stream;
  gboolean      res;
  gsize         len;
  gsize         nread;

  priv = stream->priv;
  res  = FALSE;

  if (priv->pos)
    {
      /* if we have data left move it to the front */
      len = priv->end - priv->pos;
      
      g_memmove (priv->buffer,
                 priv->buffer + priv->pos,
                 len);

      priv->pos = 0;
      priv->end = len;
    }

  len = priv->len - priv->end;

  base_stream = G_FILTER_INPUT_STREAM (stream)->base_stream;

  nread = g_input_stream_read (base_stream,
                               priv->buffer + priv->end,
                               len,
                               cancellable,
                               error);


  if (nread < 0) {
    return FALSE;
  }

  priv->end += nread;

  return TRUE;
}

static gssize
g_buffered_input_stream_read (GInputStream *stream,
                              void         *buffer,
                              gsize         count,
                              GCancellable *cancellable,
                              GError      **error)
{
  GBufferedInputStream        *bstream;
  GBufferedInputStreamPrivate *priv;
  gboolean                     res;
  gsize                        n;

  bstream = G_BUFFERED_INPUT_STREAM (stream);
  priv = bstream->priv;

  n = priv->end - priv->pos;

  if (n == 0)
    {
      res = refill_buffer (bstream, cancellable, error);
      
      if (res == FALSE)
        return -1;

      n =  priv->end - priv->pos;
    }

  count = MIN (count, n);

  memcpy (buffer, priv->buffer + priv->pos, count);
  priv->pos += count;

  return count;
}

/* ************************** */
/* Async stuff implementation */
/* ************************** */

typedef struct {
    
  gsize  count;
  void  *buffer;

} ReadData;

static void 
free_read_data (gpointer data)
{
  g_slice_free (ReadData, data);
}

static void 
fill_buffer_thread (GSimpleAsyncResult *result,
                    GObject            *object,
                    GCancellable       *cancellable)
{
  GBufferedInputStream *stream;
  GError        *error = NULL;
  gboolean res;

  stream = G_BUFFERED_INPUT_STREAM (object);

  res = refill_buffer (stream, cancellable, &error);

  if (res == FALSE)
    {
      g_simple_async_result_set_from_error (result, error);
      g_error_free (error);
    }
}

static void
g_buffered_input_stream_read_async (GInputStream              *stream,
                                    void                      *buffer,
                                    gsize                      count,
                                    int                        io_priority,
                                    GCancellable              *cancellable,
                                    GAsyncReadyCallback        callback,
                                    gpointer                   user_data)
{
  GBufferedInputStream        *bstream;
  GBufferedInputStreamPrivate *priv;
  GSimpleAsyncResult *res;
  ReadData           *rdata;

  bstream = G_BUFFERED_INPUT_STREAM (stream);
  priv = bstream->priv;

  rdata = g_slice_new (ReadData);
  rdata->count  = count;
  rdata->buffer = buffer;

  res = g_simple_async_result_new (G_OBJECT (stream),
                                   callback,
                                   rdata,
                                   g_buffered_input_stream_read_async);

  g_simple_async_result_set_op_res_gpointer (res, rdata, free_read_data);

  /* if we have data left in the buffer we direclty call
   * the callback in ilde and copy them to the buffer (should
   * be fast enough) otherwise we refill the buffer in a thread */
  if (priv->end - priv->pos > 0)
    {
      g_simple_async_result_complete_in_idle (res);
    }
  else
    {
      g_simple_async_result_run_in_thread (res, 
                                           fill_buffer_thread, 
                                           io_priority,
                                           cancellable);
      g_object_unref (res);
    }

}

static gssize
g_buffered_input_stream_read_finish (GInputStream   *stream,
                                     GAsyncResult   *result,
                                     GError        **error)
{
  GBufferedInputStream        *bstream;
  GBufferedInputStreamPrivate *priv;
  GSimpleAsyncResult *simple;
  ReadData           *rdata;
  gsize               count;


  bstream = G_BUFFERED_INPUT_STREAM (stream);
  priv = bstream->priv;

  simple = G_SIMPLE_ASYNC_RESULT (result);
  
  g_assert (g_simple_async_result_get_source_tag (simple)
            == g_buffered_input_stream_read_async);
 
  rdata = g_simple_async_result_get_op_res_gpointer (simple);

  count = MIN (rdata->count, priv->end - priv->pos);
  memcpy (rdata->buffer, priv->buffer + priv->pos, count);
  priv->pos += count;

  return count;
}

/* vim: ts=2 sw=2 et */
