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


#include "gvfsafpconnection.h"



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

static guint16
get_request_id (GVfsAfpConnection *afp_connection)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

  return priv->request_id++;
}
static char *
read_reply_sync (GVfsAfpConnection *afp_connection,
                 guint8            *flags,
                 DsiCommand        *command,
                 guint16           *request_id,
                 gsize             *len,
                 GCancellable      *cancellable,
                 GError            **error)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

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
  
  GInputStream *input;
  DSIHeader dsi_header;
  gsize read_count;
  char *data;
  
  input = g_io_stream_get_input_stream (priv->conn);

  read_count = sizeof (DSIHeader);
  while (read_count > 0)
  {
    gsize bytes_read;
    
    bytes_read = g_input_stream_read (input,
                                      &dsi_header + (sizeof (DSIHeader) - read_count),
                                      read_count, cancellable, error);
    if (bytes_read == -1)
      return NULL;

    read_count -= bytes_read;
  }
  dsi_header.requestID = GUINT16_FROM_BE (dsi_header.requestID);
  dsi_header.totalDataLength = GUINT32_FROM_BE (dsi_header.totalDataLength);
  
  *flags = dsi_header.flags;
  *command = dsi_header.command;
  *request_id = dsi_header.requestID;

  data = g_malloc (dsi_header.totalDataLength);
  read_count = dsi_header.totalDataLength;
  while (read_count > 0)
  {
    gsize bytes_read;
    
    bytes_read = g_input_stream_read (input,
                                      data + (dsi_header.totalDataLength - read_count),
                                      read_count, cancellable, error);
    if (bytes_read == -1)
    {
      g_free (data);
      return NULL;
    }

    read_count -= bytes_read;
  }

  *len = dsi_header.totalDataLength;  
  return data;
}

static gboolean
send_request_sync (GVfsAfpConnection *afp_connection,
                   DsiCommand        command,
                   gsize             len,
                   char              *data,
                   GCancellable      *cancellable,
                   GError            **error)
{
  GVfsAfpConnectionPrivate *priv = afp_connection->priv;

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
  
  GOutputStream *output;
  DSIHeader dsi_header;
  gsize write_count;
  
  output = g_io_stream_get_output_stream (priv->conn);

  dsi_header.flags = 0x00;
  dsi_header.command = command;
  dsi_header.requestID = GUINT16_TO_BE (get_request_id (afp_connection));
  dsi_header.writeOffset = 0;
  dsi_header.totalDataLength = 0; 
  dsi_header.reserved = 0;

  write_count = sizeof (DSIHeader);
  while (write_count > 0)
  {
    gsize bytes_written;

    bytes_written = g_output_stream_write (output,
                                           &dsi_header + (sizeof (DSIHeader) - write_count),
                                           write_count, cancellable, error);
    if (bytes_written == -1)
      return FALSE;

    write_count -= bytes_written;
  }

  write_count = len;
  while (write_count > 0)
  {
    gsize bytes_written;

    bytes_written = g_output_stream_write (output, data + (len - write_count),
                                           write_count, cancellable, error);
    if (bytes_written == -1)
      return FALSE;

    write_count -= bytes_written;
  }

  return TRUE;
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
  guint8 flags;
  DsiCommand command;
  guint16 request_id;
  gsize len;
  guint pos;
    
  afp_connection = g_object_new (G_VFS_TYPE_AFP_CONNECTION, NULL);
  priv = afp_connection->priv;
  
  client = g_socket_client_new ();
  priv->conn = G_IO_STREAM (g_socket_client_connect (client, addr, cancellable, error));
  g_object_unref (client);

  if (!priv->conn)
    goto error;

  if (!send_request_sync (afp_connection, DSI_OPEN_SESSION, 0, NULL,
                          cancellable, error))
    goto error;

  reply = read_reply_sync (afp_connection, &flags, &command, &request_id, &len,
                           cancellable, error);
  if (!reply)
    goto error;

  pos = 0;
  while ((len - pos) > 2)
  {
    guint8 optionType;
    guint8 optionLength;

    optionType = reply[pos++];
    optionLength = reply[pos++];

    switch (optionType)
    {
      
      case 0x00:
        if (optionLength == 4 && (len - pos) >= 4)
          priv->kRequestQuanta = GUINT32_FROM_BE (*(guint32 *)(reply + pos));

        break;
        
      case 0x02:
        if (optionLength == 4 && (len - pos) >= 4)
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

