#include <config.h>
#include <glib/gi18n-lib.h>


#include "gbufferedoutputstream.h"
#include "goutputstream.h"
#include "gsimpleasyncresult.h"
#include "string.h"

/* TODO: Real P_() */
#define P_(_x) (_x)

#define DEFAULT_BUFFER_SIZE 4096

struct _GBufferedOutputStreamPrivate {
  guint8 *buffer; 
  gsize   len;
  goffset pos;
};

enum {
  PROP_0,
  PROP_BUFSIZE
};

static void     g_buffered_output_stream_set_property (GObject      *object,
                                                       guint         prop_id,
                                                       const GValue *value,
                                                       GParamSpec   *pspec);

static void     g_buffered_output_stream_get_property (GObject    *object,
                                                       guint       prop_id,
                                                       GValue     *value,
                                                       GParamSpec *pspec);
static void     g_buffered_output_stream_finalize     (GObject *object);


static gssize   g_buffered_output_stream_write        (GOutputStream *stream,
                                                       void          *buffer,
                                                       gsize          count,
                                                       GCancellable  *cancellable,
                                                       GError       **error);
static gboolean g_buffered_output_stream_flush        (GOutputStream    *stream,
                                                       GCancellable  *cancellable,
                                                       GError          **error);
static gboolean g_buffered_output_stream_close        (GOutputStream  *stream,
                                                       GCancellable   *cancellable,
                                                       GError        **error);

static void     g_buffered_output_stream_write_async  (GOutputStream        *stream,
                                                       void                 *buffer,
                                                       gsize                 count,
                                                       int                   io_priority,
                                                       GCancellable         *cancellable,
                                                       GAsyncReadyCallback   callback,
                                                       gpointer              data);
static gssize   g_buffered_output_stream_write_finish (GOutputStream        *stream,
                                                       GAsyncResult         *result,
                                                       GError              **error);
static void     g_buffered_output_stream_flush_async  (GOutputStream        *stream,
                                                       int                   io_priority,
                                                       GCancellable         *cancellable,
                                                       GAsyncReadyCallback   callback,
                                                       gpointer              data);
static gboolean g_buffered_output_stream_flush_finish (GOutputStream        *stream,
                                                       GAsyncResult         *result,
                                                       GError              **error);
static void     g_buffered_output_stream_close_async  (GOutputStream        *stream,
                                                       int                   io_priority,
                                                       GCancellable         *cancellable,
                                                       GAsyncReadyCallback   callback,
                                                       gpointer              data);
static gboolean g_buffered_output_stream_close_finish (GOutputStream        *stream,
                                                       GAsyncResult         *result,
                                                       GError              **error);

G_DEFINE_TYPE (GBufferedOutputStream,
               g_buffered_output_stream,
               G_TYPE_FILTER_OUTPUT_STREAM)


static void
g_buffered_output_stream_class_init (GBufferedOutputStreamClass *klass)
{
  GObjectClass *object_class;
  GOutputStreamClass *ostream_class;
 
  g_type_class_add_private (klass, sizeof (GBufferedOutputStreamPrivate));

  object_class = G_OBJECT_CLASS (klass);
  object_class->get_property = g_buffered_output_stream_get_property;
  object_class->set_property = g_buffered_output_stream_set_property;
  object_class->finalize     = g_buffered_output_stream_finalize;

  ostream_class = G_OUTPUT_STREAM_CLASS (klass);
  ostream_class->write = g_buffered_output_stream_write;
  ostream_class->flush = g_buffered_output_stream_flush;
  ostream_class->close = g_buffered_output_stream_close;
  ostream_class->write_async  = g_buffered_output_stream_write_async;
  ostream_class->write_finish = g_buffered_output_stream_write_finish;
  ostream_class->flush_async  = g_buffered_output_stream_flush_async;
  ostream_class->flush_finish = g_buffered_output_stream_flush_finish;
  ostream_class->close_async  = g_buffered_output_stream_close_async;
  ostream_class->close_finish = g_buffered_output_stream_close_finish;

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
g_buffered_output_stream_set_property (GObject         *object,
                                       guint            prop_id,
                                       const GValue    *value,
                                       GParamSpec      *pspec)
{
  GBufferedOutputStream *buffered_stream;
  GBufferedOutputStreamPrivate *priv;
  guint size;

  buffered_stream = G_BUFFERED_OUTPUT_STREAM (object);
  priv = buffered_stream->priv;

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
g_buffered_output_stream_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  GBufferedOutputStream *buffered_stream;
  GBufferedOutputStreamPrivate *priv;

  buffered_stream = G_BUFFERED_OUTPUT_STREAM (object);
  priv = buffered_stream->priv;

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
g_buffered_output_stream_finalize (GObject *object)
{
  GBufferedOutputStream *stream;
  GBufferedOutputStreamPrivate *priv;

  stream = G_BUFFERED_OUTPUT_STREAM (object);
  priv = stream->priv;

  g_free (priv->buffer);

  if (G_OBJECT_CLASS (g_buffered_output_stream_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_buffered_output_stream_parent_class)->finalize) (object);
}

static void
g_buffered_output_stream_init (GBufferedOutputStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
                                              G_TYPE_BUFFERED_OUTPUT_STREAM,
                                              GBufferedOutputStreamPrivate);

}

GOutputStream *
g_buffered_output_stream_new (GOutputStream *base_stream)
{
  GOutputStream *stream;

  g_assert (base_stream != NULL);

  stream = g_object_new (G_TYPE_BUFFERED_OUTPUT_STREAM,
                         "base-stream", base_stream,
                         NULL);

  return stream;
}

GOutputStream *
g_buffered_output_stream_new_sized (GOutputStream *base_stream,
                                    guint          size)
{
  GOutputStream *stream;

  g_assert (base_stream != NULL);

  stream = g_object_new (G_TYPE_BUFFERED_OUTPUT_STREAM,
                         "base-stream", base_stream,
                         "buffer-size", size,
                         NULL);

  return stream;
}

static gboolean
flush_buffer (GBufferedOutputStream  *stream,
              GCancellable           *cancellable,
              GError                 **error)
{
  GBufferedOutputStreamPrivate *priv;
  GOutputStream                *base_stream;
  gboolean                      res;
  gsize                         bytes_written;
  gsize                         count;

  priv = stream->priv;
  bytes_written = 0;
  base_stream = G_FILTER_OUTPUT_STREAM (stream)->base_stream;

  g_return_val_if_fail (G_IS_OUTPUT_STREAM (base_stream), FALSE);

  res = g_output_stream_write_all (base_stream,
                                   priv->buffer,
                                   priv->pos,
                                   &bytes_written,
                                   cancellable,
                                   error);

  count = priv->pos - bytes_written;

  if (count > 0)
    g_memmove (priv->buffer, priv->buffer + bytes_written, count);
  
  priv->pos -= bytes_written;

  return res;
}

static gssize
g_buffered_output_stream_write  (GOutputStream *stream,
                                 void          *buffer,
                                 gsize          count,
                                 GCancellable  *cancellable,
                                 GError       **error)
{
  GBufferedOutputStream        *bstream;
  GBufferedOutputStreamPrivate *priv;
  gboolean res;
  gsize    n;

  bstream = G_BUFFERED_OUTPUT_STREAM (stream);
  priv = bstream->priv;

  n = priv->len - priv->pos;

  if (n == 0)
    {
      res = flush_buffer (bstream, cancellable, error);

      if (res == FALSE)
        return -1;
 

      n = priv->len - priv->pos; 
    }

  count = MIN (count, n);
  memcpy (priv->buffer + priv->pos, buffer, count);
  priv->pos += count;

  return count;
}

static gboolean
g_buffered_output_stream_flush (GOutputStream  *stream,
                                GCancellable   *cancellable,
                                GError        **error)
{
  GBufferedOutputStream *bstream;
  GBufferedOutputStreamPrivate *priv;
  GOutputStream                *base_stream;
  gboolean res;

  bstream = G_BUFFERED_OUTPUT_STREAM (stream);
  priv = bstream->priv;
  base_stream = G_FILTER_OUTPUT_STREAM (stream)->base_stream;

  res = flush_buffer (bstream, cancellable, error);

  if (res == FALSE) {
    return FALSE;
  }

  res = g_output_stream_flush (base_stream,
                               cancellable,
                               error);
  return res;
}

static gboolean
g_buffered_output_stream_close (GOutputStream  *stream,
                                GCancellable   *cancellable,
                                GError        **error)
{
  GBufferedOutputStream        *bstream;
  GBufferedOutputStreamPrivate *priv;
  GOutputStream                *base_stream;
  gboolean                      res;

  bstream = G_BUFFERED_OUTPUT_STREAM (stream);
  priv = bstream->priv;
  base_stream = G_FILTER_OUTPUT_STREAM (bstream)->base_stream;

  res = flush_buffer (bstream, cancellable, error);

  /* report the first error but still close the stream */
  if (res)
    {
      res = g_output_stream_close (base_stream,
                                   cancellable,
                                   error); 
    }
  else
    {
      g_output_stream_close (base_stream,
                             cancellable,
                             NULL); 
    }

  return res;
}

/* ************************** */
/* Async stuff implementation */
/* ************************** */
typedef struct {

  guint flush_stream : 1;
  guint close_stream : 1;

} FlushData;

static void
free_flush_data (gpointer data)
{
  g_slice_free (FlushData, data);
}

/* This function is used by all three (i.e. 
 * _write, _flush, _close) functions since
 * all of them will need to flush the buffer
 * and so closing and writing is just a special
 * case of flushing + some addition stuff */
static void
flush_buffer_thread (GSimpleAsyncResult *result,
                     GObject            *object,
                     GCancellable       *cancellable)
{
  GBufferedOutputStream *stream;
  GOutputStream *base_stream;
  FlushData     *fdata;
  gboolean       res;
  GError        *error = NULL;

  stream = G_BUFFERED_OUTPUT_STREAM (object);
  fdata = g_simple_async_result_get_op_res_gpointer (result);
  base_stream = G_FILTER_OUTPUT_STREAM (stream)->base_stream;

  res = flush_buffer (stream, cancellable, &error);

  /* if flushing the buffer didn't work don't even bother
   * to flush the stream but just report that error */
  if (res && fdata->flush_stream)
    {
      res = g_output_stream_flush (base_stream,
                                   cancellable,
                                   &error);
    }

  if (fdata->close_stream) 
    {
     
      /* if flushing the buffer or the stream returned 
       * an error report that first error but still try 
       * close the stream */
      if (res == FALSE)
        {
          g_output_stream_close (base_stream,
                                 cancellable,
                                 NULL);
        } 
      else 
        {
          res = g_output_stream_close (base_stream,
                                       cancellable,
                                       &error);
        } 

    }

  if (res == FALSE)
    {
      g_simple_async_result_set_from_error (result, error);
      g_error_free (error);
    }
}

typedef struct {
    
  FlushData fdata;

  gsize  count;
  void  *buffer;

} WriteData;

static void 
free_write_data (gpointer data)
{
  g_slice_free (WriteData, data);
}

static void
g_buffered_output_stream_write_async (GOutputStream        *stream,
                                      void                 *buffer,
                                      gsize                 count,
                                      int                   io_priority,
                                      GCancellable         *cancellable,
                                      GAsyncReadyCallback   callback,
                                      gpointer              data)
{
  GBufferedOutputStream *buffered_stream;
  GBufferedOutputStreamPrivate *priv;
  GSimpleAsyncResult *res;
  WriteData *wdata;

  buffered_stream = G_BUFFERED_OUTPUT_STREAM (stream);
  priv = buffered_stream->priv;

  wdata = g_slice_new (WriteData);
  wdata->count  = count;
  wdata->buffer = buffer;

  res = g_simple_async_result_new (G_OBJECT (stream),
                                   callback,
                                   data,
                                   g_buffered_output_stream_write_async);

  g_simple_async_result_set_op_res_gpointer (res, wdata, free_write_data);

  /* if we have space left directly call the
   * callback (from idle) otherwise schedule a buffer 
   * flush in the thread. In both cases the actual
   * copying of the data to the buffer will be done in
   * the write_finish () func since that should
   * be fast enough */
  if (priv->len - priv->pos > 0)
    {
      g_simple_async_result_complete_in_idle (res);
    }
  else
    {
      wdata->fdata.flush_stream = FALSE;
      wdata->fdata.close_stream = FALSE;
      g_simple_async_result_run_in_thread (res, 
                                           flush_buffer_thread, 
                                           io_priority,
                                           cancellable);
      g_object_unref (res);
    }
}

static gssize
g_buffered_output_stream_write_finish (GOutputStream        *stream,
                                       GAsyncResult         *result,
                                       GError              **error)
{
  GBufferedOutputStreamPrivate *priv;
  GBufferedOutputStream        *buffered_stream;
  GSimpleAsyncResult *simple;
  WriteData          *wdata;
  gssize              count;

  simple = G_SIMPLE_ASYNC_RESULT (result);
  buffered_stream = G_BUFFERED_OUTPUT_STREAM (stream);
  priv = buffered_stream->priv;

  g_assert (g_simple_async_result_get_source_tag (simple) == 
            g_buffered_output_stream_write_async);

  wdata = g_simple_async_result_get_op_res_gpointer (simple);

  /* Now do the real copying of data to the buffer */
  count = priv->len - priv->pos; 
  count = MIN (wdata->count, count);

  memcpy (priv->buffer + priv->pos, wdata->buffer, count);
  
  priv->pos += count;

  return count;
}

static void
g_buffered_output_stream_flush_async (GOutputStream        *stream,
                                      int                   io_priority,
                                      GCancellable         *cancellable,
                                      GAsyncReadyCallback   callback,
                                      gpointer              data)
{
  GSimpleAsyncResult *res;
  FlushData          *fdata;

  fdata = g_slice_new (FlushData);
  fdata->flush_stream = TRUE;
  fdata->close_stream = FALSE;

  res = g_simple_async_result_new (G_OBJECT (stream),
                                   callback,
                                   data,
                                   g_buffered_output_stream_flush_async);

  g_simple_async_result_set_op_res_gpointer (res, fdata, free_flush_data);

  g_simple_async_result_run_in_thread (res, 
                                       flush_buffer_thread, 
                                       io_priority,
                                       cancellable);
  g_object_unref (res);
}

static gboolean
g_buffered_output_stream_flush_finish (GOutputStream        *stream,
                                       GAsyncResult         *result,
                                       GError              **error)
{
  GSimpleAsyncResult *simple;

  simple = G_SIMPLE_ASYNC_RESULT (result);

  g_assert (g_simple_async_result_get_source_tag (simple) == 
            g_buffered_output_stream_flush_async);

  return TRUE;
}

static void
g_buffered_output_stream_close_async (GOutputStream        *stream,
                                      int                   io_priority,
                                      GCancellable         *cancellable,
                                      GAsyncReadyCallback   callback,
                                      gpointer              data)
{
  GSimpleAsyncResult *res;
  FlushData          *fdata;

  fdata = g_slice_new (FlushData);
  fdata->close_stream = TRUE;

  res = g_simple_async_result_new (G_OBJECT (stream),
                                   callback,
                                   data,
                                   g_buffered_output_stream_close_async);

  g_simple_async_result_set_op_res_gpointer (res, fdata, free_flush_data);

  g_simple_async_result_run_in_thread (res, 
                                       flush_buffer_thread, 
                                       io_priority,
                                       cancellable);
  g_object_unref (res);
}

static gboolean
g_buffered_output_stream_close_finish (GOutputStream        *stream,
                                       GAsyncResult         *result,
                                       GError              **error)
{
  GSimpleAsyncResult *simple;

  simple = G_SIMPLE_ASYNC_RESULT (result);

  g_assert (g_simple_async_result_get_source_tag (simple) == 
            g_buffered_output_stream_flush_async);

  return TRUE;
}

/* vim: ts=2 sw=2 et */
