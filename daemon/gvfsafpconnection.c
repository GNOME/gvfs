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

  reply = g_object_new (G_VFS_TYPE_AFP_REPLY,
                        "base-stream", g_memory_input_stream_new_from_data (data, len, g_free),
                        NULL);

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
                          "base-stream", g_memory_output_stream_new (NULL, 0, g_realloc, g_free));

  command->type = type;
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (command), type, NULL, NULL);

  /* pad byte */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (command), 0, NULL, NULL);

  return command;
}

void
g_vfs_afp_command_put_pascal (GVfsAfpCommand *command, char *str)
{
  size_t len;

  len = strlen (str);
  len = (len <= 256) ? len : 256;

  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (command), len, NULL, NULL);
  g_output_stream_write (G_OUTPUT_STREAM (command), str, len, NULL, NULL);
}



/*
 * GVfsAfpConnection
 */
G_DEFINE_TYPE (GVfsAfpConnection, g_vfs_afp_connection, G_TYPE_OBJECT);

struct _GVfsAfpConnectionPrivate
{
  GIOStream *conn;

  guint16 request_id;

  guint32 kRequestQuanta;
  guint32 kServerReplayCacheSize;
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

static guint16
get_request_id (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  return priv->request_id++;
}

static char *
read_reply_sync (GVfsAfpConnection *afp_connection,
                 DSIHeader         *dsi_header,
                 GCancellable      *cancellable,
                 GError            **error)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;
  
  GInputStream *input;
  gboolean res;
  gsize read_count, bytes_read;
  char *data;

  g_assert (dsi_header != NULL);
  
  input = g_io_stream_get_input_stream (priv->conn);

  read_count = sizeof (DSIHeader);
  res = g_input_stream_read_all (input, dsi_header, read_count, &bytes_read,
                                 NULL, NULL);
  if (!res || bytes_read < read_count)
    return NULL;

  dsi_header->requestID = GUINT16_FROM_BE (dsi_header->requestID);
  dsi_header->errorCode = GUINT32_FROM_BE (dsi_header->errorCode);
  dsi_header->totalDataLength = GUINT32_FROM_BE (dsi_header->totalDataLength);

  data = g_malloc (dsi_header->totalDataLength);
  read_count = dsi_header->totalDataLength;

  res = g_input_stream_read_all (input, data, read_count, &bytes_read, cancellable, error);
  if (!res || bytes_read < read_count)
  {
    g_free (data);
    return NULL;
  }

  return data;
}

static gboolean
send_request_sync (GVfsAfpConnection *afp_connection,
                   DsiCommand        command,
                   guint32           writeOffset,
                   gsize             len,
                   char              *data,
                   guint16           *request_id,
                   GCancellable      *cancellable,
                   GError            **error)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  GOutputStream *output;
  guint16 req_id;
  DSIHeader dsi_header;
  gboolean res;
  gsize write_count, bytes_written;

  output = g_io_stream_get_output_stream (priv->conn);

  req_id = get_request_id (afp_connection);
  if (request_id)
    *request_id = get_request_id (afp_connection);

  dsi_header.flags = 0x00;
  dsi_header.command = command;
  dsi_header.requestID = GUINT16_TO_BE (req_id);
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

GVfsAfpReply *
g_vfs_afp_connection_read_reply_sync (GVfsAfpConnection *afp_connection,
                                      GCancellable *cancellable,
                                      GError **error)
{
  char *data;
  DSIHeader dsi_header;

  data = read_reply_sync (afp_connection, &dsi_header, cancellable, error);
  if (!data)
    return NULL;

  return g_vfs_afp_reply_new (dsi_header.errorCode, data, dsi_header.totalDataLength);
}

gboolean
g_vfs_afp_connection_send_command_sync (GVfsAfpConnection *afp_connection,
                                        GVfsAfpCommand    *afp_command,
                                        GCancellable      *cancellable,
                                        GError            **error)
{
  DsiCommand dsi_command;
  guint32 writeOffset;
  GMemoryOutputStream *mem_stream;

  /* set dsi_command */
  switch (afp_command->type)
  {
    case AFP_COMMAND_GET_SRVR_INFO:
      writeOffset = 0;
      dsi_command = DSI_GET_STATUS;
      break;

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
  }

  mem_stream =
    G_MEMORY_OUTPUT_STREAM(g_filter_output_stream_get_base_stream (G_FILTER_OUTPUT_STREAM (afp_command)));
  
  return send_request_sync (afp_connection, dsi_command, writeOffset,
                            g_memory_output_stream_get_size (mem_stream),
                            g_memory_output_stream_get_data (mem_stream),
                            NULL, cancellable, error);
}

GVfsAfpConnection *
g_vfs_afp_connection_new (GSocketConnectable *addr,
                          GCancellable       *cancellable,
                          GError             **error)
{
  GVfsAfpConnection *afp_connection;
  GVfsAfpConnectionPrivate *priv;

  GSocketClient *client;

  char *reply;
  DSIHeader dsi_header;
  guint pos;

  afp_connection = g_object_new (G_VFS_TYPE_AFP_CONNECTION, NULL);
  priv = afp_connection->priv;

  client = g_socket_client_new ();
  priv->conn = G_IO_STREAM (g_socket_client_connect (client, addr, cancellable, error));
  g_object_unref (client);

  if (!priv->conn)
    goto error;

  if (!send_request_sync (afp_connection, DSI_OPEN_SESSION, 0, 0, NULL, NULL,
                          cancellable, error))
    goto error;

  reply = read_reply_sync (afp_connection, &dsi_header, cancellable, error);
  if (!reply)
    goto error;

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
  
  return afp_connection;
  
error:
  g_object_unref (afp_connection);
  return NULL;   
}

static void
g_vfs_afp_connection_init (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv;
  
  afp_connection->priv = priv =  G_TYPE_INSTANCE_GET_PRIVATE (afp_connection,
                                                              G_VFS_TYPE_AFP_CONNECTION,
                                                              GVfsAfpConnectionPrivate);

  priv->conn = NULL;
  priv->request_id = 0;

  priv->kRequestQuanta = -1;
  priv->kServerReplayCacheSize = -1;
}

static void
g_vfs_afp_connection_finalize (GObject *object)
{
  GVfsAfpConnection *afp_connection = (GVfsAfpConnection *)object;
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

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

