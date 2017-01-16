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
#include "gdaemonfileinputstream.h"
#include "gvfsdaemondbus.h"
#include <gvfsdaemonprotocol.h>
#include <gvfsfileinfo.h>

#define MAX_READ_SIZE (4*1024*1024)

typedef enum {
  INPUT_STATE_IN_REPLY_HEADER,
  INPUT_STATE_IN_BLOCK
} InputState;

typedef enum {
  STATE_OP_DONE,
  STATE_OP_READ,
  STATE_OP_WRITE,
  STATE_OP_SKIP
} StateOp;

typedef enum {
  READ_STATE_INIT = 0,
  READ_STATE_WROTE_COMMAND,
  READ_STATE_HANDLE_INPUT,
  READ_STATE_HANDLE_INPUT_BLOCK,
  READ_STATE_SKIP_BLOCK,
  READ_STATE_HANDLE_HEADER,
  READ_STATE_READ_BLOCK
} ReadState;

typedef struct {
  ReadState state;

  /* Input */
  char *buffer;
  gsize buffer_size;
  /* Output */
  gssize ret_val;
  GError *ret_error;
  
  gboolean sent_cancel;
  
  guint32 seq_nr;
} ReadOperation;

typedef enum {
  SEEK_STATE_INIT = 0,
  SEEK_STATE_WROTE_REQUEST,
  SEEK_STATE_HANDLE_INPUT,
  SEEK_STATE_HANDLE_INPUT_BLOCK,
  SEEK_STATE_SKIP_BLOCK,
  SEEK_STATE_HANDLE_HEADER
} SeekState;

typedef struct {
  SeekState state;

  /* Input */
  goffset offset;
  GSeekType seek_type;
  /* Output */
  gboolean ret_val;
  GError *ret_error;
  goffset ret_offset;
  
  gboolean sent_cancel;
  gboolean sent_seek;
  
  guint32 seq_nr;
} SeekOperation;

typedef enum {
  CLOSE_STATE_INIT = 0,
  CLOSE_STATE_WROTE_REQUEST,
  CLOSE_STATE_HANDLE_INPUT,
  CLOSE_STATE_HANDLE_INPUT_BLOCK,
  CLOSE_STATE_SKIP_BLOCK,
  CLOSE_STATE_HANDLE_HEADER
} CloseState;

typedef struct {
  CloseState state;

  /* Input */
  
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
  QUERY_STATE_HANDLE_INPUT_BLOCK,
  QUERY_STATE_HANDLE_HEADER,
  QUERY_STATE_READ_BLOCK,
  QUERY_STATE_SKIP_BLOCK
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

typedef struct {
  char *data;
  gsize len;
  int seek_generation;
} PreRead;

typedef StateOp (*state_machine_iterator) (GDaemonFileInputStream *file,
					   IOOperationData *io_op,
					   gpointer data);

struct _GDaemonFileInputStream {
  GFileInputStream parent;

  GOutputStream *command_stream;
  GInputStream *data_stream;
  guint can_seek : 1;
  
  int seek_generation;
  guint32 seq_nr;
  goffset current_offset;

  GList *pre_reads;
  
  InputState input_state;
  gsize input_block_size;
  int input_block_seek_generation;
  GString *input_buffer;
  
  GString *output_buffer;
};

static gssize     g_daemon_file_input_stream_read              (GInputStream         *stream,
								void                 *buffer,
								gsize                 count,
								GCancellable         *cancellable,
								GError              **error);
static gssize     g_daemon_file_input_stream_skip              (GInputStream         *stream,
								gsize                 count,
								GCancellable         *cancellable,
								GError              **error);
static gboolean   g_daemon_file_input_stream_close             (GInputStream         *stream,
								GCancellable         *cancellable,
								GError              **error);
static GFileInfo *g_daemon_file_input_stream_query_info        (GFileInputStream     *stream,
								const char           *attributes,
								GCancellable         *cancellable,
								GError              **error);
static goffset    g_daemon_file_input_stream_tell              (GFileInputStream     *stream);
static gboolean   g_daemon_file_input_stream_can_seek          (GFileInputStream     *stream);
static gboolean   g_daemon_file_input_stream_seek              (GFileInputStream     *stream,
								goffset               offset,
								GSeekType             type,
								GCancellable         *cancellable,
								GError              **error);
static void       g_daemon_file_input_stream_read_async        (GInputStream         *stream,
								void                 *buffer,
								gsize                 count,
								int                   io_priority,
								GCancellable         *cancellable,
								GAsyncReadyCallback   callback,
								gpointer              data);
static gssize     g_daemon_file_input_stream_read_finish       (GInputStream         *stream,
								GAsyncResult         *result,
								GError              **error);
static void       g_daemon_file_input_stream_skip_async        (GInputStream         *stream,
								gsize                 count,
								int                   io_priority,
								GCancellable         *cancellable,
								GAsyncReadyCallback   callback,
								gpointer              data);
static gssize     g_daemon_file_input_stream_skip_finish       (GInputStream         *stream,
								GAsyncResult         *result,
								GError              **error);
static void       g_daemon_file_input_stream_close_async       (GInputStream         *stream,
								int                   io_priority,
								GCancellable         *cancellable,
								GAsyncReadyCallback   callback,
								gpointer              data);
static gboolean   g_daemon_file_input_stream_close_finish      (GInputStream         *stream,
								GAsyncResult         *result,
								GError              **error);
static void       g_daemon_file_input_stream_query_info_async  (GFileInputStream     *stream,
								const char           *attributes,
								int                   io_priority,
								GCancellable         *cancellable,
								GAsyncReadyCallback   callback,
								gpointer              user_data);
static GFileInfo *g_daemon_file_input_stream_query_info_finish (GFileInputStream     *stream,
								GAsyncResult         *result,
								GError              **error);



G_DEFINE_TYPE (GDaemonFileInputStream, g_daemon_file_input_stream,
	       G_TYPE_FILE_INPUT_STREAM)

static void
pre_read_free (PreRead *pre)
{
  g_free (pre->data);
  g_free (pre);
}

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
g_daemon_file_input_stream_finalize (GObject *object)
{
  GDaemonFileInputStream *file;
  
  file = G_DAEMON_FILE_INPUT_STREAM (object);

  if (file->command_stream)
    g_object_unref (file->command_stream);
  if (file->data_stream)
    g_object_unref (file->data_stream);

  while (file->pre_reads)
    {
      PreRead *pre = file->pre_reads->data;
      file->pre_reads = g_list_delete_link (file->pre_reads,
					    file->pre_reads);
      pre_read_free (pre);
    }
  
  g_string_free (file->input_buffer, TRUE);
  g_string_free (file->output_buffer, TRUE);
  
  if (G_OBJECT_CLASS (g_daemon_file_input_stream_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_file_input_stream_parent_class)->finalize) (object);
}

static void
g_daemon_file_input_stream_class_init (GDaemonFileInputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);
  GFileInputStreamClass *file_stream_class = G_FILE_INPUT_STREAM_CLASS (klass);
  
  gobject_class->finalize = g_daemon_file_input_stream_finalize;

  stream_class->read_fn = g_daemon_file_input_stream_read;
  if (0) stream_class->skip = g_daemon_file_input_stream_skip;
  stream_class->close_fn = g_daemon_file_input_stream_close;
  
  stream_class->read_async = g_daemon_file_input_stream_read_async;
  stream_class->read_finish = g_daemon_file_input_stream_read_finish;
  if (0)
    {
      stream_class->skip_async = g_daemon_file_input_stream_skip_async;
      stream_class->skip_finish = g_daemon_file_input_stream_skip_finish;
    }
  stream_class->close_async = g_daemon_file_input_stream_close_async;
  stream_class->close_finish = g_daemon_file_input_stream_close_finish;
  
  file_stream_class->tell = g_daemon_file_input_stream_tell;
  file_stream_class->can_seek = g_daemon_file_input_stream_can_seek;
  file_stream_class->seek = g_daemon_file_input_stream_seek;
  file_stream_class->query_info = g_daemon_file_input_stream_query_info;
  file_stream_class->query_info_async = g_daemon_file_input_stream_query_info_async;
  file_stream_class->query_info_finish = g_daemon_file_input_stream_query_info_finish;

}

static void
g_daemon_file_input_stream_init (GDaemonFileInputStream *info)
{
  info->output_buffer = g_string_new ("");
  info->input_buffer = g_string_new ("");
  info->seq_nr = 1;
}

GFileInputStream *
g_daemon_file_input_stream_new (int fd,
				gboolean can_seek)
{
  GDaemonFileInputStream *stream;

  stream = g_object_new (G_TYPE_DAEMON_FILE_INPUT_STREAM, NULL);

  stream->command_stream = g_unix_output_stream_new (fd, FALSE);
  stream->data_stream = g_unix_input_stream_new (fd, TRUE);
  stream->can_seek = can_seek;
  
  return G_FILE_INPUT_STREAM (stream);
}

static gboolean
error_is_cancel (GError *error)
{
  return error != NULL &&
    error->domain == G_IO_ERROR &&
    error->code == G_IO_ERROR_CANCELLED;
}

static void
unappend_request (GDaemonFileInputStream *stream)
{
  g_assert (stream->output_buffer->len >= G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE);

  stream->seq_nr--;
  g_string_truncate (stream->output_buffer,
		     stream->output_buffer->len - G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE);
}

static void
append_request (GDaemonFileInputStream *stream, guint32 command,
		guint32 arg1, guint32 arg2, guint32 data_len,
		guint32 *seq_nr)
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
  
  if (type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR ||
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
run_sync_state_machine (GDaemonFileInputStream *file,
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
   start processing input, looking for a data block with same seek gen,
    or an error with same seq nr
   on cancel, send cancel command and go back to loop
 */

static StateOp
iterate_read_state_machine (GDaemonFileInputStream *file, IOOperationData *io_op, ReadOperation *op)
{
  gsize len;
  PreRead *pre;

  while (TRUE)
    {
      switch (op->state)
	{
	  /* Initial state for read op */
	case READ_STATE_INIT:

	  while (file->pre_reads)
	    {
	      pre = file->pre_reads->data;
	      if (file->seek_generation != pre->seek_generation)
		{
		  file->pre_reads = g_list_delete_link (file->pre_reads,
							file->pre_reads);
		  pre_read_free (pre);
		}
	      else
		{
		  len = MIN (op->buffer_size, pre->len);
		  memcpy (op->buffer, pre->data, len);
		  op->ret_val = len;
		  op->ret_error = NULL;

		  if (len < pre->len)
		    {
		      memmove (pre->data, pre->data + len, pre->len - len);
		      pre->len -= len;
		    }
		  else
		    {
		      file->pre_reads = g_list_delete_link (file->pre_reads,
							    file->pre_reads);
		      pre_read_free (pre);
		    }
		  
		  return STATE_OP_DONE;
		}
	    }
	  
	  
	  /* If we're already reading some data, but we didn't read all, just use that
	     and don't even send a request */
	  if (file->input_state == INPUT_STATE_IN_BLOCK &&
	      file->seek_generation == file->input_block_seek_generation)
	    {
	      op->state = READ_STATE_READ_BLOCK;
	      io_op->io_buffer = op->buffer;
	      io_op->io_size = MIN (op->buffer_size, file->input_block_size);
	      io_op->io_allow_cancel = TRUE; /* Allow cancel before we sent request */
	      return STATE_OP_READ;
	    }

	  append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_READ,
			  op->buffer_size, 0, 0, &op->seq_nr);
	  op->state = READ_STATE_WROTE_COMMAND;
	  io_op->io_buffer = file->output_buffer->str;
	  io_op->io_size = file->output_buffer->len;
	  io_op->io_allow_cancel = TRUE; /* Allow cancel before first byte of request sent */
	  return STATE_OP_WRITE;

	  /* wrote parts of output_buffer */
	case READ_STATE_WROTE_COMMAND:
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

	  op->state = READ_STATE_HANDLE_INPUT;
	  break;

	  /* No op */
	case READ_STATE_HANDLE_INPUT:
	  if (io_op->cancelled && !op->sent_cancel)
	    {
	      op->sent_cancel = TRUE;
	      append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL,
			      op->seq_nr, 0, 0, NULL);
	      op->state = READ_STATE_WROTE_COMMAND;
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }
	  
	  if (file->input_state == INPUT_STATE_IN_BLOCK)
	    {
	      op->state = READ_STATE_HANDLE_INPUT_BLOCK;
	      break;
	    }
	  else if (file->input_state == INPUT_STATE_IN_REPLY_HEADER)
	    {
	      op->state = READ_STATE_HANDLE_HEADER;
	      break;
	    }
	  g_assert_not_reached ();
	  break;

	  /* No op */
	case READ_STATE_HANDLE_INPUT_BLOCK:
	  g_assert (file->input_state == INPUT_STATE_IN_BLOCK);
	  
	  if (file->seek_generation ==
	      file->input_block_seek_generation)
	    {
	      op->state = READ_STATE_READ_BLOCK;
	      io_op->io_buffer = op->buffer;
	      io_op->io_size = MIN (op->buffer_size, file->input_block_size);
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_READ;
	    }
	  else
	    {
	      op->state = READ_STATE_SKIP_BLOCK;
	      io_op->io_buffer = NULL;
	      io_op->io_size = file->input_block_size;
	      io_op->io_allow_cancel = !op->sent_cancel;
	      return STATE_OP_SKIP;
	    }
	  break;

	  /* Read block data */
	case READ_STATE_SKIP_BLOCK:
	  if (io_op->io_cancelled)
	    {
	      op->state = READ_STATE_HANDLE_INPUT;
	      break;
	    }
	  
	  g_assert (io_op->io_res <= file->input_block_size);
	  file->input_block_size -= io_op->io_res;
	  
	  if (file->input_block_size == 0)
	    file->input_state = INPUT_STATE_IN_REPLY_HEADER;
	  
	  op->state = READ_STATE_HANDLE_INPUT;
	  break;
	  
	  /* read header data, (or manual io_len/res = 0) */
	case READ_STATE_HANDLE_HEADER:
	  if (io_op->io_cancelled)
	    {
	      op->state = READ_STATE_HANDLE_INPUT;
	      break;
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
	      io_op->io_allow_cancel = file->input_buffer->len == 0 && !op->sent_cancel;
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
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA)
	      {
		g_string_truncate (file->input_buffer, 0);
		file->input_state = INPUT_STATE_IN_BLOCK;
		file->input_block_size = reply.arg1;
		file->input_block_seek_generation = reply.arg2;
		op->state = READ_STATE_HANDLE_INPUT_BLOCK;
		break;
	      }
	    /* Ignore other reply types */
	  }

	  g_string_truncate (file->input_buffer, 0);
	  
	  /* This wasn't interesting, read next reply */
	  op->state = READ_STATE_HANDLE_HEADER;
	  break;

	  /* Read block data */
	case READ_STATE_READ_BLOCK:
	  if (io_op->io_cancelled)
	    {
	      op->ret_val = -1;
	      g_set_error_literal (&op->ret_error,
				   G_IO_ERROR,
				   G_IO_ERROR_CANCELLED,
				   _("Operation was cancelled"));
	      return STATE_OP_DONE;
	    }
	  
	  if (io_op->io_res > 0)
	    {
	      g_assert (io_op->io_res <= file->input_block_size);
	      file->input_block_size -= io_op->io_res;
	      if (file->input_block_size == 0)
		file->input_state = INPUT_STATE_IN_REPLY_HEADER;
	    }
	  
	  op->ret_val = io_op->io_res;
	  op->ret_error = NULL;
	  return STATE_OP_DONE;
	  
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
g_daemon_file_input_stream_read (GInputStream *stream,
				 void         *buffer,
				 gsize         count,
				 GCancellable *cancellable,
				 GError      **error)
{
  GDaemonFileInputStream *file;
  ReadOperation op;

  file = G_DAEMON_FILE_INPUT_STREAM (stream);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return -1;
  
  /* Limit for sanity and to avoid 32bit overflow */
  if (count > MAX_READ_SIZE)
    count = MAX_READ_SIZE;

  memset (&op, 0, sizeof (op));
  op.state = READ_STATE_INIT;
  op.buffer = buffer;
  op.buffer_size = count;
  
  if (!run_sync_state_machine (file, (state_machine_iterator)iterate_read_state_machine,
			       &op, cancellable, error))
    return -1; /* IO Error */

  if (op.ret_val == -1)
    g_propagate_error (error, op.ret_error);
  else
    file->current_offset += op.ret_val;
  
  return op.ret_val;
}

static gssize
g_daemon_file_input_stream_skip (GInputStream *stream,
				 gsize         count,
				 GCancellable *cancellable,
				 GError      **error)
{
#if 0
  GDaemonFileInputStream *file;

  file = G_DAEMON_FILE_INPUT_STREAM (stream);
#endif  
  /* TODO: implement skip */
  g_assert_not_reached ();
  
  return 0;
}

static StateOp
iterate_close_state_machine (GDaemonFileInputStream *file, IOOperationData *io_op, CloseOperation *op)
{
  gsize len;

  while (TRUE)
    {
      switch (op->state)
	{
	  /* Initial state for read op */
	case CLOSE_STATE_INIT:

	  /* Clear any pre-read data blocks */
	  while (file->pre_reads)
	    {
	      PreRead *pre = file->pre_reads->data;
	      file->pre_reads = g_list_delete_link (file->pre_reads,
						    file->pre_reads);
	      pre_read_free (pre);
	    }
	  
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
	  
	  if (file->input_state == INPUT_STATE_IN_BLOCK)
	    {
	      op->state = CLOSE_STATE_HANDLE_INPUT_BLOCK;
	      break;
	    }
	  else if (file->input_state == INPUT_STATE_IN_REPLY_HEADER)
	    {
	      op->state = CLOSE_STATE_HANDLE_HEADER;
	      break;
	    }
	  g_assert_not_reached ();
	  break;

	  /* No op */
	case CLOSE_STATE_HANDLE_INPUT_BLOCK:
	  g_assert (file->input_state == INPUT_STATE_IN_BLOCK);
	  
	  op->state = CLOSE_STATE_SKIP_BLOCK;
	  io_op->io_buffer = NULL;
	  io_op->io_size = file->input_block_size;
	  io_op->io_allow_cancel = !op->sent_cancel;
	  return STATE_OP_SKIP;

	  /* Read block data */
	case CLOSE_STATE_SKIP_BLOCK:
	  if (io_op->io_cancelled)
	    {
	      op->state = CLOSE_STATE_HANDLE_INPUT;
	      break;
	    }
	  
	  g_assert (io_op->io_res <= file->input_block_size);
	  file->input_block_size -= io_op->io_res;
	  
	  if (file->input_block_size == 0)
	    file->input_state = INPUT_STATE_IN_REPLY_HEADER;
	  
	  op->state = CLOSE_STATE_HANDLE_INPUT;
	  break;
	  
	  /* read header data, (or manual io_len/res = 0) */
	case CLOSE_STATE_HANDLE_HEADER:
	  if (io_op->io_cancelled)
	    {
	      op->state = CLOSE_STATE_HANDLE_INPUT;
	      break;
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
	      io_op->io_allow_cancel = file->input_buffer->len == 0 && !op->sent_cancel;
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
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA)
	      {
		g_string_truncate (file->input_buffer, 0);
		file->input_state = INPUT_STATE_IN_BLOCK;
		file->input_block_size = reply.arg1;
		file->input_block_seek_generation = reply.arg2;
		op->state = CLOSE_STATE_HANDLE_INPUT_BLOCK;
		break;
	      }
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_CLOSED &&
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
	  op->state = CLOSE_STATE_HANDLE_HEADER;
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
g_daemon_file_input_stream_close (GInputStream *stream,
				  GCancellable *cancellable,
				  GError      **error)
{
  GDaemonFileInputStream *file;
  CloseOperation op;
  gboolean res;

  file = G_DAEMON_FILE_INPUT_STREAM (stream);

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
g_daemon_file_input_stream_tell (GFileInputStream *stream)
{
  GDaemonFileInputStream *file;

  file = G_DAEMON_FILE_INPUT_STREAM (stream);
  
  return file->current_offset;
}

static gboolean
g_daemon_file_input_stream_can_seek (GFileInputStream *stream)
{
  GDaemonFileInputStream *file;

  file = G_DAEMON_FILE_INPUT_STREAM (stream);

  return file->can_seek;
}

static StateOp
iterate_seek_state_machine (GDaemonFileInputStream *file, IOOperationData *io_op, SeekOperation *op)
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
	  op->sent_seek = FALSE;
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

	  /* We weren't cancelled before first byte sent, so now we will send
	   * the seek request. Increase the seek generation now. */
	  if (!op->sent_seek)
	    file->seek_generation++;
	  op->sent_seek = TRUE;
	  
	  /* Clear any pre-read data blocks */
	  while (file->pre_reads)
	    {
	      PreRead *pre = file->pre_reads->data;
	      file->pre_reads = g_list_delete_link (file->pre_reads,
						    file->pre_reads);
	      pre_read_free (pre);
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
	  
	  if (file->input_state == INPUT_STATE_IN_BLOCK)
	    {
	      op->state = SEEK_STATE_HANDLE_INPUT_BLOCK;
	      break;
	    }
	  else if (file->input_state == INPUT_STATE_IN_REPLY_HEADER)
	    {
	      op->state = SEEK_STATE_HANDLE_HEADER;
	      break;
	    }
	  g_assert_not_reached ();
	  break;

	  /* No op */
	case SEEK_STATE_HANDLE_INPUT_BLOCK:
	  g_assert (file->input_state == INPUT_STATE_IN_BLOCK);
	  
	  op->state = SEEK_STATE_SKIP_BLOCK;
	  /* Reuse client buffer for skipping */
	  io_op->io_buffer = NULL;
	  io_op->io_size = file->input_block_size;
	  io_op->io_allow_cancel = !op->sent_cancel;
	  return STATE_OP_SKIP;

	  /* Read block data */
	case SEEK_STATE_SKIP_BLOCK:
	  if (io_op->io_cancelled)
	    {
	      op->state = SEEK_STATE_HANDLE_INPUT;
	      break;
	    }
	  
	  g_assert (io_op->io_res <= file->input_block_size);
	  file->input_block_size -= io_op->io_res;
	  
	  if (file->input_block_size == 0)
	    file->input_state = INPUT_STATE_IN_REPLY_HEADER;
	  
	  op->state = SEEK_STATE_HANDLE_INPUT;
	  break;
	  
	  /* read header data, (or manual io_len/res = 0) */
	case SEEK_STATE_HANDLE_HEADER:
	  if (io_op->io_cancelled)
	    {
	      op->state = SEEK_STATE_HANDLE_INPUT;
	      break;
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
	      io_op->io_allow_cancel = file->input_buffer->len == 0 && !op->sent_cancel;
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
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA)
	      {
		g_string_truncate (file->input_buffer, 0);
		file->input_state = INPUT_STATE_IN_BLOCK;
		file->input_block_size = reply.arg1;
		file->input_block_seek_generation = reply.arg2;
		op->state = SEEK_STATE_HANDLE_INPUT_BLOCK;
		break;
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
	  op->state = SEEK_STATE_HANDLE_HEADER;
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
g_daemon_file_input_stream_seek (GFileInputStream *stream,
				 goffset offset,
				 GSeekType type,
				 GCancellable *cancellable,
				 GError **error)
{
  GDaemonFileInputStream *file;
  SeekOperation op;

  file = G_DAEMON_FILE_INPUT_STREAM (stream);

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
iterate_query_state_machine (GDaemonFileInputStream *file,
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
	  
	  if (file->input_state == INPUT_STATE_IN_BLOCK)
	    {
	      op->state = QUERY_STATE_HANDLE_INPUT_BLOCK;
	      break;
	    }
	  else if (file->input_state == INPUT_STATE_IN_REPLY_HEADER)
	    {
	      op->state = QUERY_STATE_HANDLE_HEADER;
	      break;
	    }
	  g_assert_not_reached ();
	  break;

	  /* No op */
	case QUERY_STATE_HANDLE_INPUT_BLOCK:
	  g_assert (file->input_state == INPUT_STATE_IN_BLOCK);

	  if (file->input_block_size == 0)
	    {
	      file->input_state = INPUT_STATE_IN_REPLY_HEADER;
	      op->state = QUERY_STATE_HANDLE_INPUT;
	      break;
	    }
	  
	  if (file->seek_generation ==
	      file->input_block_seek_generation)
	    {
	      op->state = QUERY_STATE_READ_BLOCK;
	      io_op->io_buffer = g_malloc (file->input_block_size); 
	      io_op->io_size = file->input_block_size;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_READ;
	    }
	  else
	    {
	      op->state = QUERY_STATE_SKIP_BLOCK;
	      io_op->io_buffer = NULL;
	      io_op->io_size = file->input_block_size;
	      io_op->io_allow_cancel = !op->sent_cancel;
	      return STATE_OP_SKIP;
	    }
	  break;

	  /* Read block data */
	case QUERY_STATE_SKIP_BLOCK:
	  if (io_op->io_cancelled)
	    {
	      op->state = QUERY_STATE_HANDLE_INPUT;
	      break;
	    }
	  
	  g_assert (io_op->io_res <= file->input_block_size);
	  file->input_block_size -= io_op->io_res;
	  
	  if (file->input_block_size == 0)
	    file->input_state = INPUT_STATE_IN_REPLY_HEADER;
	  
	  op->state = QUERY_STATE_HANDLE_INPUT;
	  break;

	  /* Read block data */
	case QUERY_STATE_READ_BLOCK:
	  if (io_op->io_cancelled)
	    {
	      g_free (io_op->io_buffer);
	      op->state = QUERY_STATE_HANDLE_INPUT;
	      break;
	    }
	  
	  if (io_op->io_res > 0)
	    {
	      PreRead *pre;
	      
	      g_assert (io_op->io_res <= file->input_block_size);
	      file->input_block_size -= io_op->io_res;
	      if (file->input_block_size == 0)
		file->input_state = INPUT_STATE_IN_REPLY_HEADER;

	      pre = g_new (PreRead, 1);
	      pre->data = io_op->io_buffer;
	      pre->len = io_op->io_res;
	      pre->seek_generation = file->input_block_seek_generation;

	      file->pre_reads = g_list_append (file->pre_reads, pre);
	    }
	  else
	    g_free (io_op->io_buffer);
	  
	  op->state = QUERY_STATE_HANDLE_INPUT;
	  break;
	  
	  /* read header data, (or manual io_len/res = 0) */
	case QUERY_STATE_HANDLE_HEADER:
	  if (io_op->io_cancelled)
	    {
	      op->state = QUERY_STATE_HANDLE_INPUT;
	      break;
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
	      io_op->io_allow_cancel = file->input_buffer->len == 0 && !op->sent_cancel;
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
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA)
	      {
		g_string_truncate (file->input_buffer, 0);
		file->input_state = INPUT_STATE_IN_BLOCK;
		file->input_block_size = reply.arg1;
		file->input_block_seek_generation = reply.arg2;
		op->state = QUERY_STATE_HANDLE_INPUT_BLOCK;
		break;
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
	  op->state = QUERY_STATE_HANDLE_HEADER;
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
g_daemon_file_input_stream_query_info (GFileInputStream     *stream,
				       const char           *attributes,
				       GCancellable         *cancellable,
				       GError              **error)
{
   GDaemonFileInputStream *file;
   QueryOperation op;

  file = G_DAEMON_FILE_INPUT_STREAM (stream);

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
			gpointer user_data)
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
			gpointer user_data)
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
  GDaemonFileInputStream *file;
  GCancellable *cancellable = g_task_get_cancellable (iterator->task);
  StateOp io_op;

  io_data->cancelled = g_cancellable_is_cancelled (cancellable);

  file = G_DAEMON_FILE_INPUT_STREAM (g_task_get_source_object (iterator->task));
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
async_read_done (GTask *task)
{
  ReadOperation *op;
  gssize count_read;
  GError *error;

  op = g_task_get_task_data (task);

  count_read = op->ret_val;
  error = op->ret_error;

  if (count_read == -1)
    g_task_return_error (task, error);
  else
    g_task_return_int (task, count_read);

  g_object_unref (task);
}

static void
g_daemon_file_input_stream_read_async  (GInputStream        *stream,
					void               *buffer,
					gsize               count,
					int                 io_priority,
					GCancellable       *cancellable,
					GAsyncReadyCallback callback,
					gpointer            user_data)
{
  ReadOperation *op;
  GTask *task;

  task = g_task_new (stream, cancellable, callback, user_data);
  g_task_set_priority (task, io_priority);
  g_task_set_source_tag (task, g_daemon_file_input_stream_read_async);

  /* Limit for sanity and to avoid 32bit overflow */
  if (count > MAX_READ_SIZE)
    count = MAX_READ_SIZE;

  op = g_new0 (ReadOperation, 1);
  op->state = READ_STATE_INIT;
  op->buffer = buffer;
  op->buffer_size = count;

  g_task_set_task_data (task, op, g_free);

  run_async_state_machine (task,
			   (state_machine_iterator)iterate_read_state_machine,
			   async_read_done);
}

static gssize
g_daemon_file_input_stream_read_finish (GInputStream              *stream,
					GAsyncResult              *result,
					GError                   **error)
{
  g_return_val_if_fail (g_task_is_valid (result, stream), -1);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_daemon_file_input_stream_read_async), -1);

  return g_task_propagate_int (G_TASK (result), error);
}


static void
g_daemon_file_input_stream_skip_async  (GInputStream        *stream,
					gsize               count,
					int                 io_priority,
					GCancellable       *cancellable,
					GAsyncReadyCallback callback,
					gpointer            data)
{
  g_assert_not_reached ();
  /* TODO: Not implemented */
}

static gssize
g_daemon_file_input_stream_skip_finish  (GInputStream              *stream,
					 GAsyncResult              *result,
					 GError                   **error)
{
  g_assert_not_reached ();
  /* TODO: Not implemented */
}

static void
async_close_done (GTask *task)
{
  GDaemonFileInputStream *file;
  CloseOperation *op;
  gboolean result;
  GError *error;
  GCancellable *cancellable = g_task_get_cancellable (task);

  file = G_DAEMON_FILE_INPUT_STREAM (g_task_get_source_object (task));
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
g_daemon_file_input_stream_close_async (GInputStream       *stream,
					int                 io_priority,
					GCancellable       *cancellable,
					GAsyncReadyCallback callback,
					gpointer            data)
{
  CloseOperation *op;
  GTask *task;

  task = g_task_new (stream, cancellable, callback, data);
  g_task_set_priority (task, io_priority);
  g_task_set_source_tag (task, g_daemon_file_input_stream_close_async);

  op = g_new0 (CloseOperation, 1);
  op->state = CLOSE_STATE_INIT;

  g_task_set_task_data (task, op, g_free);

  run_async_state_machine (task,
			   (state_machine_iterator)iterate_close_state_machine,
			   async_close_done);
}

static gboolean
g_daemon_file_input_stream_close_finish (GInputStream              *stream,
					 GAsyncResult              *result,
					 GError                   **error)
{
  g_return_val_if_fail (g_task_is_valid (result, stream), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_daemon_file_input_stream_close_async), FALSE);

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
g_daemon_file_input_stream_query_info_async  (GFileInputStream     *stream,
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
  g_task_set_source_tag (task, g_daemon_file_input_stream_query_info_async);

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
g_daemon_file_input_stream_query_info_finish (GFileInputStream     *stream,
					      GAsyncResult         *result,
					      GError              **error)
{
  g_return_val_if_fail (g_task_is_valid (result, stream), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_daemon_file_input_stream_query_info_async), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
