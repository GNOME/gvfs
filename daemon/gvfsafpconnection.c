 /* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) Carl-Anton Ingmarsson 2011 <ca.ingmarsson@gmail.com>
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
 * Author: Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
 */

#include <stdlib.h>
#include <string.h>
#include <glib/gi18n.h>
#include <gio/gnetworking.h>


#include "gvfsafpconnection.h"

/*
 * GVfsAfpName
 */
struct _GVfsAfpName
{
  guint32 text_encoding;
  gchar *str;
  gsize len;

  gint ref_count;
};

static void
_g_vfs_afp_name_free (GVfsAfpName *afp_name)
{
  g_free (afp_name->str);
  g_slice_free (GVfsAfpName, afp_name);
}

void
g_vfs_afp_name_unref (GVfsAfpName *afp_name)
{
  if (g_atomic_int_dec_and_test (&afp_name->ref_count))
    _g_vfs_afp_name_free (afp_name);
}

void
g_vfs_afp_name_ref (GVfsAfpName *afp_name)
{
  g_atomic_int_inc (&afp_name->ref_count);
}

char *
g_vfs_afp_name_get_string (GVfsAfpName *afp_name)
{
  return g_utf8_normalize (afp_name->str, afp_name->len, G_NORMALIZE_DEFAULT_COMPOSE);
}

GVfsAfpName *
g_vfs_afp_name_new (guint32 text_encoding, gchar *str, gsize len)
{
  GVfsAfpName *afp_name;

  afp_name = g_slice_new (GVfsAfpName);
  afp_name->ref_count = 1;
  
  afp_name->text_encoding = text_encoding;

  afp_name->str = str;
  afp_name->len = len;

  return afp_name;
}

/*
 * GVfsAfpReply
 */
struct _GVfsAfpReplyClass
{
  GObjectClass parent_class;
};

struct _GVfsAfpReply
{
  GObject parent_instance;

  AfpResultCode result_code;

  char *data;
  gsize len;
  gboolean free_data;

  goffset pos;
};

G_DEFINE_TYPE (GVfsAfpReply, g_vfs_afp_reply, G_TYPE_OBJECT);

static void
g_vfs_afp_reply_init (GVfsAfpReply *reply)
{
  reply->data = NULL;
  reply->len = 0;
  reply->pos = 0;
}

static void
g_vfs_afp_reply_finalize (GObject *object)
{
  GVfsAfpReply *reply = (GVfsAfpReply *)object;

  if (reply->free_data)
    g_free (reply->data);
}

static void
g_vfs_afp_reply_class_init (GVfsAfpReplyClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = g_vfs_afp_reply_finalize;
}

static GVfsAfpReply *
g_vfs_afp_reply_new (AfpResultCode result_code, char *data, gsize len, gboolean take_data)
{
  GVfsAfpReply *reply;

  reply = g_object_new (G_VFS_TYPE_AFP_REPLY, NULL);

  reply->result_code = result_code;
  reply->len = len;
  reply->data = data;
  reply->free_data = take_data;
  
  return reply;
}

gboolean
g_vfs_afp_reply_read_byte (GVfsAfpReply *reply, guint8 *byte)
{
  if ((reply->len - reply->pos) < 1)
    return FALSE;

  if (byte)
    *byte = reply->data[reply->pos];

  reply->pos++;

  return TRUE;
}

gboolean
g_vfs_afp_reply_read_int64 (GVfsAfpReply *reply, gint64 *val)
{
  if ((reply->len - reply->pos) < 8)
    return FALSE;

  if (val)
    *val = GINT64_FROM_BE (*((gint64 *)(reply->data + reply->pos)));

  reply->pos += 8;
  
  return TRUE;
}

gboolean
g_vfs_afp_reply_read_int32 (GVfsAfpReply *reply, gint32 *val)
{
  if ((reply->len - reply->pos) < 4)
    return FALSE;

  if (val)
    *val = GINT32_FROM_BE (*((gint32 *)(reply->data + reply->pos)));

  reply->pos += 4;
  
  return TRUE;
}

gboolean
g_vfs_afp_reply_read_int16 (GVfsAfpReply *reply, gint16 *val)
{
  if ((reply->len - reply->pos) < 2)
    return FALSE;

  if (val)
    *val = GINT16_FROM_BE (*((gint16 *)(reply->data + reply->pos)));

  reply->pos += 2;
  
  return TRUE;
}

gboolean
g_vfs_afp_reply_read_uint64 (GVfsAfpReply *reply, guint64 *val)
{
  if ((reply->len - reply->pos) < 8)
    return FALSE;

  if (val)
    *val = GUINT64_FROM_BE (*((guint64 *)(reply->data + reply->pos)));

  reply->pos += 8;
  
  return TRUE;
}

gboolean
g_vfs_afp_reply_read_uint32 (GVfsAfpReply *reply, guint32 *val)
{
  if ((reply->len - reply->pos) < 4)
    return FALSE;

  if (val)
    *val = GUINT32_FROM_BE (*((guint32 *)(reply->data + reply->pos)));

  reply->pos += 4;
  
  return TRUE;
}

gboolean
g_vfs_afp_reply_read_uint16 (GVfsAfpReply *reply, guint16 *val)
{
  if ((reply->len - reply->pos) < 2)
    return FALSE;

  if (val)
    *val = GUINT16_FROM_BE (*((guint16 *)(reply->data + reply->pos)));

  reply->pos += 2;
  
  return TRUE;
}

gboolean
g_vfs_afp_reply_get_data (GVfsAfpReply *reply, gsize size, guint8 **data)
{
  if ((reply->len - reply->pos) < size)
    return FALSE;

  if (data)
    *data = (guint8 *)(reply->data + reply->pos);

  reply->pos += size;

  return TRUE;
}

gboolean
g_vfs_afp_reply_dup_data (GVfsAfpReply *reply, gsize size, guint8 **data)
{
  if ((reply->len - reply->pos) < size)
    return FALSE;

  if (data)
  {
    *data = g_malloc (size);
    memcpy (*data, reply->data + reply->pos, size);
  }

  reply->pos += size;

  return TRUE;
}

gboolean
g_vfs_afp_reply_read_pascal (GVfsAfpReply *reply, gboolean is_utf8, char **str)
{
  guint8 strsize;
  
  if (!g_vfs_afp_reply_read_byte (reply, &strsize))
    return FALSE;

  if (strsize > (reply->len - reply->pos))
  {
    reply->pos--;
    return FALSE;
  }

  if (str)
  {
    if (is_utf8)
    {
      char *tmp;

      if (!g_vfs_afp_reply_get_data (reply, strsize, (guint8 **)&tmp))
      {
        reply->pos--;
        return FALSE;
      }

      *str = g_utf8_normalize (tmp, strsize, G_NORMALIZE_DEFAULT_COMPOSE);
    }
    else
    {
      *str = g_convert (reply->data + reply->pos, strsize,
                        "UTF-8", "MACINTOSH", NULL, NULL, NULL);
      reply->pos += strsize;
    }
  }
  else
  {
      reply->pos += strsize;
  }

  return TRUE;
}

gboolean
g_vfs_afp_reply_read_afp_name (GVfsAfpReply *reply, gboolean read_text_encoding,
                               GVfsAfpName **afp_name)
{
  gint old_pos;
  
  guint32 text_encoding;
  guint16 len;
  gchar *str;

  old_pos = reply->pos;
  
  if (read_text_encoding)
  {
    if (!g_vfs_afp_reply_read_uint32 (reply, &text_encoding))
      return FALSE;
  }
  else
    text_encoding = 0;
  
  if (!g_vfs_afp_reply_read_uint16 (reply, &len))
  {
    reply->pos = old_pos;
    return FALSE;
  }  
  
  if (!g_vfs_afp_reply_get_data (reply, len, (guint8 **)&str))
  {
    reply->pos = old_pos;
    return FALSE;
  }

  if (afp_name)
    *afp_name = g_vfs_afp_name_new (text_encoding, g_strndup (str, len), len);

  return TRUE;
    
}

gboolean
g_vfs_afp_reply_seek (GVfsAfpReply *reply, goffset offset, GSeekType type)
{
  goffset absolute;
  
  switch (type)
  {
    case G_SEEK_CUR:
      absolute = reply->pos + offset;
      break;

    case G_SEEK_SET:
      absolute = offset;
      break;
      
    case G_SEEK_END:
      absolute = reply->len + offset;
      break;

    default:
      return FALSE;
  }

  if (absolute < 0 || absolute >= reply->len)
    return FALSE;

  reply->pos = absolute;
  return TRUE;
}

gboolean
g_vfs_afp_reply_skip_to_even (GVfsAfpReply *reply)
{
  if ((reply->pos % 2) == 0)
    return TRUE;

  if ((reply->len - reply->pos) < 1)
    return FALSE;

  reply->pos++;

  return TRUE;
}

goffset
g_vfs_afp_reply_get_pos (GVfsAfpReply *reply)
{
  return reply->pos;
}

gsize
g_vfs_afp_reply_get_size (GVfsAfpReply *reply)
{
  return reply->len;
}

AfpResultCode
g_vfs_afp_reply_get_result_code (GVfsAfpReply *reply)
{
  return reply->result_code;
}

/*
 * GVfsAfpCommand
 */
struct _GVfsAfpCommandClass
{
  GDataOutputStreamClass parent_class;
};

struct _GVfsAfpCommand
{
  GDataOutputStream parent_instance;
  
  AfpCommandType type;

  char *buf;
  gsize buf_size;
};

G_DEFINE_TYPE (GVfsAfpCommand, g_vfs_afp_command, G_TYPE_DATA_OUTPUT_STREAM);


static void
g_vfs_afp_command_init (GVfsAfpCommand *comm)
{
}

static void
g_vfs_afp_command_class_init (GVfsAfpCommandClass *klass)
{
}

GVfsAfpCommand *
g_vfs_afp_command_new (AfpCommandType type)
{
  GOutputStream *mem_stream;
  GVfsAfpCommand *comm;

  mem_stream = g_memory_output_stream_new (NULL, 0, g_realloc, g_free);
  comm = g_object_new (G_VFS_TYPE_AFP_COMMAND,
                       "base-stream", mem_stream,
                       NULL);

  g_object_unref (mem_stream);
  
  comm->type = type;
  g_vfs_afp_command_put_byte (comm, type);

  return comm;
}

void
g_vfs_afp_command_put_byte (GVfsAfpCommand *comm, guint8 byte)
{
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), byte, NULL, NULL);
}

void
g_vfs_afp_command_put_int16 (GVfsAfpCommand *comm, gint16 val)
{
  g_data_output_stream_put_int16 (G_DATA_OUTPUT_STREAM (comm), val, NULL, NULL);
}

void
g_vfs_afp_command_put_int32 (GVfsAfpCommand *comm, gint32 val)
{
  g_data_output_stream_put_int32 (G_DATA_OUTPUT_STREAM (comm), val, NULL, NULL);
}

void
g_vfs_afp_command_put_int64 (GVfsAfpCommand *comm, gint64 val)
{
  g_data_output_stream_put_int64 (G_DATA_OUTPUT_STREAM (comm), val, NULL, NULL);
}

void
g_vfs_afp_command_put_uint16 (GVfsAfpCommand *comm, guint16 val)
{
  g_data_output_stream_put_uint16 (G_DATA_OUTPUT_STREAM (comm), val, NULL, NULL);
}

void
g_vfs_afp_command_put_uint32 (GVfsAfpCommand *comm, guint32 val)
{
  g_data_output_stream_put_uint32 (G_DATA_OUTPUT_STREAM (comm), val, NULL, NULL);
}

void
g_vfs_afp_command_put_uint64 (GVfsAfpCommand *comm, guint64 val)
{
  g_data_output_stream_put_uint64 (G_DATA_OUTPUT_STREAM (comm), val, NULL, NULL);
}

void
g_vfs_afp_command_put_pascal (GVfsAfpCommand *comm, const char *str)
{
  size_t len;

  if (str == NULL)
  {
    g_vfs_afp_command_put_byte (comm, 0);
    return;
  }

  len = MIN (strlen (str), 256);

  g_vfs_afp_command_put_byte (comm, len);
  g_output_stream_write (G_OUTPUT_STREAM (comm), str, len, NULL, NULL);
}

void
g_vfs_afp_command_put_afp_name (GVfsAfpCommand *comm, GVfsAfpName *afp_name)
{
  g_vfs_afp_command_put_uint32 (comm, afp_name->text_encoding);
  g_vfs_afp_command_put_uint16 (comm, afp_name->len);

  if (afp_name->len > 0)
  {
    g_output_stream_write_all (G_OUTPUT_STREAM (comm), afp_name->str,
                               afp_name->len, NULL, NULL, NULL);
  }
}

static GVfsAfpName *
filename_to_afp_pathname (const char *filename)
{
  gsize len;
  char *str;
  gint i;

  while (*filename == '/')
    filename++;
  
  len = strlen (filename);
  
  str = g_malloc (len); 

  for (i = 0; i < len; i++)
  {
    if (filename[i] == '/')
      str[i] = '\0';
    else
      str[i] = filename[i];
  }
  

  return g_vfs_afp_name_new (0x08000103, str, len);
}

void
g_vfs_afp_command_put_pathname (GVfsAfpCommand *comm, const char *filename)
{
  GVfsAfpName *pathname;
  
  /* PathType */
  g_vfs_afp_command_put_byte (comm, AFP_PATH_TYPE_UTF8_NAME);

  /* Pathname */
  pathname = filename_to_afp_pathname (filename);
  g_vfs_afp_command_put_afp_name (comm, pathname);
  g_vfs_afp_name_unref (pathname);
}

void
g_vfs_afp_command_pad_to_even (GVfsAfpCommand *comm)
{ 
  if (g_vfs_afp_command_get_size (comm) % 2 == 1)
    g_vfs_afp_command_put_byte (comm, 0);
}

gsize
g_vfs_afp_command_get_size (GVfsAfpCommand *comm)
{
  GMemoryOutputStream *mem_stream;

  mem_stream =
    G_MEMORY_OUTPUT_STREAM (g_filter_output_stream_get_base_stream (G_FILTER_OUTPUT_STREAM (comm)));

  return g_memory_output_stream_get_data_size (mem_stream);
}

char *
g_vfs_afp_command_get_data (GVfsAfpCommand *comm)
{
  GMemoryOutputStream *mem_stream;

  mem_stream =
    G_MEMORY_OUTPUT_STREAM (g_filter_output_stream_get_base_stream (G_FILTER_OUTPUT_STREAM (comm)));

  return g_memory_output_stream_get_data (mem_stream);
}

void
g_vfs_afp_command_set_buffer (GVfsAfpCommand *comm, char *buf, gsize size)
{
  g_return_if_fail (buf != NULL);
  g_return_if_fail (size > 0);

  comm->buf = buf;
  comm->buf_size = size;
}

/*
 * GVfsAfpConnection
 */

enum {
  ATTENTION,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0,};

typedef struct {
  guint8 flags;
  guint8 command;
  guint16 requestID;
  union {
    guint32 errorCode;
    guint32 writeOffset;
  };
  guint32 totalDataLength;
  guint32 reserved;
} DSIHeader;

enum {
  STATE_INITIAL,
  STATE_CONNECTED,
  STATE_PENDING_CLOSE,
  STATE_CLOSED
};

struct _GVfsAfpConnectionPrivate
{
  GSocketConnectable *addr;
  GIOStream *stream;

  /* State */
  volatile gint atomic_state;
  
  guint16 request_id;
  guint16 tickle_id;

  guint32 kRequestQuanta;
  guint32 kServerReplayCacheSize;

  GThread      *worker_thread;
  GMainContext *worker_context;
  GMainLoop    *worker_loop;
  GMutex        mutex;
  
  GQueue     *request_queue;
  GHashTable *request_hash;

  /* send loop */
  gboolean send_loop_running;
  DSIHeader write_dsi_header;

  /* read loop */
  GCancellable *read_cancellable;
  DSIHeader read_dsi_header;
  char *reply_buf;
  gboolean free_reply_buf;

  GSList *pending_closes;
};

G_DEFINE_TYPE_WITH_PRIVATE (GVfsAfpConnection, g_vfs_afp_connection, G_TYPE_OBJECT);

typedef enum
{
  DSI_CLOSE_SESSION = 1,
  DSI_COMMAND       = 2,
  DSI_GET_STATUS    = 3,
  DSI_OPEN_SESSION  = 4,
  DSI_TICKLE        = 5,
  DSI_WRITE         = 6,
  DSI_ATTENTION     = 8
} DsiCommand;

typedef enum
{
  REQUEST_TYPE_COMMAND,
  REQUEST_TYPE_TICKLE
} RequestType;

typedef struct
{
  RequestType type;
  
  GVfsAfpCommand *command;
  char           *reply_buf;
  GTask *task;

  GVfsAfpConnection *conn;
} RequestData;

typedef struct
{
  GMutex mutex;
  GCond  cond;
  GVfsAfpConnection *conn;
  GCancellable *cancellable;
  gboolean res;
  GError **error;
  void *data;
} SyncData;

static void
sync_data_init (SyncData *data, GVfsAfpConnection *conn, GError **error)
{
  g_mutex_init (&data->mutex);
  g_cond_init (&data->cond);
  data->conn = conn;
  data->error = error;
  data->res = FALSE;
}

static void
sync_data_clear (SyncData *data)
{
  g_mutex_clear (&data->mutex);
  g_cond_clear (&data->cond);
}

static void
sync_data_signal (SyncData *data)
{
  g_mutex_lock (&data->mutex);
  g_cond_signal (&data->cond);
  g_mutex_unlock (&data->mutex);
}

static void
sync_data_wait (SyncData *data)
{
  g_mutex_lock (&data->mutex);
  g_cond_wait (&data->cond, &data->mutex);
  g_mutex_unlock (&data->mutex);
}

static void send_request_unlocked (GVfsAfpConnection *afp_connection);
static void close_connection (GVfsAfpConnection *conn);
static void read_reply (GVfsAfpConnection *afp_connection);

static void
free_request_data (RequestData *req_data)
{
  if (req_data->command)
    g_object_unref (req_data->command);
  if (req_data->task)
    g_object_unref (req_data->task);

  g_slice_free (RequestData, req_data);
}

static gboolean
check_open (GVfsAfpConnection *conn, GError **error)
{
  GVfsAfpConnectionPrivate *priv = conn->priv;
  
  /* Acts as memory barrier */
  gint state = g_atomic_int_get (&priv->atomic_state);

  if (state == STATE_INITIAL)
  {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                         _("The connection is not opened"));
    return FALSE;
  }

  else if (state == STATE_CLOSED)
  {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                         _("The connection is closed"));
    return FALSE;
  }

  return TRUE;
}

static void
g_vfs_afp_connection_init (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv;
  
  afp_connection->priv = priv = g_vfs_afp_connection_get_instance_private (afp_connection);
  priv->kRequestQuanta = -1;
  priv->kServerReplayCacheSize = -1;

  g_mutex_init (&priv->mutex);

  priv->request_queue = g_queue_new ();
  priv->request_hash = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                              NULL, (GDestroyNotify)free_request_data);
  priv->read_cancellable = g_cancellable_new ();

  priv->send_loop_running = FALSE;
}

static void
g_vfs_afp_connection_finalize (GObject *object)
{
  GVfsAfpConnection *afp_connection = (GVfsAfpConnection *)object;
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  g_clear_object (&priv->addr);
  g_clear_object (&priv->stream);
  g_clear_object (&priv->read_cancellable);

  g_mutex_clear (&priv->mutex);

  G_OBJECT_CLASS (g_vfs_afp_connection_parent_class)->finalize (object);
}

static void
g_vfs_afp_connection_class_init (GVfsAfpConnectionClass *klass)
{
  GObjectClass* object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = g_vfs_afp_connection_finalize;

  signals[ATTENTION] =
    g_signal_new ("attention",
                  G_TYPE_FROM_CLASS (object_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
                  0, NULL, NULL, g_cclosure_marshal_VOID__UINT,
                  G_TYPE_NONE, 1, G_TYPE_UINT);
}

static guint16
get_request_id (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  return priv->request_id++;
}

static guint16
get_tickle_id (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  return priv->tickle_id++;
}

typedef struct
{
  void         *buffer;
  gsize         count;
  gsize         bytes_read;
} ReadAllData;

static void
free_read_all_data (ReadAllData *read_data)
{
  g_slice_free (ReadAllData, read_data);
}

static void
read_all_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GInputStream *stream = G_INPUT_STREAM (source_object);
  GTask *task = G_TASK (user_data);
  gssize bytes_read;
  GError *err = NULL;
  ReadAllData *read_data = g_task_get_task_data (task);

  bytes_read = g_input_stream_read_finish (stream, res, &err);
  if (bytes_read == -1)
  {
    g_task_return_error (task, err);
    g_object_unref (task);
    return;
  }
  else if (bytes_read == 0)
  {
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CLOSED, _("Got EOS"));
    g_object_unref (task);
    return;
  }

  read_data->bytes_read += bytes_read;
  if (read_data->bytes_read < read_data->count)
  {
    g_input_stream_read_async (stream,
                               (guint8 *)read_data->buffer + read_data->bytes_read,
                               read_data->count - read_data->bytes_read,
                               g_task_get_priority (task), g_task_get_cancellable (task),
                               read_all_cb, task);
    return;
  }

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

static void
read_all_async (GInputStream        *stream,
                void                *buffer,
                gsize                count,
                int                  io_priority,
                GCancellable        *cancellable,
                GAsyncReadyCallback  callback,
                gpointer             user_data)
{
  ReadAllData *read_data;
  GTask *task;

  task = g_task_new (stream, cancellable, callback, user_data);
  g_task_set_source_tag (task, read_all_async);
  g_task_set_priority (task, io_priority);

  read_data = g_slice_new0 (ReadAllData);
  read_data->buffer = buffer;
  read_data->count = count;

  g_task_set_task_data (task, read_data, (GDestroyNotify)free_read_all_data);

  g_input_stream_read_async (stream, buffer, count, io_priority, cancellable,
                             read_all_cb, task);
}

static gboolean
read_all_finish (GInputStream *stream,
                 GAsyncResult *res,
                 gsize        *bytes_read,
                 GError      **error)
{
  g_return_val_if_fail (g_task_is_valid (res, stream), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (res, read_all_async), FALSE);

  if (bytes_read)
  {
    ReadAllData *read_data;

    read_data = g_task_get_task_data (G_TASK (res));
    *bytes_read = read_data->bytes_read;
  }

  return g_task_propagate_boolean (G_TASK (res), error);
}

static void
dispatch_reply (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;
  DSIHeader *dsi_header = &priv->read_dsi_header;
  
  switch (dsi_header->command)
  {
    case DSI_CLOSE_SESSION:
    {
      g_warning ("Server closed session\n");
      break;
    }

    case DSI_TICKLE:
    {
      RequestData *req_data;

      /* Send back a tickle message */
      req_data = g_slice_new0 (RequestData);
      req_data->type = REQUEST_TYPE_TICKLE;
      req_data->conn = afp_connection;

      /* take lock */
      g_mutex_lock (&priv->mutex);
      g_queue_push_head (priv->request_queue, req_data);
      if (!priv->send_loop_running) {
        priv->send_loop_running = TRUE;
        send_request_unlocked (afp_connection);
      }
      /* release lock */
      g_mutex_unlock (&priv->mutex);

      break;
    }

    case DSI_ATTENTION:
    {
      guint8 attention_code;

      attention_code = priv->reply_buf[0] >> 4;

      g_signal_emit (afp_connection, signals[ATTENTION], 0, attention_code);
      break;
    }

    case DSI_COMMAND:
    case DSI_WRITE:
    {
      RequestData *req_data;
      
      req_data = g_hash_table_lookup (priv->request_hash,
                                      GUINT_TO_POINTER ((guint)dsi_header->requestID));
      if (req_data)
      {
        GVfsAfpReply *reply;

        reply = g_vfs_afp_reply_new (dsi_header->errorCode, priv->reply_buf,
                                     dsi_header->totalDataLength, priv->free_reply_buf);
        priv->free_reply_buf = FALSE;

        g_task_return_pointer (req_data->task, reply, g_object_unref);

        g_hash_table_remove (priv->request_hash,
                             GUINT_TO_POINTER ((guint)dsi_header->requestID));
      }
      break;
    }

    default:
      g_assert_not_reached ();
  }
}
    
static void
read_data_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
  GInputStream *input = G_INPUT_STREAM (object);
  GVfsAfpConnection *afp_connection = G_VFS_AFP_CONNECTION (user_data);
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  gboolean result;
  GError *err = NULL;

  if (g_atomic_int_get (&priv->atomic_state) == STATE_PENDING_CLOSE)
  {
    if (!priv->send_loop_running)
      close_connection (afp_connection);
    return;
  }
  
  result = read_all_finish (input, res, NULL, &err);
  if (!result)
  {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CLOSED) ||
        g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED))
    {
      g_message (_("Host closed connection"));
    }
    else
    {
      g_warning ("FAIL!!! \"%s\"\n", err->message);
    }
    exit (0);
  }

  dispatch_reply (afp_connection);

  if (priv->free_reply_buf)
    g_free (priv->reply_buf);
  priv->reply_buf = NULL;
  
  read_reply (afp_connection);
}

static void
read_dsi_header_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
  GInputStream *input = G_INPUT_STREAM (object);
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (user_data);
  GVfsAfpConnectionPrivate *priv = afp_conn->priv;
  
  gboolean result;
  GError *err = NULL;
  DSIHeader *dsi_header;

  if (g_atomic_int_get (&priv->atomic_state) == STATE_PENDING_CLOSE)
  {
    if (!priv->send_loop_running)
      close_connection (afp_conn);
    return;
  }
  
  result = read_all_finish (input, res, NULL, &err);
  if (!result)
  {
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CLOSED) ||
        g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED))
    {
      g_message (_("Host closed connection"));
    }
    else
    {
      g_warning ("FAIL!!! \"%s\"\n", err->message);
    }
    exit (0);
  }

  dsi_header = &priv->read_dsi_header;
  
  dsi_header->requestID = GUINT16_FROM_BE (dsi_header->requestID);
  dsi_header->errorCode = GUINT32_FROM_BE (dsi_header->errorCode);
  dsi_header->totalDataLength = GUINT32_FROM_BE (dsi_header->totalDataLength);
  
  if (dsi_header->totalDataLength > 0)
  {
    RequestData *req_data;

    req_data = g_hash_table_lookup (priv->request_hash,
                                    GUINT_TO_POINTER ((guint)dsi_header->requestID));
    if (req_data && req_data->reply_buf)
    {
        priv->reply_buf = req_data->reply_buf;
        priv->free_reply_buf = FALSE;
    }
    else
    {
      priv->reply_buf = g_malloc (dsi_header->totalDataLength);
      priv->free_reply_buf = TRUE;
    }
    
    read_all_async (input, priv->reply_buf, dsi_header->totalDataLength,
                    0, priv->read_cancellable, read_data_cb, afp_conn);
    
    return;
  }

  dispatch_reply (afp_conn);
  read_reply (afp_conn);
}

static void
read_reply (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;
  
  GInputStream *input;

  if (g_atomic_int_get (&priv->atomic_state) & STATE_PENDING_CLOSE)
  {
    if (!priv->send_loop_running)
      close_connection (afp_connection);
    return;
  }
  
  input = g_io_stream_get_input_stream (priv->stream);
  
  read_all_async (input, &priv->read_dsi_header, sizeof (DSIHeader), 0,
                  priv->read_cancellable, read_dsi_header_cb, afp_connection);
}

typedef struct
{
  const void *buffer;
  gsize count;
  gsize bytes_written;
} WriteAllData;

inline static void
free_write_all_data (WriteAllData *write_data)
{
  g_slice_free (WriteAllData, write_data);
}

static void
write_all_cb (GObject      *source_object,
              GAsyncResult *res,
              gpointer      user_data)
{
  GOutputStream *stream = G_OUTPUT_STREAM (source_object);
  GTask *task = G_TASK (user_data);
  gssize bytes_written;
  GError *err = NULL;
  WriteAllData *write_data = g_task_get_task_data (task);

  bytes_written = g_output_stream_write_finish (stream, res, &err);
  if (bytes_written == -1)
  {
    g_task_return_error (task, err);
    g_object_unref (task);
    return;
  }

  write_data->bytes_written += bytes_written;
  if (write_data->bytes_written < write_data->count)
  {
    g_output_stream_write_async (stream,
                                 (const guint8 *)write_data->buffer + write_data->bytes_written,
                                 write_data->count - write_data->bytes_written,
                                 g_task_get_priority (task), g_task_get_cancellable (task),
                                 write_all_cb, task);
    return;
  }

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

static void
write_all_async (GOutputStream      *stream,
                 const void         *buffer,
                 gsize               count,
                 int                 io_priority,
                 GCancellable       *cancellable,
                 GAsyncReadyCallback callback,
                 gpointer            user_data)
{
  GTask *task;
  WriteAllData *write_data;

  task = g_task_new (stream, cancellable, callback, user_data);
  g_task_set_source_tag (task, write_all_async);
  g_task_set_priority (task, io_priority);

  write_data = g_slice_new0 (WriteAllData);
  write_data->buffer = buffer;
  write_data->count = count;

  g_task_set_task_data (task, write_data, (GDestroyNotify)free_write_all_data);

  g_output_stream_write_async (stream, buffer, count, io_priority, cancellable,
                               write_all_cb, task);
}

static gboolean
write_all_finish (GOutputStream *stream,
                  GAsyncResult  *res,
                  gsize         *bytes_written,
                  GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (res, stream), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (res, write_all_async), FALSE);

  if (bytes_written)
  {
    WriteAllData *write_data;

    write_data = g_task_get_task_data (G_TASK (res));
    *bytes_written = write_data->bytes_written;
  }

  return g_task_propagate_boolean (G_TASK (res), error);
}

#define HANDLE_RES() { \
  gboolean result; \
  GError *err = NULL; \
\
 result = write_all_finish (output, res, NULL, &err); \
 if (!result) \
  { \
    if (req_data->task) \
      g_task_return_error (req_data->task, err); \
    else \
      g_error_free (err); \
\
    g_hash_table_remove (priv->request_hash, \
                         GUINT_TO_POINTER ((guint)GUINT16_FROM_BE (priv->write_dsi_header.requestID))); \
    free_request_data (req_data); \
\
    g_mutex_lock (&priv->mutex); \
    send_request_unlocked (afp_conn); \
    g_mutex_unlock (&priv->mutex); \
    return; \
  } \
}

static void
write_buf_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
  GOutputStream *output = G_OUTPUT_STREAM (object);
  RequestData *req_data = user_data;
  GVfsAfpConnection *afp_conn = req_data->conn;
  GVfsAfpConnectionPrivate *priv = afp_conn->priv;
  
  HANDLE_RES ();

  g_mutex_lock (&priv->mutex);
  send_request_unlocked (afp_conn);
  g_mutex_unlock (&priv->mutex);
}

static void
write_command_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
  GOutputStream *output = G_OUTPUT_STREAM (object);
  RequestData *req_data = user_data;
  GVfsAfpConnection *afp_conn = req_data->conn;
  GVfsAfpConnectionPrivate *priv = afp_conn->priv;

  HANDLE_RES ();

  if (priv->write_dsi_header.command == DSI_WRITE &&
     req_data->command->buf)
  {
    write_all_async (output, req_data->command->buf, req_data->command->buf_size,
                     0, NULL, write_buf_cb, req_data);
    return;
  }
  
  g_mutex_lock (&priv->mutex);
  send_request_unlocked (afp_conn);
  g_mutex_unlock (&priv->mutex);
}

static void
write_dsi_header_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
  GOutputStream *output = G_OUTPUT_STREAM (object);
  RequestData *req_data = user_data;
  GVfsAfpConnection *afp_conn = req_data->conn;
  GVfsAfpConnectionPrivate *priv = afp_conn->priv;
  
  char *data;
  gsize size;

  HANDLE_RES ();

  if (req_data->type == REQUEST_TYPE_TICKLE)
  {
    g_mutex_lock (&priv->mutex);
    send_request_unlocked (afp_conn);
    g_mutex_unlock (&priv->mutex);
    return;
  }

  data = g_vfs_afp_command_get_data (req_data->command);
  size = g_vfs_afp_command_get_size (req_data->command);

  write_all_async (output, data, size, 0, NULL, write_command_cb, req_data);
}

static void
send_request_unlocked (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  RequestData *req_data;
  guint32 writeOffset;
  guint8 dsi_command;

  while ((req_data = g_queue_pop_head (priv->request_queue)))
  {
    if (!req_data->task || !g_task_return_error_if_cancelled (req_data->task))
      break;
  }

  if (!req_data) {
    priv->send_loop_running = FALSE;
    return;
  }

  switch (req_data->type)
  {
    case REQUEST_TYPE_TICKLE:
      priv->write_dsi_header.flags = 0x00;
      priv->write_dsi_header.command = DSI_TICKLE;
      priv->write_dsi_header.requestID = GUINT16_TO_BE (get_tickle_id (afp_connection));
      priv->write_dsi_header.writeOffset = 0;
      priv->write_dsi_header.totalDataLength = 0;
      priv->write_dsi_header.reserved = 0;
      break;

    case REQUEST_TYPE_COMMAND:
    {
      gsize size;
      
      switch (req_data->command->type)
      {
        case AFP_COMMAND_WRITE:
          writeOffset = 8;
          dsi_command = DSI_WRITE;
          break;
        case AFP_COMMAND_WRITE_EXT:
          writeOffset = 20;
          dsi_command = DSI_WRITE;
          break;

        default:
          writeOffset = 0;
          dsi_command = DSI_COMMAND;
          break;
      }

      priv->write_dsi_header.flags = 0x00;
      priv->write_dsi_header.command = dsi_command;
      priv->write_dsi_header.requestID = GUINT16_TO_BE (get_request_id (afp_connection));
      priv->write_dsi_header.writeOffset = GUINT32_TO_BE (writeOffset);

      /* totalDataLength */
      size = g_vfs_afp_command_get_size (req_data->command);
      if (dsi_command == DSI_WRITE && req_data->command->buf)
        size += req_data->command->buf_size;
      priv->write_dsi_header.totalDataLength = GUINT32_TO_BE (size);
      
      priv->write_dsi_header.reserved = 0;
      break;
    }

    default:
      g_assert_not_reached ();
  }

  if (req_data->type != REQUEST_TYPE_TICKLE)
    g_hash_table_insert (priv->request_hash,
                         GUINT_TO_POINTER ((guint)GUINT16_FROM_BE (priv->write_dsi_header.requestID)),
                         req_data);

  write_all_async (g_io_stream_get_output_stream (priv->stream),
                   &priv->write_dsi_header, sizeof (DSIHeader), 0,
                   NULL, write_dsi_header_cb, req_data);
}

static gboolean
start_send_loop_func (gpointer data)
{
  GVfsAfpConnection *conn = data;
  GVfsAfpConnectionPrivate *priv = conn->priv;

  g_mutex_lock (&priv->mutex);
  
  if (priv->send_loop_running)
    goto out;

  priv->send_loop_running = TRUE;
  send_request_unlocked (conn);

out:
  g_mutex_unlock (&priv->mutex);
  return G_SOURCE_REMOVE;
}

void
g_vfs_afp_connection_send_command (GVfsAfpConnection   *afp_connection,
                                   GVfsAfpCommand      *command,
                                   char                *reply_buf,
                                   GAsyncReadyCallback  callback,
                                   GCancellable        *cancellable,
                                   gpointer             user_data)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;
  GTask *task;
  GError *err = NULL;
  RequestData *req_data;

  task = g_task_new (afp_connection, cancellable, callback, user_data);
  g_task_set_source_tag (task, g_vfs_afp_connection_send_command);

  if (!check_open (afp_connection, &err))
  {
    g_task_return_error (task, err);
    g_object_unref (task);
    return;
  }
  
  req_data = g_slice_new0 (RequestData);
  req_data->type = REQUEST_TYPE_COMMAND;
  req_data->command = g_object_ref (command);
  req_data->reply_buf = reply_buf;
  req_data->conn = afp_connection;
  req_data->task = task;

  /* Take lock */
  g_mutex_lock (&priv->mutex);
  
  g_queue_push_tail (priv->request_queue, req_data);
  if (!priv->send_loop_running)
  {
    g_main_context_invoke (priv->worker_context, start_send_loop_func,
                           afp_connection);
  }

  /* Release lock */
  g_mutex_unlock (&priv->mutex);
}

GVfsAfpReply *
g_vfs_afp_connection_send_command_finish (GVfsAfpConnection *afp_connection,
                                          GAsyncResult *res,
                                          GError **error)
{
  g_return_val_if_fail (g_task_is_valid (res, afp_connection), NULL);
  g_return_val_if_fail (g_async_result_is_tagged (res, g_vfs_afp_connection_send_command), NULL);

  return g_task_propagate_pointer (G_TASK (res), error);
}

static gboolean
read_reply_sync (GInputStream      *input,
                 DSIHeader         *dsi_header,
                 char              **data,
                 GCancellable      *cancellable,
                 GError            **error)
{
  gboolean res;
  gsize read_count, bytes_read;

  g_assert (dsi_header != NULL);

  read_count = sizeof (DSIHeader);
  res = g_input_stream_read_all (input, dsi_header, read_count, &bytes_read,
                                 cancellable, error);
  if (!res)
    return FALSE;

  if (bytes_read < read_count)
  {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         _("Connection unexpectedly went down"));
    return FALSE;
  }

  dsi_header->requestID = GUINT16_FROM_BE (dsi_header->requestID);
  dsi_header->errorCode = GUINT32_FROM_BE (dsi_header->errorCode);
  dsi_header->totalDataLength = GUINT32_FROM_BE (dsi_header->totalDataLength);

  if (dsi_header->totalDataLength == 0)
  {
    *data = NULL;
    return TRUE;    
  }
  
  *data = g_malloc (dsi_header->totalDataLength);
  read_count = dsi_header->totalDataLength;

  res = g_input_stream_read_all (input, *data, read_count, &bytes_read, cancellable, error);
  if (!res)
  {
    g_free (*data);
    return FALSE;
  }
  if (bytes_read < read_count)
  {
    g_free (*data);
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         _("Got unexpected end of stream"));
    return FALSE;
  }

  return TRUE;
}

static gboolean
send_request_sync (GOutputStream     *output,
                   DsiCommand        command,
                   guint16           request_id,
                   guint32           writeOffset,
                   gsize             len,
                   const char        *data,
                   GCancellable      *cancellable,
                   GError            **error)
{
  DSIHeader dsi_header;
  gboolean res;
  gsize write_count, bytes_written;

  dsi_header.flags = 0x00;
  dsi_header.command = command;
  dsi_header.requestID = GUINT16_TO_BE (request_id);
  dsi_header.writeOffset = GUINT32_TO_BE (writeOffset);
  dsi_header.totalDataLength = GUINT32_TO_BE (len); 
  dsi_header.reserved = 0;

  write_count = sizeof (DSIHeader);
  res = g_output_stream_write_all (output, &dsi_header, write_count,
                                   &bytes_written, cancellable, error);
  if (!res)
    return FALSE;

  if (data == NULL)
    return TRUE;

  write_count = len;
  res = g_output_stream_write_all (output, data, write_count, &bytes_written,
                                   cancellable, error);
  if (!res)
    return FALSE;

  return TRUE;
}

static void
send_command_sync_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  SyncData *sync_data = user_data;

  sync_data->data = g_object_ref (res);
  sync_data_signal (sync_data);
}

GVfsAfpReply *
g_vfs_afp_connection_send_command_sync (GVfsAfpConnection *afp_connection,
                                        GVfsAfpCommand    *command,
                                        GCancellable      *cancellable,
                                        GError            **error)
{
  SyncData sync_data;
  GVfsAfpReply *reply;
  
  if (!check_open (afp_connection, error))
    return FALSE;

  sync_data_init (&sync_data, afp_connection, NULL);

  g_vfs_afp_connection_send_command (afp_connection, command, NULL,
                                     send_command_sync_cb, cancellable, &sync_data);

  sync_data_wait (&sync_data);

  reply = g_vfs_afp_connection_send_command_finish (afp_connection, sync_data.data,
                                                    error);
  g_object_unref (sync_data.data);
  sync_data_clear (&sync_data);
  
  return reply;
}

static void
close_connection (GVfsAfpConnection *conn)
{
  GVfsAfpConnectionPrivate *priv = conn->priv;
  
  guint16 req_id;
  gboolean res;
  GError *err = NULL;

  GQueue *request_queue;
  GSList *pending_closes, *siter;
  GHashTable *request_hash;
  GHashTableIter iter;
  RequestData *req_data;

  /* Take lock */
  g_mutex_lock (&priv->mutex);

  /* Set closed flag */
  g_atomic_int_set (&priv->atomic_state, STATE_CLOSED);

  request_queue = priv->request_queue;
  priv->request_queue = NULL;

  request_hash = priv->request_hash;
  priv->request_hash = NULL;
  
  pending_closes = priv->pending_closes;
  priv->pending_closes = NULL;

  /* Release lock */
  g_mutex_unlock (&priv->mutex);
  
  /* close DSI session */
  req_id = get_request_id (conn);
  res = send_request_sync (g_io_stream_get_output_stream (priv->stream),
                           DSI_CLOSE_SESSION, req_id, 0, 0, NULL,
                           NULL, &err);
  if (!res)
    g_io_stream_close (priv->stream, NULL, NULL);
  else
    res = g_io_stream_close (priv->stream, NULL, &err);
  
  g_clear_object (&priv->stream);

  while ((req_data = g_queue_pop_head (request_queue)))
  {
    g_task_return_new_error (req_data->task, G_IO_ERROR, G_IO_ERROR_CLOSED, "Connection was closed");
    free_request_data (req_data);
  }

  g_hash_table_iter_init (&iter, request_hash);
  while (g_hash_table_iter_next (&iter, NULL, (void **)&req_data))
  {
    g_task_return_new_error (req_data->task, G_IO_ERROR, G_IO_ERROR_CLOSED, "Connection was closed");
    free_request_data (req_data);
  }

  /* quit main_loop */
  g_main_loop_quit (priv->worker_loop);
  g_main_loop_unref (priv->worker_loop);
  g_main_context_unref (priv->worker_context);
  
  for (siter = pending_closes; siter != NULL; siter = siter->next)
  {
    SyncData *close_data = siter->data;

    close_data->res = TRUE;
    sync_data_signal (close_data);
  }
  g_slist_free (pending_closes);
}

gboolean
g_vfs_afp_connection_close_sync (GVfsAfpConnection *afp_connection,
                                 GCancellable      *cancellable,
                                 GError            **error)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  SyncData close_data;

  /* Take lock */
  g_mutex_lock (&priv->mutex);

  if (!check_open (afp_connection, error)) {
    g_mutex_unlock (&priv->mutex);
    return FALSE;
  }

  sync_data_init (&close_data, afp_connection, error);
  priv->pending_closes = g_slist_prepend (priv->pending_closes, &close_data);

  /* Release lock */
  g_mutex_unlock (&priv->mutex);

  if (g_atomic_int_compare_and_exchange (&priv->atomic_state, STATE_CONNECTED, STATE_PENDING_CLOSE))
    g_cancellable_cancel (priv->read_cancellable);
  
  sync_data_wait (&close_data);

  
  return close_data.res;
}

static gpointer
open_thread_func (gpointer user_data)
{
  SyncData *data = user_data;
  GVfsAfpConnection *conn = data->conn;
  GVfsAfpConnectionPrivate *priv = conn->priv;

  GSocketClient *client;
  GSocketConnection *connection;
  GSocket *socket;
  GError *error = NULL;

  guint16 req_id;
  gboolean res = FALSE;
  char *reply;
  DSIHeader dsi_header;
  guint pos;

  client = g_socket_client_new ();
  connection = g_socket_client_connect (client,
                                        priv->addr,
                                        data->cancellable, data->error);
  g_object_unref (client);

  if (!connection)
    goto out;

  socket = g_socket_connection_get_socket (connection);
  if (!g_socket_set_option (socket, IPPROTO_TCP, TCP_NODELAY, TRUE, &error))
  {
    g_warning ("Could not set TCP_NODELAY: %s\n", error->message);
    g_error_free (error);
  }
  priv->stream = G_IO_STREAM (connection);

  req_id = get_request_id (conn);
  res = send_request_sync (g_io_stream_get_output_stream (priv->stream),
                           DSI_OPEN_SESSION, req_id, 0,  0, NULL,
                           data->cancellable, data->error);
  if (!res)
    goto out;

  res = read_reply_sync (g_io_stream_get_input_stream (priv->stream),
                         &dsi_header, &reply, data->cancellable, data->error);
  if (!res)
    goto out;

  pos = 0;
  while ((dsi_header.totalDataLength - pos) > 2)
  {
    guint8 optionType;
    guint8 optionLength;

    optionType = reply[pos++];
    optionLength = reply[pos++];

    switch (optionType)
    {
      
      case 0x00:
        if (optionLength == 4 && (dsi_header.totalDataLength - pos) >= 4)
          priv->kRequestQuanta = GUINT32_FROM_BE (*(guint32 *)(reply + pos));

        break;
        
      case 0x02:
        if (optionLength == 4 && (dsi_header.totalDataLength - pos) >= 4)
         priv->kServerReplayCacheSize = GUINT32_FROM_BE (*(guint32 *)(reply + pos));

        break;
      

      default:
        g_debug ("Unknown DSI option\n");
    }

    pos += optionLength;
  }
  g_free (reply);

out:
  if (res)
    g_atomic_int_set (&priv->atomic_state, STATE_CONNECTED);
  
  /* Signal sync call thread */
  data->res = res;
  sync_data_signal (data);

  /* Return from thread on failure */
  if (!res)
  {
    g_clear_object (&priv->stream);
    return NULL;
  }
  
  /* Create MainLoop */
  priv->worker_context = g_main_context_new ();
  priv->worker_loop = g_main_loop_new (priv->worker_context, TRUE);

  read_reply (conn);
  
  /* Run mainloop */
  g_main_loop_run (priv->worker_loop);

  return NULL;
}

gboolean
g_vfs_afp_connection_open_sync (GVfsAfpConnection *afp_connection,
                                GCancellable      *cancellable,
                                GError            **error)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  SyncData data;

  sync_data_init (&data, afp_connection, error);
  data.cancellable = cancellable;

  priv->worker_thread = g_thread_new ("AFP Worker Thread", open_thread_func,
                                      &data);
  sync_data_wait (&data);
  sync_data_clear (&data);

  return data.res;
}

GVfsAfpConnection *
g_vfs_afp_connection_new (GSocketConnectable *addr)
{
  GVfsAfpConnection        *afp_connection;
  GVfsAfpConnectionPrivate *priv;

  afp_connection = g_object_new (G_VFS_TYPE_AFP_CONNECTION, NULL);
  priv = afp_connection->priv;

  priv->addr = g_object_ref (addr);

  return afp_connection;
}

GVfsAfpReply *
g_vfs_afp_query_server_info (GSocketConnectable *addr,
                             GCancellable *cancellable,
                             GError **error)
{
  GSocketClient *client;
  GIOStream *conn;
  gboolean res;
  DSIHeader dsi_header;
  char *data;

  client = g_socket_client_new ();
  conn = G_IO_STREAM (g_socket_client_connect (client, addr, cancellable, error));
  g_object_unref (client);

  if (!conn)
    return NULL;

  res = send_request_sync (g_io_stream_get_output_stream (conn), DSI_GET_STATUS,
                           0, 0, 0, NULL, cancellable, error);
  if (!res)
  {
    g_object_unref (conn);
    return NULL;
  }

  res = read_reply_sync (g_io_stream_get_input_stream (conn), &dsi_header,
                         &data, cancellable, error);
  if (!res)
  {
    g_object_unref (conn);
    return NULL;
  }

  g_object_unref (conn);
  
  return g_vfs_afp_reply_new (dsi_header.errorCode, data,
                              dsi_header.totalDataLength, TRUE);
}

guint32
g_vfs_afp_connection_get_max_request_size (GVfsAfpConnection *afp_connection)
{
  g_return_val_if_fail (G_VFS_IS_AFP_CONNECTION (afp_connection), 0);

  return afp_connection->priv->kRequestQuanta;
}
