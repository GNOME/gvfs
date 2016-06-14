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
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include "gdaemonfileoutputstream.h"
#include "gvfsdaemondbus.h"
#include <gvfsdaemonprotocol.h>
#include <gvfsfileinfo.h>

#define MAX_WRITE_SIZE (4*1024*1024)

typedef enum {
  STATE_OP_DONE,
  STATE_OP_READ,
  STATE_OP_WRITE,
  STATE_OP_SKIP
} StateOp;

typedef enum {
  WRITE_STATE_INIT = 0,
  WRITE_STATE_WROTE_COMMAND,
  WRITE_STATE_SEND_DATA,
  WRITE_STATE_HANDLE_INPUT
} WriteState;

typedef struct {
  WriteState state;

  /* Output */
  const char *buffer;
  gsize buffer_size;
  gsize buffer_pos;
  
  /* Input */
  gssize ret_val;
  GError *ret_error;
  
  gboolean sent_cancel;
  
  guint32 seq_nr;
} WriteOperation;

typedef enum {
  SEEK_STATE_INIT = 0,
  SEEK_STATE_WROTE_REQUEST,
  SEEK_STATE_HANDLE_INPUT
} SeekState;

typedef struct {
  SeekState state;

  /* Output */
  goffset offset;
  GSeekType seek_type;
  /* Output */
  gboolean ret_val;
  GError *ret_error;
  goffset ret_offset;
  
  gboolean sent_cancel;
  
  guint32 seq_nr;
} SeekOperation;

typedef enum {
  TRUNCATE_STATE_INIT = 0,
  TRUNCATE_STATE_WROTE_REQUEST,
  TRUNCATE_STATE_HANDLE_INPUT
} TruncateState;

typedef struct {
  TruncateState state;

  /* Output */
  goffset size;
  /* Input */
  gboolean ret_val;
  GError *ret_error;

  gboolean sent_cancel;

  guint32 seq_nr;
} TruncateOperation;

typedef enum {
  CLOSE_STATE_INIT = 0,
  CLOSE_STATE_WROTE_REQUEST,
  CLOSE_STATE_HANDLE_INPUT
} CloseState;

typedef struct {
  CloseState state;

  /* Output */
  
  /* Output */
  gboolean ret_val;
  GError *ret_error;
  
  gboolean sent_cancel;
  
  guint32 seq_nr;
} CloseOperation;

typedef enum {
  QUERY_STATE_INIT = 0,
  QUERY_STATE_WROTE_REQUEST,
  QUERY_STATE_HANDLE_INPUT,
} QueryState;

typedef struct {
  QueryState state;

  /* Input */
  char *attributes;
  
  /* Output */
  GFileInfo *info;
  GError *ret_error;

  gboolean sent_cancel;
  
  guint32 seq_nr;
} QueryOperation;

typedef struct {
  gboolean cancelled;
  
  char *io_buffer;
  gsize io_size;
  gsize io_res;
  /* The operation always succeeds, or gets cancelled.
     If we get an error doing the i/o that is considered fatal */
  gboolean io_allow_cancel;
  gboolean io_cancelled;
} IOOperationData;

typedef StateOp (*state_machine_iterator) (GDaemonFileOutputStream *file, IOOperationData *io_op, gpointer data);

struct _GDaemonFileOutputStream {
  GFileOutputStream parent;

  GOutputStream *command_stream;
  GInputStream *data_stream;
  gboolean can_seek;
  gboolean can_truncate;
  
  guint32 seq_nr;
  goffset current_offset;

  gsize input_block_size;
  GString *input_buffer;
  
  GString *output_buffer;

  char *etag;
  
};

static gssize     g_daemon_file_output_stream_write             (GOutputStream        *stream,
								 const void           *buffer,
								 gsize                 count,
								 GCancellable         *cancellable,
								 GError              **error);
static gboolean   g_daemon_file_output_stream_close             (GOutputStream        *stream,
								 GCancellable         *cancellable,
								 GError              **error);
static GFileInfo *g_daemon_file_output_stream_query_info        (GFileOutputStream    *stream,
								 const char           *attributes,
								 GCancellable         *cancellable,
								 GError              **error);
static char      *g_daemon_file_output_stream_get_etag          (GFileOutputStream    *stream);
static goffset    g_daemon_file_output_stream_tell              (GFileOutputStream    *stream);
static gboolean   g_daemon_file_output_stream_can_seek          (GFileOutputStream    *stream);
static gboolean   g_daemon_file_output_stream_seek              (GFileOutputStream    *stream,
								 goffset               offset,
								 GSeekType             type,
								 GCancellable         *cancellable,
								 GError              **error);
static gboolean   g_daemon_file_output_stream_can_truncate      (GFileOutputStream    *stream);
static gboolean   g_daemon_file_output_stream_truncate          (GFileOutputStream    *stream,
								 goffset               size,
								 GCancellable         *cancellable,
								 GError              **error);
static void       g_daemon_file_output_stream_write_async       (GOutputStream        *stream,
								 const void           *buffer,
								 gsize                 count,
								 int                   io_priority,
								 GCancellable         *cancellable,
								 GAsyncReadyCallback   callback,
								 gpointer              data);
static gssize     g_daemon_file_output_stream_write_finish      (GOutputStream        *stream,
								 GAsyncResult         *result,
								 GError              **error);
static void       g_daemon_file_output_stream_close_async       (GOutputStream        *stream,
								 int                   io_priority,
								 GCancellable         *cancellable,
								 GAsyncReadyCallback   callback,
								 gpointer              data);
static gboolean   g_daemon_file_output_stream_close_finish      (GOutputStream        *stream,
								 GAsyncResult         *result,
								 GError              **error);
static void       g_daemon_file_output_stream_query_info_async  (GFileOutputStream    *stream,
								 const char           *attributes,
								 int                   io_priority,
								 GCancellable         *cancellable,
								 GAsyncReadyCallback   callback,
								 gpointer              user_data);
static GFileInfo *g_daemon_file_output_stream_query_info_finish (GFileOutputStream    *stream,
								 GAsyncResult         *result,
								 GError              **error);



G_DEFINE_TYPE (GDaemonFileOutputStream, g_daemon_file_output_stream,
	       G_TYPE_FILE_OUTPUT_STREAM)

static void
query_operation_free (QueryOperation *op)
{
  g_free (op->attributes);
  g_free (op);
}

static void
g_string_remove_in_front (GString *string,
			  gsize bytes)
{
  memmove (string->str,
	   string->str + bytes,
	   string->len - bytes);
  g_string_truncate (string,
		     string->len - bytes);
}

static void
g_daemon_file_output_stream_finalize (GObject *object)
{
  GDaemonFileOutputStream *file;
  
  file = G_DAEMON_FILE_OUTPUT_STREAM (object);

  if (file->command_stream)
    g_object_unref (file->command_stream);
  if (file->data_stream)
    g_object_unref (file->data_stream);

  g_string_free (file->input_buffer, TRUE);
  g_string_free (file->output_buffer, TRUE);

  g_free (file->etag);
  
  if (G_OBJECT_CLASS (g_daemon_file_output_stream_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_file_output_stream_parent_class)->finalize) (object);
}

static void
g_daemon_file_output_stream_class_init (GDaemonFileOutputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GOutputStreamClass *stream_class = G_OUTPUT_STREAM_CLASS (klass);
  GFileOutputStreamClass *file_stream_class = G_FILE_OUTPUT_STREAM_CLASS (klass);
  
  gobject_class->finalize = g_daemon_file_output_stream_finalize;

  stream_class->write_fn = g_daemon_file_output_stream_write;
  stream_class->close_fn = g_daemon_file_output_stream_close;
  
  stream_class->write_async = g_daemon_file_output_stream_write_async;
  stream_class->write_finish = g_daemon_file_output_stream_write_finish;
  stream_class->close_async = g_daemon_file_output_stream_close_async;
  stream_class->close_finish = g_daemon_file_output_stream_close_finish;
  
  file_stream_class->tell = g_daemon_file_output_stream_tell;
  file_stream_class->can_seek = g_daemon_file_output_stream_can_seek;
  file_stream_class->seek = g_daemon_file_output_stream_seek;
  file_stream_class->can_truncate = g_daemon_file_output_stream_can_truncate;
  file_stream_class->truncate_fn = g_daemon_file_output_stream_truncate;
  file_stream_class->query_info = g_daemon_file_output_stream_query_info;
  file_stream_class->get_etag = g_daemon_file_output_stream_get_etag;
  file_stream_class->query_info_async = g_daemon_file_output_stream_query_info_async;
  file_stream_class->query_info_finish = g_daemon_file_output_stream_query_info_finish;
}

static void
g_daemon_file_output_stream_init (GDaemonFileOutputStream *info)
{
  info->output_buffer = g_string_new ("");
  info->input_buffer = g_string_new ("");
  info->seq_nr = 1;
}

GFileOutputStream *
g_daemon_file_output_stream_new (int fd,
				 guint32 flags,
				 goffset initial_offset)
{
  GDaemonFileOutputStream *stream;

  stream = g_object_new (G_TYPE_DAEMON_FILE_OUTPUT_STREAM, NULL);

  stream->command_stream = g_unix_output_stream_new (fd, FALSE);
  stream->data_stream = g_unix_input_stream_new (fd, TRUE);
  stream->can_seek = flags & OPEN_FOR_WRITE_FLAG_CAN_SEEK;
  stream->can_truncate = flags & OPEN_FOR_WRITE_FLAG_CAN_TRUNCATE;
  stream->current_offset = initial_offset;
  
  return G_FILE_OUTPUT_STREAM (stream);
}

static gboolean
error_is_cancel (GError *error)
{
  return error != NULL &&
    error->domain == G_IO_ERROR &&
    error->code == G_IO_ERROR_CANCELLED;
}

static void
unappend_request (GDaemonFileOutputStream *stream)
{
  g_assert (stream->output_buffer->len >= G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE);
  stream->seq_nr--;
  g_string_truncate (stream->output_buffer,
		     stream->output_buffer->len - G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE);
}

static void
append_request (GDaemonFileOutputStream *stream, guint32 command,
		guint32 arg1, guint32 arg2, guint32 data_len, guint32 *seq_nr)
{
  GVfsDaemonSocketProtocolRequest cmd;

  g_assert (sizeof (cmd) == G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE);
  
  if (seq_nr)
    *seq_nr = stream->seq_nr;
  
  cmd.command = g_htonl (command);
  cmd.seq_nr = g_htonl (stream->seq_nr);
  cmd.arg1 = g_htonl (arg1);
  cmd.arg2 = g_htonl (arg2);
  cmd.data_len = g_htonl (data_len);

  stream->seq_nr++;
  
  g_string_append_len (stream->output_buffer,
		       (char *)&cmd, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE);
}

static gsize
get_reply_header_missing_bytes (GString *buffer)
{
  GVfsDaemonSocketProtocolReply *reply;
  guint32 type;
  guint32 arg2;
  
  if (buffer->len < G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE)
    return G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE - buffer->len;

  reply = (GVfsDaemonSocketProtocolReply *)buffer->str;

  type = g_ntohl (reply->type);
  arg2 = g_ntohl (reply->arg2);

  /* ERROR, CLOSED and INFO has extra data w/ len in arg2 */
  if (type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR ||
      type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_CLOSED ||
      type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_INFO)
    return G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE + arg2 - buffer->len;
  return 0;
}

static char *
decode_reply (GString *buffer, GVfsDaemonSocketProtocolReply *reply_out)
{
  GVfsDaemonSocketProtocolReply *reply;
  reply = (GVfsDaemonSocketProtocolReply *)buffer->str;
  reply_out->type = g_ntohl (reply->type);
  reply_out->seq_nr = g_ntohl (reply->seq_nr);
  reply_out->arg1 = g_ntohl (reply->arg1);
  reply_out->arg2 = g_ntohl (reply->arg2);
  
  return buffer->str + G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE;
}

static void
decode_error (GVfsDaemonSocketProtocolReply *reply, char *data, GError **error)
{
  g_set_error_literal (error,
		       g_quark_from_string (data),
		       reply->arg1,
		       data + strlen (data) + 1);
}


static gboolean
run_sync_state_machine (GDaemonFileOutputStream *file,
			state_machine_iterator iterator,
			gpointer data,
			GCancellable *cancellable,
			GError **error)
{
  gssize res;
  StateOp io_op;
  IOOperationData io_data;
  GError *io_error;

  memset (&io_data, 0, sizeof (io_data));
  
  while (TRUE)
    {
      if (cancellable)
	io_data.cancelled = g_cancellable_is_cancelled (cancellable);
      
      io_op = iterator (file, &io_data, data);

      if (io_op == STATE_OP_DONE)
	return TRUE;
      
      io_error = NULL;
      if (io_op == STATE_OP_READ)
	{
	  res = g_input_stream_read (file->data_stream,
				     io_data.io_buffer, io_data.io_size,
				     io_data.io_allow_cancel ? cancellable : NULL,
				     &io_error);
	}
      else if (io_op == STATE_OP_SKIP)
	{
	  res = g_input_stream_skip (file->data_stream,
				     io_data.io_size,
				     io_data.io_allow_cancel ? cancellable : NULL,
				     &io_error);
	}
      else if (io_op == STATE_OP_WRITE)
	{
	  res = g_output_stream_write (file->command_stream,
				       io_data.io_buffer, io_data.io_size,
				       io_data.io_allow_cancel ? cancellable : NULL,
				       &io_error);
	}
      else
	{
	  res = 0;
	  g_assert_not_reached ();
	}

      if (res == -1)
	{
	  if (error_is_cancel (io_error))
	    {
	      io_data.io_res = 0;
	      io_data.io_cancelled = TRUE;
	      g_error_free (io_error);
	    }
	  else
	    {
	      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Error in stream protocol: %s"), io_error->message);
	      g_error_free (io_error);
	      return FALSE;
	    }
	}
      else if (res == 0 && io_data.io_size != 0)
	{
	  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		       _("Error in stream protocol: %s"), _("End of stream"));
	  return FALSE;
	}
      else
	{
	  io_data.io_res = res;
	  io_data.io_cancelled = FALSE;
	}
    }
}

/* read cycle:

   if we know of a (partially read) matching outstanding block, read from it
   create packet, append to outgoing
   flush outgoing
   start processing output, looking for a data block with same seek gen,
    or an error with same seq nr
   on cancel, send cancel command and go back to loop
 */

static StateOp
iterate_write_state_machine (GDaemonFileOutputStream *file, IOOperationData *io_op, WriteOperation *op)
{
  gsize len;

  while (TRUE)
    {
      switch (op->state)
	{
	  /* Initial state for read op */
	case WRITE_STATE_INIT:
	  append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_WRITE,
			  op->buffer_size, 0, op->buffer_size, &op->seq_nr);
	  op->state = WRITE_STATE_WROTE_COMMAND;
	  io_op->io_buffer = file->output_buffer->str;
	  io_op->io_size = file->output_buffer->len;
	  io_op->io_allow_cancel = TRUE; /* Allow cancel before first byte of request sent */
	  return STATE_OP_WRITE;

	  /* wrote parts of output_buffer */
	case WRITE_STATE_WROTE_COMMAND:
	  if (io_op->io_cancelled)
	    {
	      if (!op->sent_cancel)
		unappend_request (file);
	      op->ret_val = -1;
	      g_set_error_literal (&op->ret_error,
				   G_IO_ERROR,
				   G_IO_ERROR_CANCELLED,
                        	   _("Operation was cancelled"));
	      return STATE_OP_DONE;
	    }
	  
	  if (io_op->io_res < file->output_buffer->len)
	    {
	      g_string_remove_in_front (file->output_buffer,
					io_op->io_res);
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }
	  g_string_truncate (file->output_buffer, 0);

	  op->buffer_pos = 0;
	  if (op->sent_cancel)
	    op->state = WRITE_STATE_HANDLE_INPUT;
	  else
	    op->state = WRITE_STATE_SEND_DATA;
	  break;

	  /* No op */
	case WRITE_STATE_SEND_DATA:
	  op->buffer_pos += io_op->io_res;
	  
	  if (op->buffer_pos < op->buffer_size)
	    {
	      io_op->io_buffer = (char *)(op->buffer + op->buffer_pos);
	      io_op->io_size = op->buffer_size - op->buffer_pos;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }

	  op->state = WRITE_STATE_HANDLE_INPUT;
	  break;

	  /* No op */
	case WRITE_STATE_HANDLE_INPUT:
	  if (io_op->cancelled && !op->sent_cancel)
	    {
	      op->sent_cancel = TRUE;
	      append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL,
			      op->seq_nr, 0, 0, NULL);
	      op->state = WRITE_STATE_WROTE_COMMAND;
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }

	  if (io_op->io_res > 0)
	    {
	      gsize unread_size = io_op->io_size - io_op->io_res;
	      g_string_set_size (file->input_buffer,
				 file->input_buffer->len - unread_size);
	    }
	  
	  len = get_reply_header_missing_bytes (file->input_buffer);
	  if (len > 0)
	    {
	      gsize current_len = file->input_buffer->len;
	      g_string_set_size (file->input_buffer,
				 current_len + len);
	      io_op->io_buffer = file->input_buffer->str + current_len;
	      io_op->io_size = len;
	      io_op->io_allow_cancel = !op->sent_cancel;
	      return STATE_OP_READ;
	    }

	  /* Got full header */

	  {
	    GVfsDaemonSocketProtocolReply reply;
	    char *data;
	    data = decode_reply (file->input_buffer, &reply);

	    if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR &&
		reply.seq_nr == op->seq_nr)
	      {
		op->ret_val = -1;
		decode_error (&reply, data, &op->ret_error);
		g_string_truncate (file->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_WRITTEN &&
		     reply.seq_nr == op->seq_nr)
	      {
		op->ret_val = reply.arg1;
		g_string_truncate (file->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    /* Ignore other reply types */
	  }

	  g_string_truncate (file->input_buffer, 0);
	  
	  /* This wasn't interesting, read next reply */
	  op->state = WRITE_STATE_HANDLE_INPUT;
	  break;
	  
	default:
	  g_assert_not_reached ();
	}
      
      /* Clear io_op between non-op state switches */
      io_op->io_size = 0;
      io_op->io_res = 0;
      io_op->io_cancelled = FALSE;
 
    }
}

static gssize
g_daemon_file_output_stream_write (GOutputStream *stream,
				   const void   *buffer,
				   gsize         count,
				   GCancellable *cancellable,
				   GError      **error)
{
  GDaemonFileOutputStream *file;
  WriteOperation op;

  file = G_DAEMON_FILE_OUTPUT_STREAM (stream);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return -1;
  
  /* Limit for sanity and to avoid 32bit overflow */
  if (count > MAX_WRITE_SIZE)
    count = MAX_WRITE_SIZE;

  memset (&op, 0, sizeof (op));
  op.state = WRITE_STATE_INIT;
  op.buffer = buffer;
  op.buffer_size = count;
  
  if (!run_sync_state_machine (file, (state_machine_iterator)iterate_write_state_machine,
			       &op, cancellable, error))
    return -1; /* IO Error */

  if (op.ret_val == -1)
    g_propagate_error (error, op.ret_error);
  else
    file->current_offset += op.ret_val;
  
  return op.ret_val;
}

static StateOp
iterate_close_state_machine (GDaemonFileOutputStream *file, IOOperationData *io_op, CloseOperation *op)
{
  gsize len;

  while (TRUE)
    {
      switch (op->state)
	{
	  /* Initial state for read op */
	case CLOSE_STATE_INIT:
	  append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CLOSE,
			  0, 0, 0, &op->seq_nr);
	  op->state = CLOSE_STATE_WROTE_REQUEST;
	  io_op->io_buffer = file->output_buffer->str;
	  io_op->io_size = file->output_buffer->len;
	  io_op->io_allow_cancel = TRUE; /* Allow cancel before first byte of request sent */
	  return STATE_OP_WRITE;

	  /* wrote parts of output_buffer */
	case CLOSE_STATE_WROTE_REQUEST:
	  if (io_op->io_cancelled)
	    {
	      if (!op->sent_cancel)
		unappend_request (file);
	      op->ret_val = FALSE;
	      g_set_error_literal (&op->ret_error,
				   G_IO_ERROR,
				   G_IO_ERROR_CANCELLED,
				   _("Operation was cancelled"));
	      return STATE_OP_DONE;
	    }

	  if (io_op->io_res < file->output_buffer->len)
	    {
	      g_string_remove_in_front (file->output_buffer,
					io_op->io_res);
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }
	  g_string_truncate (file->output_buffer, 0);

	  op->state = CLOSE_STATE_HANDLE_INPUT;
	  break;

	  /* No op */
	case CLOSE_STATE_HANDLE_INPUT:
	  if (io_op->cancelled && !op->sent_cancel)
	    {
	      op->sent_cancel = TRUE;
	      append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL,
			      op->seq_nr, 0, 0, NULL);
	      op->state = CLOSE_STATE_WROTE_REQUEST;
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }

	  if (io_op->io_res > 0)
	    {
	      gsize unread_size = io_op->io_size - io_op->io_res;
	      g_string_set_size (file->input_buffer,
				 file->input_buffer->len - unread_size);
	    }
	  
	  len = get_reply_header_missing_bytes (file->input_buffer);
	  if (len > 0)
	    {
	      gsize current_len = file->input_buffer->len;
	      g_string_set_size (file->input_buffer,
				 current_len + len);
	      io_op->io_buffer = file->input_buffer->str + current_len;
	      io_op->io_size = len;
	      io_op->io_allow_cancel = !op->sent_cancel;
	      return STATE_OP_READ;
	    }

	  /* Got full header */

	  {
	    GVfsDaemonSocketProtocolReply reply;
	    char *data;
	    data = decode_reply (file->input_buffer, &reply);

	    if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR &&
		reply.seq_nr == op->seq_nr)
	      {
		op->ret_val = FALSE;
		decode_error (&reply, data, &op->ret_error);
		g_string_truncate (file->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_CLOSED &&
		     reply.seq_nr == op->seq_nr)
	      {
		op->ret_val = TRUE;
		if (reply.arg2 > 0)
		  file->etag = g_strndup (data, reply.arg2);
		g_string_truncate (file->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    /* Ignore other reply types */
	  }

	  g_string_truncate (file->input_buffer, 0);
	  
	  /* This wasn't interesting, read next reply */
	  op->state = CLOSE_STATE_HANDLE_INPUT;
	  break;

	default:
	  g_assert_not_reached ();
	}
      
      /* Clear io_op between non-op state switches */
      io_op->io_size = 0;
      io_op->io_res = 0;
      io_op->io_cancelled = FALSE;
    }
}


static gboolean
g_daemon_file_output_stream_close (GOutputStream *stream,
				  GCancellable *cancellable,
				  GError      **error)
{
  GDaemonFileOutputStream *file;
  CloseOperation op;
  gboolean res;

  file = G_DAEMON_FILE_OUTPUT_STREAM (stream);

  /* We need to do a full roundtrip to guarantee that the writes have
     reached the disk. */

  memset (&op, 0, sizeof (op));
  op.state = CLOSE_STATE_INIT;

  if (!run_sync_state_machine (file, (state_machine_iterator)iterate_close_state_machine,
			       &op, cancellable, error))
    res = FALSE;
  else
    {
      if (!op.ret_val)
	g_propagate_error (error, op.ret_error);
      res = op.ret_val;
    }

  /* Return the first error, but close all streams */
  if (res)
    res = g_output_stream_close (file->command_stream, cancellable, error);
  else
    g_output_stream_close (file->command_stream, cancellable, NULL);
    
  if (res)
    res = g_input_stream_close (file->data_stream, cancellable, error);
  else
    g_input_stream_close (file->data_stream, cancellable, NULL);
  
  return res;
}

static goffset
g_daemon_file_output_stream_tell (GFileOutputStream *stream)
{
  GDaemonFileOutputStream *file;

  file = G_DAEMON_FILE_OUTPUT_STREAM (stream);
  
  return file->current_offset;
}

static gboolean
g_daemon_file_output_stream_can_seek (GFileOutputStream *stream)
{
  GDaemonFileOutputStream *file;

  file = G_DAEMON_FILE_OUTPUT_STREAM (stream);

  return file->can_seek;
}

static StateOp
iterate_seek_state_machine (GDaemonFileOutputStream *file, IOOperationData *io_op, SeekOperation *op)
{
  gsize len;
  guint32 request;

  while (TRUE)
    {
      switch (op->state)
	{
	  /* Initial state for read op */
	case SEEK_STATE_INIT:
	  request = G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_SET;
	  if (op->seek_type == G_SEEK_CUR)
	    op->offset = file->current_offset + op->offset;
	  else if (op->seek_type == G_SEEK_END)
	    request = G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_END;
	  append_request (file, request,
			  op->offset & 0xffffffff,
			  op->offset >> 32,
			  0,
			  &op->seq_nr);
	  op->state = SEEK_STATE_WROTE_REQUEST;
	  io_op->io_buffer = file->output_buffer->str;
	  io_op->io_size = file->output_buffer->len;
	  io_op->io_allow_cancel = TRUE; /* Allow cancel before first byte of request sent */
	  return STATE_OP_WRITE;

	  /* wrote parts of output_buffer */
	case SEEK_STATE_WROTE_REQUEST:
	  if (io_op->io_cancelled)
	    {
	      if (!op->sent_cancel)
		unappend_request (file);
	      op->ret_val = -1;
	      g_set_error_literal (&op->ret_error,
				   G_IO_ERROR,
				   G_IO_ERROR_CANCELLED,
				   _("Operation was cancelled"));
	      return STATE_OP_DONE;
	    }

	  if (io_op->io_res < file->output_buffer->len)
	    {
	      g_string_remove_in_front (file->output_buffer,
					io_op->io_res);
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }
	  g_string_truncate (file->output_buffer, 0);

	  op->state = SEEK_STATE_HANDLE_INPUT;
	  break;

	  /* No op */
	case SEEK_STATE_HANDLE_INPUT:
	  if (io_op->cancelled && !op->sent_cancel)
	    {
	      op->sent_cancel = TRUE;
	      append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL,
			      op->seq_nr, 0, 0, NULL);
	      op->state = SEEK_STATE_WROTE_REQUEST;
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }

	  if (io_op->io_res > 0)
	    {
	      gsize unread_size = io_op->io_size - io_op->io_res;
	      g_string_set_size (file->input_buffer,
				 file->input_buffer->len - unread_size);
	    }
	  
	  len = get_reply_header_missing_bytes (file->input_buffer);
	  if (len > 0)
	    {
	      gsize current_len = file->input_buffer->len;
	      g_string_set_size (file->input_buffer,
				 current_len + len);
	      io_op->io_buffer = file->input_buffer->str + current_len;
	      io_op->io_size = len;
	      io_op->io_allow_cancel = !op->sent_cancel;
	      return STATE_OP_READ;
	    }

	  /* Got full header */
	  
	  {
	    GVfsDaemonSocketProtocolReply reply;
	    char *data;
	    data = decode_reply (file->input_buffer, &reply);

	    if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR &&
		reply.seq_nr == op->seq_nr)
	      {
		op->ret_val = FALSE;
		decode_error (&reply, data, &op->ret_error);
		g_string_truncate (file->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SEEK_POS &&
		     reply.seq_nr == op->seq_nr)
	      {
		op->ret_val = TRUE;
		op->ret_offset = ((goffset)reply.arg2) << 32 | (goffset)reply.arg1;
		g_string_truncate (file->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    /* Ignore other reply types */
	  }

	  g_string_truncate (file->input_buffer, 0);
	  
	  /* This wasn't interesting, read next reply */
	  op->state = SEEK_STATE_HANDLE_INPUT;
	  break;

	default:
	  g_assert_not_reached ();
	}
      
      /* Clear io_op between non-op state switches */
      io_op->io_size = 0;
      io_op->io_res = 0;
      io_op->io_cancelled = FALSE;
    }
}

static gboolean
g_daemon_file_output_stream_seek (GFileOutputStream *stream,
				 goffset offset,
				 GSeekType type,
				 GCancellable *cancellable,
				 GError **error)
{
  GDaemonFileOutputStream *file;
  SeekOperation op;

  file = G_DAEMON_FILE_OUTPUT_STREAM (stream);

  if (!file->can_seek)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			   _("Seek not supported on stream"));
      return FALSE;
    }
  
  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;
  
  memset (&op, 0, sizeof (op));
  op.state = SEEK_STATE_INIT;
  op.offset = offset;
  op.seek_type = type;
  
  if (!run_sync_state_machine (file, (state_machine_iterator)iterate_seek_state_machine,
			       &op, cancellable, error))
    return FALSE; /* IO Error */

  if (!op.ret_val)
    g_propagate_error (error, op.ret_error);
  else
    file->current_offset = op.ret_offset;
  
  return op.ret_val;
}

static StateOp
iterate_truncate_state_machine (GDaemonFileOutputStream *file,
                                IOOperationData *io_op,
                                TruncateOperation *op)
{
  gsize len;

  while (TRUE)
    {
      switch (op->state)
        {
        case TRUNCATE_STATE_INIT:
          append_request (file,
                          G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_TRUNCATE,
                          op->size & 0xffffffff,
                          op->size >> 32,
                          0,
                          &op->seq_nr);
          op->state = TRUNCATE_STATE_WROTE_REQUEST;
          io_op->io_buffer = file->output_buffer->str;
          io_op->io_size = file->output_buffer->len;
          io_op->io_allow_cancel = TRUE;
          return STATE_OP_WRITE;

        case TRUNCATE_STATE_WROTE_REQUEST:
          if (io_op->io_cancelled)
            {
              if (!op->sent_cancel)
                unappend_request (file);
              op->ret_val = FALSE;
              g_set_error_literal (&op->ret_error,
                                   G_IO_ERROR,
                                   G_IO_ERROR_CANCELLED,
                                   _("Operation was cancelled"));
              return STATE_OP_DONE;
            }

          if (io_op->io_res < file->output_buffer->len)
            {
              g_string_remove_in_front (file->output_buffer, io_op->io_res);
              io_op->io_buffer = file->output_buffer->str;
              io_op->io_size = file->output_buffer->len;
              io_op->io_allow_cancel = FALSE;
              return STATE_OP_WRITE;
            }
          g_string_truncate (file->output_buffer, 0);

          op->state = TRUNCATE_STATE_HANDLE_INPUT;
          break;

        case TRUNCATE_STATE_HANDLE_INPUT:
          if (io_op->cancelled && !op->sent_cancel)
            {
              op->sent_cancel = TRUE;
              append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL,
                              op->seq_nr, 0, 0, NULL);
              op->state = TRUNCATE_STATE_WROTE_REQUEST;
              io_op->io_buffer = file->output_buffer->str;
              io_op->io_size = file->output_buffer->len;
              io_op->io_allow_cancel = FALSE;
              return STATE_OP_WRITE;
            }

          if (io_op->io_res > 0)
            {
              gsize unread_size = io_op->io_size - io_op->io_res;
              g_string_set_size (file->input_buffer,
                                 file->input_buffer->len - unread_size);
            }

          len = get_reply_header_missing_bytes (file->input_buffer);
          if (len > 0)
            {
              gsize current_len = file->input_buffer->len;
              g_string_set_size (file->input_buffer, current_len + len);
              io_op->io_buffer = file->input_buffer->str + current_len;
              io_op->io_size = len;
              io_op->io_allow_cancel = !op->sent_cancel;
              return STATE_OP_READ;
            }

          {
            GVfsDaemonSocketProtocolReply reply;
            char *data;
            data = decode_reply (file->input_buffer, &reply);

            if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR &&
                reply.seq_nr == op->seq_nr)
              {
                op->ret_val = FALSE;
                decode_error (&reply, data, &op->ret_error);
                g_string_truncate (file->input_buffer, 0);
                return STATE_OP_DONE;
              }
            else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_TRUNCATED &&
                     reply.seq_nr == op->seq_nr)
              {
                op->ret_val = TRUE;
                g_string_truncate (file->input_buffer, 0);
                return STATE_OP_DONE;
              }
            /* Ignore other reply types */
          }

          g_string_truncate (file->input_buffer, 0);

          /* This wasn't interesting, read next reply */
          op->state = TRUNCATE_STATE_HANDLE_INPUT;
          break;

        default:
          g_assert_not_reached ();
        }

      /* Clear io_op between non-op state switches */
      io_op->io_size = 0;
      io_op->io_res = 0;
      io_op->io_cancelled = FALSE;
    }
}

static gboolean
g_daemon_file_output_stream_can_truncate (GFileOutputStream *stream)
{
  GDaemonFileOutputStream *file;

  file = G_DAEMON_FILE_OUTPUT_STREAM (stream);

  return file->can_truncate;
}

static gboolean
g_daemon_file_output_stream_truncate (GFileOutputStream *stream,
                                      goffset            size,
                                      GCancellable      *cancellable,
                                      GError           **error)
{
  GDaemonFileOutputStream *file;
  TruncateOperation op;

  file = G_DAEMON_FILE_OUTPUT_STREAM (stream);

  if (!file->can_truncate)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           _("Truncate not supported on stream"));
      return FALSE;
    }

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return FALSE;

  memset (&op, 0, sizeof (op));
  op.state = TRUNCATE_STATE_INIT;
  op.size = size;

  if (!run_sync_state_machine (file, (state_machine_iterator)iterate_truncate_state_machine,
                               &op, cancellable, error))
    return FALSE; /* IO Error */

  if (!op.ret_val)
    g_propagate_error (error, op.ret_error);

  return op.ret_val;
}

static char *
g_daemon_file_output_stream_get_etag (GFileOutputStream     *stream)
{
  GDaemonFileOutputStream *file;

  file = G_DAEMON_FILE_OUTPUT_STREAM (stream);

  return g_strdup (file->etag);
}

static StateOp
iterate_query_state_machine (GDaemonFileOutputStream *file,
			     IOOperationData *io_op,
			     QueryOperation *op)
{
  gsize len;
  guint32 request;

  while (TRUE)
    {
      switch (op->state)
	{
	  /* Initial state for read op */
	case QUERY_STATE_INIT:
	  request = G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_QUERY_INFO;
	  append_request (file, request,
			  0,
			  0,
			  strlen (op->attributes),
			  &op->seq_nr);
	  g_string_append (file->output_buffer,
			   op->attributes);
	  
	  op->state = QUERY_STATE_WROTE_REQUEST;
	  io_op->io_buffer = file->output_buffer->str;
	  io_op->io_size = file->output_buffer->len;
	  io_op->io_allow_cancel = TRUE; /* Allow cancel before first byte of request sent */
	  return STATE_OP_WRITE;

	  /* wrote parts of output_buffer */
	case QUERY_STATE_WROTE_REQUEST:
	  if (io_op->io_cancelled)
	    {
	      if (!op->sent_cancel)
		unappend_request (file);
	      op->info = NULL;
	      g_set_error_literal (&op->ret_error,
				   G_IO_ERROR,
				   G_IO_ERROR_CANCELLED,
				   _("Operation was cancelled"));
	      return STATE_OP_DONE;
	    }

	  if (io_op->io_res < file->output_buffer->len)
	    {
	      g_string_remove_in_front (file->output_buffer,
					io_op->io_res);
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }
	  g_string_truncate (file->output_buffer, 0);

	  op->state = QUERY_STATE_HANDLE_INPUT;
	  break;

	  /* No op */
	case QUERY_STATE_HANDLE_INPUT:
	  if (io_op->cancelled && !op->sent_cancel)
	    {
	      op->sent_cancel = TRUE;
	      append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL,
			      op->seq_nr, 0, 0, NULL);
	      op->state = QUERY_STATE_WROTE_REQUEST;
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }


	  if (io_op->io_res > 0)
	    {
	      gsize unread_size = io_op->io_size - io_op->io_res;
	      g_string_set_size (file->input_buffer,
				 file->input_buffer->len - unread_size);
	    }
	  
	  len = get_reply_header_missing_bytes (file->input_buffer);
	  if (len > 0)
	    {
	      gsize current_len = file->input_buffer->len;
	      g_string_set_size (file->input_buffer,
				 current_len + len);
	      io_op->io_buffer = file->input_buffer->str + current_len;
	      io_op->io_size = len;
	      io_op->io_allow_cancel = !op->sent_cancel;
	      return STATE_OP_READ;
	    }

	  /* Got full header */

	  {
	    GVfsDaemonSocketProtocolReply reply;
	    char *data;
	    data = decode_reply (file->input_buffer, &reply);

	    if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR &&
		reply.seq_nr == op->seq_nr)
	      {
		op->info = NULL;
		decode_error (&reply, data, &op->ret_error);
		g_string_truncate (file->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_INFO &&
		     reply.seq_nr == op->seq_nr)
	      {
		op->info = gvfs_file_info_demarshal (data, reply.arg2);
		g_string_truncate (file->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    /* Ignore other reply types */
	  }

	  g_string_truncate (file->input_buffer, 0);
	  
	  /* This wasn't interesting, read next reply */
	  op->state = QUERY_STATE_HANDLE_INPUT;
	  break;

	default:
	  g_assert_not_reached ();
	}
      
      /* Clear io_op between non-op state switches */
      io_op->io_size = 0;
      io_op->io_res = 0;
      io_op->io_cancelled = FALSE;
    }
}

static GFileInfo *
g_daemon_file_output_stream_query_info (GFileOutputStream    *stream,
					const char           *attributes,
					GCancellable         *cancellable,
					GError              **error)
{
   GDaemonFileOutputStream *file;
   QueryOperation op;

  file = G_DAEMON_FILE_OUTPUT_STREAM (stream);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;
  
  memset (&op, 0, sizeof (op));
  op.state = QUERY_STATE_INIT;
  if (attributes)
    op.attributes = (char *)attributes;
  else
    op.attributes = "";
  
  if (!run_sync_state_machine (file, (state_machine_iterator)iterate_query_state_machine,
			       &op, cancellable, error))
    return NULL; /* IO Error */

  if (op.info == NULL)
    g_propagate_error (error, op.ret_error);
  
  return op.info;
}

/************************************************************************
 *         Async I/O Code                                               *
 ************************************************************************/

typedef struct AsyncIterator AsyncIterator;

typedef void (*AsyncIteratorDone) (GTask *task);

struct AsyncIterator {
  AsyncIteratorDone done_cb;
  IOOperationData io_data;
  state_machine_iterator iterator;
  GTask *task;
};

static void async_iterate (AsyncIterator *iterator);

static void
async_op_handle (AsyncIterator *iterator,
		 gssize res,
		 GError *io_error)
{
  IOOperationData *io_data = &iterator->io_data;

  if (io_error != NULL)
    {
      if (error_is_cancel (io_error))
	{
	  io_data->io_res = 0;
	  io_data->io_cancelled = TRUE;
	}
      else
	{
        g_task_return_new_error (iterator->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 _("Error in stream protocol: %s"), io_error->message);
        g_object_unref (iterator->task);
        g_free (iterator);
        return;
	}
    }
  else if (res == 0 && io_data->io_size != 0)
    {
      g_task_return_new_error (iterator->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               _("Error in stream protocol: %s"), _("End of stream"));
      g_object_unref (iterator->task);
      g_free (iterator);
      return;
    }
  else
    {
      io_data->io_res = res;
      io_data->io_cancelled = FALSE;
    }
  
  async_iterate (iterator);
}

static void
async_read_op_callback (GObject *source_object,
			GAsyncResult *res,
			gpointer      user_data)
{
  GInputStream *stream = G_INPUT_STREAM (source_object);
  gssize count_read;
  GError *error = NULL;
  
  count_read = g_input_stream_read_finish (stream, res, &error);
  
  async_op_handle ((AsyncIterator *)user_data, count_read, error);
  if (error)
    g_error_free (error);
}

static void
async_skip_op_callback (GObject *source_object,
			GAsyncResult *res,
			gpointer      user_data)
{
  GInputStream *stream = G_INPUT_STREAM (source_object);
  gssize count_skipped;
  GError *error = NULL;
  
  count_skipped = g_input_stream_skip_finish (stream, res, &error);
  
  async_op_handle ((AsyncIterator *)user_data, count_skipped, error);
  if (error)
    g_error_free (error);
}

static void
async_write_op_callback (GObject *source_object,
			 GAsyncResult *res,
			 gpointer user_data)
{
  GOutputStream *stream = G_OUTPUT_STREAM (source_object);
  gssize bytes_written;
  GError *error = NULL;
  
  bytes_written = g_output_stream_write_finish (stream, res, &error);
  
  async_op_handle ((AsyncIterator *)user_data, bytes_written, error);
  if (error)
    g_error_free (error);
}

static void
async_iterate (AsyncIterator *iterator)
{
  IOOperationData *io_data = &iterator->io_data;
  GDaemonFileOutputStream *file;
  GCancellable *cancellable = g_task_get_cancellable (iterator->task);
  StateOp io_op;

  io_data->cancelled = g_cancellable_is_cancelled (cancellable);

  file = G_DAEMON_FILE_OUTPUT_STREAM (g_task_get_source_object (iterator->task));
  io_op = iterator->iterator (file, io_data, g_task_get_task_data (iterator->task));

  if (io_op == STATE_OP_DONE)
    {
      iterator->done_cb (iterator->task);
      g_free (iterator);
      return;
    }

  /* TODO: Handle allow_cancel... */
  
  if (io_op == STATE_OP_READ)
    {
      g_input_stream_read_async (file->data_stream,
				 io_data->io_buffer, io_data->io_size,
				 g_task_get_priority (iterator->task),
				 io_data->io_allow_cancel ? cancellable : NULL,
				 async_read_op_callback, iterator);
    }
  else if (io_op == STATE_OP_SKIP)
    {
      g_input_stream_skip_async (file->data_stream,
				 io_data->io_size,
				 g_task_get_priority (iterator->task),
				 io_data->io_allow_cancel ? cancellable : NULL,
				 async_skip_op_callback, iterator);
    }
  else if (io_op == STATE_OP_WRITE)
    {
      g_output_stream_write_async (file->command_stream,
				   io_data->io_buffer, io_data->io_size,
				   g_task_get_priority (iterator->task),
				   io_data->io_allow_cancel ? cancellable : NULL,
				   async_write_op_callback, iterator);
    }
  else
    g_assert_not_reached ();
}

static void
run_async_state_machine (GTask *task,
			 state_machine_iterator iterator_cb,
			 AsyncIteratorDone done_cb)
{
  AsyncIterator *iterator;

  iterator = g_new0 (AsyncIterator, 1);
  iterator->iterator = iterator_cb;
  iterator->done_cb = done_cb;
  iterator->task = task;

  async_iterate (iterator);
}

static void
async_write_done (GTask *task)
{
  WriteOperation *op;
  gssize count_written;
  GError *error;

  op = g_task_get_task_data (task);

  count_written = op->ret_val;
  error = op->ret_error;

  if (count_written == -1)
    g_task_return_error (task, error);
  else
    g_task_return_int (task, count_written);

  g_object_unref (task);
}

static void
g_daemon_file_output_stream_write_async  (GOutputStream      *stream,
					  const void         *buffer,
					  gsize               count,
					  int                 io_priority,
					  GCancellable       *cancellable,
					  GAsyncReadyCallback callback,
					  gpointer            data)
{
  WriteOperation *op;
  GTask *task;

  task = g_task_new (stream, cancellable, callback, data);
  g_task_set_priority (task, io_priority);
  g_task_set_source_tag (task, g_daemon_file_output_stream_write_async);

  /* Limit for sanity and to avoid 32bit overflow */
  if (count > MAX_WRITE_SIZE)
    count = MAX_WRITE_SIZE;

  op = g_new0 (WriteOperation, 1);
  op->state = WRITE_STATE_INIT;
  op->buffer = buffer;
  op->buffer_size = count;

  g_task_set_task_data (task, op, g_free);

  run_async_state_machine (task,
			   (state_machine_iterator)iterate_write_state_machine,
			   async_write_done);
}

static gssize
g_daemon_file_output_stream_write_finish (GOutputStream             *stream,
					  GAsyncResult              *result,
					  GError                   **error)
{
  g_return_val_if_fail (g_task_is_valid (result, stream), -1);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_daemon_file_output_stream_write_async), -1);

  return g_task_propagate_int (G_TASK (result), error);
}

static void
async_close_done (GTask *task)
{
  GDaemonFileOutputStream *file;
  CloseOperation *op;
  gboolean result;
  GError *error;
  GCancellable *cancellable = g_task_get_cancellable (task);

  file = G_DAEMON_FILE_OUTPUT_STREAM (g_task_get_source_object (task));
  op = g_task_get_task_data (task);

  result = op->ret_val;
  error = op->ret_error;

  if (result)
    result = g_output_stream_close (file->command_stream, cancellable, &error);
  else
    g_output_stream_close (file->command_stream, cancellable, NULL);
    
  if (result)
    result = g_input_stream_close (file->data_stream, cancellable, &error);
  else
    g_input_stream_close (file->data_stream, cancellable, NULL);

  if (!result)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);

  g_object_unref (task);
}

static void
g_daemon_file_output_stream_close_async (GOutputStream     *stream,
					int                 io_priority,
					GCancellable       *cancellable,
					GAsyncReadyCallback callback,
					gpointer            data)
{
  CloseOperation *op;
  GTask *task;

  task = g_task_new (stream, cancellable, callback, data);
  g_task_set_priority (task, io_priority);
  g_task_set_source_tag (task, g_daemon_file_output_stream_close_async);

  op = g_new0 (CloseOperation, 1);
  op->state = CLOSE_STATE_INIT;

  g_task_set_task_data (task, op, g_free);

  run_async_state_machine (task,
			   (state_machine_iterator)iterate_close_state_machine,
			   async_close_done);
}

static gboolean
g_daemon_file_output_stream_close_finish (GOutputStream             *stream,
					  GAsyncResult              *result,
					  GError                   **error)
{
  g_return_val_if_fail (g_task_is_valid (result, stream), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_daemon_file_output_stream_close_async), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

static void
async_query_done (GTask *task)
{
  QueryOperation *op;
  GFileInfo *info;
  GError *error;

  op = g_task_get_task_data (task);

  info = op->info;
  error = op->ret_error;

  if (info == NULL)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, info, g_object_unref);

  g_object_unref (task);
}

static void
g_daemon_file_output_stream_query_info_async  (GFileOutputStream    *stream,
					       const char           *attributes,
					       int                   io_priority,
					       GCancellable         *cancellable,
					       GAsyncReadyCallback   callback,
					       gpointer              user_data)
{
  QueryOperation *op;
  GTask *task;

  task = g_task_new (stream, cancellable, callback, user_data);
  g_task_set_priority (task, io_priority);
  g_task_set_source_tag (task, g_daemon_file_output_stream_query_info_async);

  op = g_new0 (QueryOperation, 1);
  op->state = QUERY_STATE_INIT;
  if (attributes)
    op->attributes = g_strdup (attributes);
  else
    op->attributes = g_strdup ("");

  g_task_set_task_data (task, op, (GDestroyNotify)query_operation_free);

  run_async_state_machine (task,
			   (state_machine_iterator)iterate_query_state_machine,
			   async_query_done);
}

static GFileInfo *
g_daemon_file_output_stream_query_info_finish (GFileOutputStream     *stream,
					       GAsyncResult         *result,
					       GError              **error)
{
  g_return_val_if_fail (g_task_is_valid (result, stream), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_daemon_file_output_stream_query_info_async), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
