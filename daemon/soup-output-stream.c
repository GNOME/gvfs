/* soup-output-stream.c, based on gunixoutputstream.c
 *
 * Copyright (C) 2006-2008 Red Hat, Inc.
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
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <config.h>

#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include <libsoup/soup.h>

#include "soup-output-stream.h"
#include "soup-input-stream.h"

G_DEFINE_TYPE (SoupOutputStream, soup_output_stream, G_TYPE_OUTPUT_STREAM)

typedef void (*SoupOutputStreamCallback) (GOutputStream *);

typedef struct {
  SoupSession *session;
  GMainContext *async_context;
  SoupMessage *msg;
  gboolean finished;

  goffset size, offset;
  GByteArray *ba;

  GCancellable *cancellable;
  GSource *cancel_watch;
  SoupOutputStreamCallback finished_cb;
  SoupOutputStreamCallback cancelled_cb;

  GSimpleAsyncResult *result;
} SoupOutputStreamPrivate;
#define SOUP_OUTPUT_STREAM_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), SOUP_TYPE_OUTPUT_STREAM, SoupOutputStreamPrivate))

static gssize   soup_output_stream_write        (GOutputStream        *stream,
						 const void           *buffer,
						 gsize                 count,
						 GCancellable         *cancellable,
						 GError              **error);
static gboolean soup_output_stream_close        (GOutputStream        *stream,
						 GCancellable         *cancellable,
						 GError              **error);
static void     soup_output_stream_write_async  (GOutputStream        *stream,
						 const void           *buffer,
						 gsize                 count,
						 int                   io_priority,
						 GCancellable         *cancellable,
						 GAsyncReadyCallback   callback,
						 gpointer              data);
static gssize   soup_output_stream_write_finish (GOutputStream        *stream,
						 GAsyncResult         *result,
						 GError              **error);
static void     soup_output_stream_close_async  (GOutputStream        *stream,
						 int                   io_priority,
						 GCancellable         *cancellable,
						 GAsyncReadyCallback   callback,
						 gpointer              data);
static gboolean soup_output_stream_close_finish (GOutputStream        *stream,
						 GAsyncResult         *result,
						 GError              **error);

static void soup_output_stream_finished (SoupMessage *msg, gpointer stream);

static void
soup_output_stream_finalize (GObject *object)
{
  SoupOutputStreamPrivate *priv = SOUP_OUTPUT_STREAM_GET_PRIVATE (object);

  g_object_unref (priv->session);

  g_signal_handlers_disconnect_by_func (priv->msg, G_CALLBACK (soup_output_stream_finished), object);
  g_object_unref (priv->msg);

  if (priv->ba)
    g_byte_array_free (priv->ba, TRUE);

  if (G_OBJECT_CLASS (soup_output_stream_parent_class)->finalize)
    (*G_OBJECT_CLASS (soup_output_stream_parent_class)->finalize) (object);
}

static void
soup_output_stream_class_init (SoupOutputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GOutputStreamClass *stream_class = G_OUTPUT_STREAM_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (SoupOutputStreamPrivate));
  
  gobject_class->finalize = soup_output_stream_finalize;

  stream_class->write_fn = soup_output_stream_write;
  stream_class->close_fn = soup_output_stream_close;
  stream_class->write_async = soup_output_stream_write_async;
  stream_class->write_finish = soup_output_stream_write_finish;
  stream_class->close_async = soup_output_stream_close_async;
  stream_class->close_finish = soup_output_stream_close_finish;
}

static void
soup_output_stream_init (SoupOutputStream *stream)
{
  SoupOutputStreamPrivate *priv = SOUP_OUTPUT_STREAM_GET_PRIVATE (stream);

  priv->ba = g_byte_array_new ();
}


/**
 * soup_output_stream_new:
 * @session: the #SoupSession to use
 * @msg: the #SoupMessage whose request will be streamed
 * @size: the total size of the request body, or -1 if not known
 * 
 * Prepares to send @msg over @session, and returns a #GOutputStream
 * that can be used to write the response. The server's response will
 * be available in @msg after calling soup_output_stream_close()
 * (which will return a %SOUP_OUTPUT_STREAM_HTTP_ERROR #GError if the
 * status is not 2xx).
 *
 * If you know the total number of bytes that will be written, pass
 * that in @size. Otherwise, pass -1. (If you pass a size, you MUST
 * write that many bytes to the stream; Trying to write more than
 * that, or closing the stream without having written enough, will
 * result in an error.
 *
 * In some situations, the request will not actually be sent until you
 * call g_output_stream_close(). (In fact, currently this is *always*
 * true.)
 *
 * Internally, #SoupOutputStream is implemented using asynchronous
 * I/O, so if you are using the synchronous API (eg,
 * g_output_stream_write()), you should create a new #GMainContext and
 * set it as the %SOUP_SESSION_ASYNC_CONTEXT property on @session. (If
 * you don't, then synchronous #GOutputStream calls will cause the
 * main loop to be run recursively.) The async #GOutputStream API
 * works fine with %SOUP_SESSION_ASYNC_CONTEXT either set or unset.
 *
 * Returns: a new #GOutputStream.
 **/
GOutputStream *
soup_output_stream_new (SoupSession *session, SoupMessage *msg, goffset size)
{
  SoupOutputStream *stream;
  SoupOutputStreamPrivate *priv;

  g_return_val_if_fail (SOUP_IS_MESSAGE (msg), NULL);

  stream = g_object_new (SOUP_TYPE_OUTPUT_STREAM, NULL);
  priv = SOUP_OUTPUT_STREAM_GET_PRIVATE (stream);

  priv->session = g_object_ref (session);
  priv->async_context = soup_session_get_async_context (session);
  priv->msg = g_object_ref (msg);
  priv->size = size;

  return G_OUTPUT_STREAM (stream);
}

static gboolean
soup_output_stream_cancelled (GIOChannel *chan, GIOCondition condition,
			      gpointer stream)
{
  SoupOutputStreamPrivate *priv = SOUP_OUTPUT_STREAM_GET_PRIVATE (stream);

  priv->cancel_watch = NULL;

  soup_session_pause_message (priv->session, priv->msg);
  if (priv->cancelled_cb)
    priv->cancelled_cb (stream);

  return FALSE;
}  

static void
soup_output_stream_prepare_for_io (GOutputStream *stream, GCancellable *cancellable)
{
  SoupOutputStreamPrivate *priv = SOUP_OUTPUT_STREAM_GET_PRIVATE (stream);
  int cancel_fd;

  /* Move the buffer to the SoupMessage */
  soup_message_body_append (priv->msg->request_body, SOUP_MEMORY_TAKE,
			    priv->ba->data, priv->ba->len);
  g_byte_array_free (priv->ba, FALSE);
  priv->ba = NULL;

  /* Set up cancellation */
  priv->cancellable = cancellable;
  cancel_fd = g_cancellable_get_fd (cancellable);
  if (cancel_fd != -1)
    {
      GIOChannel *chan = g_io_channel_unix_new (cancel_fd);
      priv->cancel_watch = soup_add_io_watch (priv->async_context, chan,
					      G_IO_IN | G_IO_ERR | G_IO_HUP,
					      soup_output_stream_cancelled,
					      stream);
      g_io_channel_unref (chan);
    }

  /* Add an extra ref since soup_session_queue_message steals one */
  g_object_ref (priv->msg);
  soup_session_queue_message (priv->session, priv->msg, NULL, NULL);
}

static void
soup_output_stream_done_io (GOutputStream *stream)
{
  SoupOutputStreamPrivate *priv = SOUP_OUTPUT_STREAM_GET_PRIVATE (stream);

  if (priv->cancel_watch)
    {
      g_source_destroy (priv->cancel_watch);
      priv->cancel_watch = NULL;
      g_cancellable_release_fd (priv->cancellable);
    }
  priv->cancellable = NULL;
}

static gboolean
set_error_if_http_failed (SoupMessage *msg, GError **error)
{
  if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    {
      g_set_error_literal (error, SOUP_HTTP_ERROR,
			   msg->status_code, msg->reason_phrase);
      return TRUE;
    }
  return FALSE;
}

static gssize
soup_output_stream_write (GOutputStream  *stream,
			  const void     *buffer,
			  gsize           count,
			  GCancellable   *cancellable,
			  GError        **error)
{
  SoupOutputStreamPrivate *priv = SOUP_OUTPUT_STREAM_GET_PRIVATE (stream);

  if (priv->size > 0 && priv->offset + count > priv->size) {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
			   "Write would exceed caller-defined file size");
      return -1;
  }

  g_byte_array_append (priv->ba, buffer, count);
  priv->offset += count;
  return count;
}

static int
soup_output_stream_close (GOutputStream  *stream,
			  GCancellable   *cancellable,
			  GError        **error)
{
  SoupOutputStreamPrivate *priv = SOUP_OUTPUT_STREAM_GET_PRIVATE (stream);

  if (priv->size > 0 && priv->offset != priv->size) {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
			   "File is incomplete");
      return -1;
  }

  soup_output_stream_prepare_for_io (stream, cancellable);
  while (!priv->finished && !g_cancellable_is_cancelled (cancellable))
    g_main_context_iteration (priv->async_context, TRUE);
  soup_output_stream_done_io (stream);

  return !set_error_if_http_failed (priv->msg, error);
}

static void
soup_output_stream_write_async (GOutputStream       *stream,
				const void          *buffer,
				gsize                count,
				int                  io_priority,
				GCancellable        *cancellable,
				GAsyncReadyCallback  callback,
				gpointer             user_data)
{
  SoupOutputStreamPrivate *priv = SOUP_OUTPUT_STREAM_GET_PRIVATE (stream);
  GSimpleAsyncResult *result;

  result = g_simple_async_result_new (G_OBJECT (stream),
				      callback, user_data,
				      soup_output_stream_write_async);

  if (priv->size > 0 && priv->offset + count > priv->size)
    {
      GError *error;

      error = g_error_new (G_IO_ERROR, G_IO_ERROR_NO_SPACE,
			   "Write would exceed caller-defined file size");
      g_simple_async_result_set_from_error (result, error);
      g_error_free (error);
    }
  else
    {
      g_byte_array_append (priv->ba, buffer, count);
      priv->offset += count;
      g_simple_async_result_set_op_res_gssize (result, count);
    }

  g_simple_async_result_complete_in_idle (result);
}

static gssize
soup_output_stream_write_finish (GOutputStream  *stream,
				 GAsyncResult   *result,
				 GError        **error)
{
  GSimpleAsyncResult *simple;
  gssize nwritten;

  simple = G_SIMPLE_ASYNC_RESULT (result);
  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == soup_output_stream_write_async);
  
  nwritten = g_simple_async_result_get_op_res_gssize (simple);
  return nwritten;
}

static void
close_async_done (GOutputStream *stream)
{
  SoupOutputStreamPrivate *priv = SOUP_OUTPUT_STREAM_GET_PRIVATE (stream);
  GSimpleAsyncResult *result;
  GError *error = NULL;

  result = priv->result;
  priv->result = NULL;

  if (g_cancellable_set_error_if_cancelled (priv->cancellable, &error) ||
      set_error_if_http_failed (priv->msg, &error))
    {
      g_simple_async_result_set_from_error (result, error);
      g_error_free (error);
    }
  else
    g_simple_async_result_set_op_res_gboolean (result, TRUE);

  priv->finished_cb = NULL;
  priv->cancelled_cb = NULL;
  soup_output_stream_done_io (stream);

  g_simple_async_result_complete (result);
}

static void
soup_output_stream_finished (SoupMessage *msg, gpointer stream)
{
  SoupOutputStreamPrivate *priv = SOUP_OUTPUT_STREAM_GET_PRIVATE (stream);

  priv->finished = TRUE;

  g_signal_handlers_disconnect_by_func (priv->msg, G_CALLBACK (soup_output_stream_finished), stream);
  close_async_done (stream);
}

static void
soup_output_stream_close_async (GOutputStream        *stream,
				int                  io_priority,
				GCancellable        *cancellable,
				GAsyncReadyCallback  callback,
				gpointer             user_data)
{
  SoupOutputStreamPrivate *priv = SOUP_OUTPUT_STREAM_GET_PRIVATE (stream);
  GSimpleAsyncResult *result;

  result = g_simple_async_result_new (G_OBJECT (stream),
				      callback, user_data,
				      soup_output_stream_close_async);

  if (priv->size > 0 && priv->offset != priv->size)
    {
      GError *error;

      error = g_error_new (G_IO_ERROR, G_IO_ERROR_NO_SPACE,
			   "File is incomplete");
      g_simple_async_result_set_from_error (result, error);
      g_error_free (error);
      g_simple_async_result_complete_in_idle (result);
      return;
    }

  priv->result = result;
  priv->cancelled_cb = close_async_done;
  g_signal_connect (priv->msg, "finished",
		    G_CALLBACK (soup_output_stream_finished), stream);
  soup_output_stream_prepare_for_io (stream, cancellable);
}

static gboolean
soup_output_stream_close_finish (GOutputStream  *stream,
				 GAsyncResult   *result,
				 GError        **error)
{
  /* Failures handled in generic close_finish code */
  return TRUE;
}
