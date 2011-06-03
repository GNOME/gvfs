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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
 */

#include <string.h>


#include "gvfsafpconnection.h"


/*
 * GVfsAfpReply
 */
struct _GVfsAfpReplyClass
{
	GDataInputStreamClass parent_class;
};

struct _GVfsAfpReply
{
	GDataInputStream parent_instance;

  AfpErrorCode error_code;
};

G_DEFINE_TYPE (GVfsAfpReply, g_vfs_afp_reply, G_TYPE_DATA_INPUT_STREAM);

static void
g_vfs_afp_reply_init (GVfsAfpReply *object)
{
}

static void
g_vfs_afp_reply_class_init (GVfsAfpReplyClass *klass)
{
}

static GVfsAfpReply *
g_vfs_afp_reply_new (AfpErrorCode error_code, const char *data, gsize len)
{
  GVfsAfpReply *reply;

  if (data)
    reply = g_object_new (G_VFS_TYPE_AFP_REPLY, "base-stream",
                          g_memory_input_stream_new_from_data (data, len, g_free), 
                          NULL);
  else
    reply = g_object_new (G_VFS_TYPE_AFP_REPLY, "base-stream",
                          g_memory_input_stream_new (), NULL);
  

  reply->error_code = error_code;
  
  return reply;
}

char *
g_vfs_afp_reply_read_pascal (GVfsAfpReply *reply)
{
  GError *err;
  guint8 strsize;
  char *str;
  gboolean res;  
  gsize bytes_read;

  err = NULL;
  strsize = g_data_input_stream_read_byte (G_DATA_INPUT_STREAM (reply), NULL, &err);
  if (err != NULL)
  {
    g_error_free (err);
    return NULL;
  }

  str = g_malloc (strsize + 1);
  res = g_input_stream_read_all (G_INPUT_STREAM (reply), str, strsize,
                                 &bytes_read, NULL, NULL);
  if (!res ||  (bytes_read < strsize))
  {
    g_free (str);
    return NULL;
  }

  str[strsize] = '\0';
  return str;
}

gboolean
g_vfs_afp_reply_seek (GVfsAfpReply *reply, goffset offset, GSeekType type)
{
  gsize avail;
  GMemoryInputStream *mem_stream;

  /* flush buffered data */
  avail = g_buffered_input_stream_get_available (G_BUFFERED_INPUT_STREAM (reply));
  g_input_stream_skip (G_INPUT_STREAM (reply), avail, NULL, NULL);
  
  g_object_get (reply, "base-stream", &mem_stream, NULL);
  
  return g_seekable_seek (G_SEEKABLE (mem_stream), offset, type, NULL, NULL);
}

AfpErrorCode
g_vfs_afp_reply_get_error_code (GVfsAfpReply *reply)
{
  return reply->error_code;
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
};

G_DEFINE_TYPE (GVfsAfpCommand, g_vfs_afp_command, G_TYPE_DATA_OUTPUT_STREAM);


static void
g_vfs_afp_command_init (GVfsAfpCommand *command)
{
}

static void
g_vfs_afp_command_class_init (GVfsAfpCommandClass *klass)
{
}

GVfsAfpCommand *
g_vfs_afp_command_new (AfpCommandType type)
{
  GVfsAfpCommand *command;

  command = g_object_new (G_VFS_TYPE_AFP_COMMAND,
                          "base-stream", g_memory_output_stream_new (NULL, 0, g_realloc, g_free),
                          NULL);

  command->type = type;
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (command), type, NULL, NULL);

  return command;
}

void
g_vfs_afp_command_put_pascal (GVfsAfpCommand *command, const char *str)
{
  size_t len;

  len = strlen (str);
  len = (len <= 256) ? len : 256;

  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (command), len, NULL, NULL);
  g_output_stream_write (G_OUTPUT_STREAM (command), str, len, NULL, NULL);
}

void
g_vfs_afp_command_pad_to_even (GVfsAfpCommand *command)
{ 
  if (g_vfs_afp_command_get_size (command) % 2 == 1)
    g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (command), 0, NULL, NULL);
}

gsize
g_vfs_afp_command_get_size (GVfsAfpCommand *command)
{
  GMemoryOutputStream *mem_stream;

  mem_stream =
    G_MEMORY_OUTPUT_STREAM (g_filter_output_stream_get_base_stream (G_FILTER_OUTPUT_STREAM (command)));

  return g_memory_output_stream_get_data_size (mem_stream);
}

char *
g_vfs_afp_command_get_data (GVfsAfpCommand *command)
{
  GMemoryOutputStream *mem_stream;

  mem_stream =
    G_MEMORY_OUTPUT_STREAM (g_filter_output_stream_get_base_stream (G_FILTER_OUTPUT_STREAM (command)));

  return g_memory_output_stream_get_data (mem_stream);
}

/*
 * GVfsAfpConnection
 */
G_DEFINE_TYPE (GVfsAfpConnection, g_vfs_afp_connection, G_TYPE_OBJECT);

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

struct _GVfsAfpConnectionPrivate
{
  GSocketConnectable *addr;
  GIOStream *conn;

  guint16 request_id;

  guint32 kRequestQuanta;
  guint32 kServerReplayCacheSize;

  GQueue     *request_queue;
  GHashTable *request_hash;

  /* send loop */
  gboolean send_loop_running;
  gsize bytes_written;
  DSIHeader write_dsi_header;

  /* read loop */
  gboolean read_loop_running;
  gsize bytes_read;
  DSIHeader read_dsi_header;
  char *data;
};

typedef enum
{
  DSI_CLOSE_SESSION = 1,
  DSI_COMMAND       = 2,
  DSI_GET_STATUS    = 3,
  DSI_OPEN_SESSION  = 4,
  DSI_TICKLE        = 5,
  DSI_WRITE         = 6
} DsiCommand;

static guint16
get_request_id (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  return priv->request_id++;
}

typedef struct
{
  GVfsAfpCommand *command;
  GVfsAfpConnectionReplyCallback reply_cb;
  GCancellable *cancellable;
  gpointer user_data;
  GVfsAfpConnection *conn;
} RequestData;

static void
free_request_data (RequestData *request_data)
{
  g_object_unref (request_data->command);
  g_object_unref (request_data->cancellable);

  g_slice_free (RequestData, request_data);
}

static void
dispatch_reply (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  RequestData *req_data;

  if (priv->read_dsi_header.command == DSI_TICKLE)
  {
    /* TODO: should send back a DSI_TICKLE command */
  }
  
  else if (priv->read_dsi_header.command == DSI_COMMAND)
  {
    req_data = g_hash_table_lookup (priv->request_hash,
                                    GUINT_TO_POINTER (priv->read_dsi_header.requestID));
    if (req_data)
    {
      GVfsAfpReply *reply;

      reply = g_vfs_afp_reply_new (priv->read_dsi_header.errorCode, priv->data,
                                   priv->read_dsi_header.totalDataLength);
      req_data->reply_cb (afp_connection, reply, NULL, req_data->user_data);
      g_hash_table_remove (priv->request_hash, GUINT_TO_POINTER (priv->read_dsi_header.requestID));
    }
  }
  else
    g_free (priv->data);
}

static void read_reply (GVfsAfpConnection *afp_connection);

static void
read_data_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
  GInputStream *input = G_INPUT_STREAM (object);
  GVfsAfpConnection *afp_connection = G_VFS_AFP_CONNECTION (user_data);
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;
  
  gssize bytes_read;
  GError *err = NULL;
  
  bytes_read = g_input_stream_read_finish (input, res, &err);
  if (bytes_read == -1)
  {
    g_warning ("FAIL!!! \"%s\"\n", err->message);
    g_error_free (err);
  }

  priv->bytes_read += bytes_read;
  if (priv->bytes_read < priv->read_dsi_header.totalDataLength)
  {
    g_input_stream_read_async (input, priv->data + priv->bytes_read,
                               priv->read_dsi_header.totalDataLength - priv->bytes_read,
                               0, NULL, read_data_cb, afp_connection);
    return;
  }

  dispatch_reply (afp_connection);
  read_reply (afp_connection);
}

static void
read_dsi_header_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
  GInputStream *input = G_INPUT_STREAM (object);
  GVfsAfpConnection *afp_connection = G_VFS_AFP_CONNECTION (user_data);
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;
  
  gssize bytes_read;
  GError *err = NULL;
  
  bytes_read = g_input_stream_read_finish (input, res, &err);
  if (bytes_read == -1)
  {
    g_warning ("FAIL!!! \"%s\"\n", err->message);
    g_error_free (err);
  }

  priv->bytes_read += bytes_read;
  if (priv->bytes_read < sizeof (DSIHeader))
  {
    g_input_stream_read_async (input, &priv->read_dsi_header + priv->bytes_read,
                               sizeof (DSIHeader) - priv->bytes_read, 0, NULL,
                               read_dsi_header_cb, afp_connection);
    return;
  }

  priv->read_dsi_header.requestID = GUINT16_FROM_BE (priv->read_dsi_header.requestID);
  priv->read_dsi_header.errorCode = GUINT32_FROM_BE (priv->read_dsi_header.errorCode);
  priv->read_dsi_header.totalDataLength = GUINT32_FROM_BE (priv->read_dsi_header.totalDataLength);
  
  if (priv->read_dsi_header.totalDataLength > 0)
  {
    priv->data = g_malloc (priv->read_dsi_header.totalDataLength);
    priv->bytes_read = 0;
    g_input_stream_read_async (input, priv->data, priv->read_dsi_header.totalDataLength,
                               0, NULL, read_data_cb, afp_connection);
    return;
  }

  dispatch_reply (afp_connection);
  read_reply (afp_connection);
}

static void
read_reply (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;
  
  GInputStream *input;

  input = g_io_stream_get_input_stream (priv->conn);
  
  priv->bytes_read = 0;
  priv->data = NULL;
  g_input_stream_read_async (input, &priv->read_dsi_header, sizeof (DSIHeader), 0, NULL,
                             read_dsi_header_cb, afp_connection);
}

static void send_request (GVfsAfpConnection *afp_connection);

static void
write_command_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
  GOutputStream *output = G_OUTPUT_STREAM (object);
  RequestData *request_data = (RequestData *)user_data;
  GVfsAfpConnectionPrivate *priv = request_data->conn->priv;
  
  gssize bytes_written;
  GError *err = NULL;
  gsize size;
  
  bytes_written = g_output_stream_write_finish (output, res, &err);
  if (bytes_written == -1)
  {
    request_data->reply_cb (request_data->conn, NULL, err, request_data->user_data);
    free_request_data (request_data);
    g_error_free (err);
  }

  size = g_vfs_afp_command_get_size (request_data->command);
  
  priv->bytes_written += bytes_written;
  if (priv->bytes_written < size)
  {
    char *data;
    
    data = g_vfs_afp_command_get_data (request_data->command);
    
    
    g_output_stream_write_async (output, data + priv->bytes_written,
                                 size - priv->bytes_written, 0, NULL,
                                 write_command_cb, request_data);
    return;
  }

  g_hash_table_insert (priv->request_hash,
                       GUINT_TO_POINTER (GUINT16_FROM_BE (priv->write_dsi_header.requestID)),
                       request_data);

  send_request (request_data->conn);
}

static void
write_dsi_header_cb (GObject *object, GAsyncResult *res, gpointer user_data)
{
  GOutputStream *output = G_OUTPUT_STREAM (object);
  RequestData *request_data = (RequestData *)user_data;
  GVfsAfpConnectionPrivate *priv = request_data->conn->priv;
  
  gssize bytes_written;
  GError *err = NULL;

  char *data;
  gsize size;
  
  bytes_written = g_output_stream_write_finish (output, res, &err);
  if (bytes_written == -1)
  {
    request_data->reply_cb (request_data->conn, NULL, err, request_data->user_data);
    free_request_data (request_data);
    g_error_free (err);
  }

  priv->bytes_written += bytes_written;
  if (priv->bytes_written < sizeof (DSIHeader))
  {
    g_output_stream_write_async (output, &priv->write_dsi_header + priv->bytes_written,
                                 sizeof (DSIHeader) - priv->bytes_written, 0,
                                 NULL, write_dsi_header_cb, request_data);
    return;
  }

  data = g_vfs_afp_command_get_data (request_data->command);
  size = g_vfs_afp_command_get_size (request_data->command);
  
  priv->bytes_written = 0;
  g_output_stream_write_async (output, data, size, 0,
                               NULL, write_command_cb, request_data);
}

static void
send_request (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  RequestData *request_data;
  guint32 writeOffset;
  guint8 dsi_command;

  request_data = g_queue_pop_head (priv->request_queue);

  if (!request_data) {
    priv->send_loop_running = FALSE;
    return;
  }
 
  switch (request_data->command->type)
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
  priv->write_dsi_header.totalDataLength = GUINT32_TO_BE (g_vfs_afp_command_get_size (request_data->command));
  priv->write_dsi_header.reserved = 0;

  priv->bytes_written = 0;
  g_output_stream_write_async (g_io_stream_get_output_stream (priv->conn),
                               &priv->write_dsi_header, sizeof (DSIHeader), 0,
                               NULL, write_dsi_header_cb, request_data); 
}

static void
run_loop (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  if (!priv->send_loop_running)
  {
    send_request (afp_connection);
    priv->send_loop_running = TRUE;
  }
  if (!priv->read_loop_running)
  {
    read_reply (afp_connection);
    priv->read_loop_running = TRUE;
  }
}

void
g_vfs_afp_connection_queue_command (GVfsAfpConnection *afp_connection,
                                    GVfsAfpCommand *command,
                                    GVfsAfpConnectionReplyCallback reply_cb,
                                    GCancellable *cancellable,
                                    gpointer user_data)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  RequestData *request_data;
  
  request_data = g_slice_new (RequestData);
  request_data->command = g_object_ref (command);
  request_data->reply_cb = reply_cb;
  request_data->cancellable = g_object_ref (cancellable);
  request_data->user_data = user_data;
  request_data->conn = afp_connection;

  g_queue_push_tail (priv->request_queue, request_data);

  run_loop (afp_connection);
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
  if (!res || bytes_read < read_count)
    return FALSE;

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
  if (!res || bytes_read < read_count)
  {
    g_free (*data);
    return FALSE;
  }

  return TRUE;
}

GVfsAfpReply *
g_vfs_afp_connection_read_reply_sync (GVfsAfpConnection *afp_connection,
                                      GCancellable *cancellable,
                                      GError **error)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;
  
  gboolean res;
  char *data;
  DSIHeader dsi_header;

  res = read_reply_sync (g_io_stream_get_input_stream (priv->conn), &dsi_header,
                         &data, cancellable, error);
  if (!res)
    return NULL;

  return g_vfs_afp_reply_new (dsi_header.errorCode, data, dsi_header.totalDataLength);
}

static gboolean
send_request_sync (GOutputStream     *output,
                   DsiCommand        command,
                   guint16           request_id,
                   guint32           writeOffset,
                   gsize             len,
                   char              *data,
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
  if (!res || bytes_written < write_count)
    return FALSE;

  if (data == NULL)
    return TRUE;

  write_count = len;
  res = g_output_stream_write_all (output, data, write_count, &bytes_written,
                                   cancellable, error);
  if (!res || bytes_written < write_count)
    return FALSE;

  return TRUE;
}

gboolean
g_vfs_afp_connection_send_command_sync (GVfsAfpConnection *afp_connection,
                                        GVfsAfpCommand    *afp_command,
                                        GCancellable      *cancellable,
                                        GError            **error)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;
  
  DsiCommand dsi_command;
  guint16 req_id;
  guint32 writeOffset;

  /* set dsi_command */
  switch (afp_command->type)
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

  req_id = get_request_id (afp_connection);
  return send_request_sync (g_io_stream_get_output_stream (priv->conn),
                            dsi_command, req_id, writeOffset,
                            g_vfs_afp_command_get_size (afp_command),
                            g_vfs_afp_command_get_data (afp_command),
                            cancellable, error);
}

gboolean
g_vfs_afp_connection_open (GVfsAfpConnection *afp_connection,
                           GCancellable      *cancellable,
                           GError            **error)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  GSocketClient *client;

  guint16 req_id;
  gboolean res;
  char *reply;
  DSIHeader dsi_header;
  guint pos;

  client = g_socket_client_new ();
  priv->conn = G_IO_STREAM (g_socket_client_connect (client, priv->addr, cancellable, error));
  g_object_unref (client);

  if (!priv->conn)
    return FALSE;

  req_id = get_request_id (afp_connection);
  res = send_request_sync (g_io_stream_get_output_stream (priv->conn),
                           DSI_OPEN_SESSION, req_id, 0,  0, NULL,
                           cancellable, error);
  if (!res)
    return FALSE;

  res = read_reply_sync (g_io_stream_get_input_stream (priv->conn),
                         &dsi_header, &reply, cancellable, error);
  if (!res)
    return FALSE;

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

  return TRUE;
}

GVfsAfpReply *
g_vfs_afp_connection_get_server_info (GVfsAfpConnection *afp_connection,
                                      GCancellable *cancellable,
                                      GError **error)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;
  
  GSocketClient *client;
  GIOStream *conn;
  gboolean res;
  DSIHeader dsi_header;
  char *data;

  client = g_socket_client_new ();
  conn = G_IO_STREAM (g_socket_client_connect (client, priv->addr, cancellable, error));
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
                              dsi_header.totalDataLength);
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

static void
g_vfs_afp_connection_init (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv;
  
  afp_connection->priv = priv =  G_TYPE_INSTANCE_GET_PRIVATE (afp_connection,
                                                              G_VFS_TYPE_AFP_CONNECTION,
                                                              GVfsAfpConnectionPrivate);

  priv->addr = NULL;
  priv->conn = NULL;
  priv->request_id = 0;

  priv->kRequestQuanta = -1;
  priv->kServerReplayCacheSize = -1;

  priv->request_queue = g_queue_new ();
  priv->request_hash = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                              NULL, (GDestroyNotify)free_request_data);

  priv->send_loop_running = FALSE;
  priv->read_loop_running = FALSE;
}

static void
g_vfs_afp_connection_finalize (GObject *object)
{
  GVfsAfpConnection *afp_connection = (GVfsAfpConnection *)object;
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  if (priv->addr)
    g_object_unref (priv->addr);
  
  if (priv->conn)
    g_object_unref (priv->conn);

	G_OBJECT_CLASS (g_vfs_afp_connection_parent_class)->finalize (object);
}

static void
g_vfs_afp_connection_class_init (GVfsAfpConnectionClass *klass)
{
	GObjectClass* object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GVfsAfpConnectionPrivate));
  
	object_class->finalize = g_vfs_afp_connection_finalize;
}

