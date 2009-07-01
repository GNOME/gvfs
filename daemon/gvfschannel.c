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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gi18n.h>
#include <gvfsreadchannel.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsdaemonutils.h>
#include <gvfsjobcloseread.h>
#include <gvfsjobclosewrite.h>
#include <gvfsfileinfo.h>

static void g_vfs_channel_job_source_iface_init (GVfsJobSourceIface *iface);

G_DEFINE_TYPE_WITH_CODE (GVfsChannel, g_vfs_channel, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_VFS_TYPE_JOB_SOURCE,
						g_vfs_channel_job_source_iface_init))

/* TODO: Real P_() */
#define P_(_x) (_x)

enum {
  PROP_0,
  PROP_BACKEND,
  PROP_ACTUAL_CONSUMER
};

typedef struct
{
  GVfsChannel *channel;
  GInputStream *command_stream;
  char buffer[G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE];
  int buffer_size;
  char *data;
  gsize data_len;
  gsize data_pos;
} RequestReader;

typedef struct {
  guint32 command;
  guint32 arg1;
  guint32 arg2;
  guint32 seq_nr;

  gpointer data;
  gsize data_len;
  gboolean cancelled;
} Request;

struct _GVfsChannelPrivate
{
  GVfsBackend *backend;
  gboolean connection_closed;
  GInputStream *command_stream;
  GOutputStream *reply_stream;
  int remote_fd;
  GPid actual_consumer;
  
  GVfsBackendHandle backend_handle;
  GVfsJob *current_job;
  guint32 current_job_seq_nr;

  GList *queued_requests;
  
  RequestReader *request_reader;
  
  char reply_buffer[G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE];
  int reply_buffer_pos;
  
  const char *output_data; /* Owned by job */
  gsize output_data_size;
  gsize output_data_pos;
};

static void start_request_reader       (GVfsChannel  *channel);
static void g_vfs_channel_get_property (GObject      *object,
					guint         prop_id,
					GValue       *value,
					GParamSpec   *pspec);
static void g_vfs_channel_set_property (GObject      *object,
					guint         prop_id,
					const GValue *value,
					GParamSpec   *pspec);


static void
g_vfs_channel_finalize (GObject *object)
{
  GVfsChannel *channel;

  channel = G_VFS_CHANNEL (object);

  if (channel->priv->current_job)
    g_object_unref (channel->priv->current_job);
  channel->priv->current_job = NULL;
  
  if (channel->priv->reply_stream)
    g_object_unref (channel->priv->reply_stream);
  channel->priv->reply_stream = NULL;

  if (channel->priv->request_reader)
    channel->priv->request_reader->channel = NULL;
  channel->priv->request_reader = NULL;

  if (channel->priv->command_stream)
    g_object_unref (channel->priv->command_stream);
  channel->priv->command_stream = NULL;
  
  if (channel->priv->remote_fd != -1)
    close (channel->priv->remote_fd);

  if (channel->priv->backend)
    g_object_unref (channel->priv->backend);
  
  g_assert (channel->priv->backend_handle == NULL);
  
  if (G_OBJECT_CLASS (g_vfs_channel_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_channel_parent_class)->finalize) (object);
}

static void
g_vfs_channel_job_source_iface_init (GVfsJobSourceIface *iface)
{
}

static void
g_vfs_channel_class_init (GVfsChannelClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GVfsChannelPrivate));
  
  gobject_class->finalize = g_vfs_channel_finalize;
  gobject_class->set_property = g_vfs_channel_set_property;
  gobject_class->get_property = g_vfs_channel_get_property;

  g_object_class_install_property (gobject_class,
				   PROP_BACKEND,
				   g_param_spec_object ("backend",
							P_("Backend"),
							P_("Backend implementation to use"),
							G_VFS_TYPE_BACKEND,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));

  g_object_class_install_property (gobject_class,
				   PROP_ACTUAL_CONSUMER,
				   g_param_spec_int ("actual-consumer",
                                                     P_("Actual Consumer"),
                                                     P_("The process id of the remote end"),
                                                     G_MININT,
                                                     G_MAXINT,
                                                     0,
                                                     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                                                     G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));
}

static void
g_vfs_channel_init (GVfsChannel *channel)
{
  int socket_fds[2];
  int ret;
  
  channel->priv = G_TYPE_INSTANCE_GET_PRIVATE (channel,
					       G_VFS_TYPE_CHANNEL,
					       GVfsChannelPrivate);
  channel->priv->remote_fd = -1;

  ret = socketpair (AF_UNIX, SOCK_STREAM, 0, socket_fds);
  if (ret == -1) 
    g_warning ("Error creating socket pair: %d\n", errno);
  else
    {
      channel->priv->command_stream = g_unix_input_stream_new (socket_fds[0], TRUE);
      channel->priv->reply_stream = g_unix_output_stream_new (socket_fds[0], FALSE);
      channel->priv->remote_fd = socket_fds[1];
      
      start_request_reader (channel);
    }
}

static void
g_vfs_channel_set_property (GObject         *object,
			    guint            prop_id,
			    const GValue    *value,
			    GParamSpec      *pspec)
{
  GVfsChannel *channel = G_VFS_CHANNEL (object);
  
  switch (prop_id)
    {
    case PROP_BACKEND:
      if (channel->priv->backend)
	g_object_unref (channel->priv->backend);
      channel->priv->backend = G_VFS_BACKEND (g_value_dup_object (value));
      break;

    case PROP_ACTUAL_CONSUMER:
      channel->priv->actual_consumer = g_value_get_int (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_vfs_channel_get_property (GObject    *object,
			    guint       prop_id,
			    GValue     *value,
			    GParamSpec *pspec)
{
  GVfsChannel *channel = G_VFS_CHANNEL (object);
  
  switch (prop_id)
    {
    case PROP_BACKEND:
      g_value_set_object (value, channel->priv->backend);
      break;
    case PROP_ACTUAL_CONSUMER:
      g_value_set_int (value, channel->priv->actual_consumer);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_vfs_channel_connection_closed (GVfsChannel *channel)
{
  GVfsChannelClass *class;

  if (channel->priv->connection_closed)
    return;
  channel->priv->connection_closed = TRUE;
  
  if (channel->priv->current_job == NULL &&
      channel->priv->backend_handle != NULL)
    {
      class = G_VFS_CHANNEL_GET_CLASS (channel);
      
      channel->priv->current_job = class->close (channel);
      channel->priv->current_job_seq_nr = 0;
      g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (channel), channel->priv->current_job);
    }
  /* Otherwise we'll close when current_job is finished */
}

static void
request_reader_free (RequestReader *reader)
{
  g_object_unref (reader->command_stream);
  g_free (reader->data);
  g_free (reader);
}

static gboolean
start_queued_request (GVfsChannel *channel)
{
  GVfsChannelClass *class;
  Request *req;
  GVfsJob *job;
  GError *error;
  gboolean started_job;

  started_job = FALSE;
  
  class = G_VFS_CHANNEL_GET_CLASS (channel);
  
  while (channel->priv->current_job == NULL &&
	 channel->priv->queued_requests != NULL)
    {
      req = channel->priv->queued_requests->data;

      channel->priv->queued_requests =
	g_list_delete_link (channel->priv->queued_requests,
			    channel->priv->queued_requests);
      
      error = NULL;
      job = NULL;
      if (req->cancelled)
	{
	  error =
	    g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
				 _("Operation was cancelled"));
	  g_free (req->data); /* Did no pass ownership */
	}
      else
	{
	  /* This passes on ownership of req->data */
	  job = class->handle_request (channel,
				       req->command, req->seq_nr,
				       req->arg1, req->arg2,
				       req->data, req->data_len, 
				       &error);
	}
      
      if (job)
	{
	  channel->priv->current_job = job;
	  channel->priv->current_job_seq_nr = req->seq_nr;
	  g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (channel), channel->priv->current_job);
	  started_job = TRUE;
	}
      else
	{
	  g_vfs_channel_send_error (channel, error);
	  g_error_free (error);
	}
      
      g_free (req);
    }

  return started_job;
}


/* Ownership of data is passed here to avoid copying it */
static void
got_request (GVfsChannel *channel,
	     GVfsDaemonSocketProtocolRequest *request,
	     gpointer data, gsize data_len)
{
  Request *req;
  guint32 command, arg1;
  GList *l;

  command = g_ntohl (request->command);
  arg1 = g_ntohl (request->arg1);

  if (command == G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL)
    {
      if (arg1 == channel->priv->current_job_seq_nr &&
	  channel->priv->current_job != NULL)
	g_vfs_job_cancel (channel->priv->current_job);
      else
	{
	  for (l = channel->priv->queued_requests; l != NULL; l = l->next)
	    {
	      req = l->data;

	      if (req->seq_nr == 0)
		/* We're cancelling something later but this readahead might
		   be the actual operation thats replacing it */
		req->cancelled = TRUE;
		  
	      if (req->seq_nr == arg1)
		{
		  req->cancelled = TRUE;
		  break;
		}
	    }
	}

      /* Cancel ops get no return */
      g_free (data);
      return;
    }
  
  req = g_new0 (Request, 1);
  req->command = command;
  req->arg1 = arg1;
  req->arg2 = g_ntohl (request->arg2);
  req->seq_nr = g_ntohl (request->seq_nr);
  req->data_len = data_len;
  req->data = data;

  channel->priv->queued_requests =
    g_list_append (channel->priv->queued_requests,
		   req);
  
  start_queued_request (channel);
}

static void command_read_cb (GObject *source_object,
			     GAsyncResult *res,
			     gpointer user_data);


static void
finish_request (RequestReader *reader)
{
  /* Ownership of reader->data passed here */
  got_request (reader->channel, (GVfsDaemonSocketProtocolRequest *)reader->buffer,
	       reader->data, reader->data_len);
  reader->data = NULL;
  
  /* Request more commands immediately, so can get cancel requests */

  reader->buffer_size = 0;
  reader->data_len = 0;
  g_input_stream_read_async (reader->command_stream,
			     reader->buffer + reader->buffer_size,
			     G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE - reader->buffer_size,
			     0, NULL,
			     command_read_cb,
			     reader);
}

static void
data_read_cb (GObject *source_object,
	      GAsyncResult *res,
	      gpointer user_data)
{
  RequestReader *reader = user_data;
  GInputStream *stream = G_INPUT_STREAM (source_object);
  gssize count_read;

  if (reader->channel == NULL)
    {
      /* Channel was finalized */
      request_reader_free (reader);
      return;
    }

  count_read = g_input_stream_read_finish (stream, res, NULL);
  
  if (count_read <= 0)
    {
      reader->channel->priv->request_reader = NULL;
      g_vfs_channel_connection_closed (reader->channel);
      request_reader_free (reader);
      return;
    }

  reader->data_pos += count_read;

  if (reader->data_pos < reader->data_len)
    {
      g_input_stream_read_async (reader->command_stream,
				 reader->data + reader->data_pos,
				 reader->data_len - reader->data_pos,
				 0, NULL,
				 data_read_cb, reader);
      return;
    }
  
  finish_request (reader);
}
  

static void
command_read_cb (GObject *source_object,
		 GAsyncResult *res,
		 gpointer user_data)
{
  GInputStream *stream = G_INPUT_STREAM (source_object);
  RequestReader *reader = user_data;
  GVfsDaemonSocketProtocolRequest *request;
  guint32 data_len;
  gssize count_read;

  if (reader->channel == NULL)
    {
      /* Channel was finalized */
      request_reader_free (reader);
      return;
    }

  count_read = g_input_stream_read_finish (stream, res, NULL);
  
  if (count_read <= 0)
    {
      reader->channel->priv->request_reader = NULL;
      g_vfs_channel_connection_closed (reader->channel);
      request_reader_free (reader);
      return;
    }

  reader->buffer_size += count_read;

  if (reader->buffer_size < G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE)
    {
      g_input_stream_read_async (reader->command_stream,
				 reader->buffer + reader->buffer_size,
				 G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE - reader->buffer_size,
				 0, NULL, 
				 command_read_cb, reader);
      return;
    }

  request = (GVfsDaemonSocketProtocolRequest *)reader->buffer;
  data_len  = g_ntohl (request->data_len);

  if (data_len > 0)
    {
      reader->data = g_malloc (data_len);
      reader->data_len = data_len;
      reader->data_pos = 0;

      g_input_stream_read_async (reader->command_stream,
				 reader->data + reader->data_pos,
				 reader->data_len - reader->data_pos,
				 0, NULL,
				 data_read_cb, reader);
      return;
    }
  
  finish_request (reader);
}

static void
start_request_reader (GVfsChannel *channel)
{
  RequestReader *reader;

  reader = g_new0 (RequestReader, 1);
  reader->channel = channel;
  reader->command_stream = g_object_ref (channel->priv->command_stream);
  
  g_input_stream_read_async (reader->command_stream,
			     reader->buffer + reader->buffer_size,
			     G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE - reader->buffer_size,
			     0, NULL,
			     command_read_cb, reader);

  channel->priv->request_reader = reader;
}

static void
send_reply_cb (GObject *source_object,
	       GAsyncResult *res,
	       gpointer user_data)
{
  GOutputStream *output_stream = G_OUTPUT_STREAM (source_object);
  gssize bytes_written;
  GVfsChannel *channel = user_data;
  GVfsChannelClass *class;
  GVfsJob *job;

  bytes_written = g_output_stream_write_finish (output_stream, res, NULL);
  
  if (bytes_written <= 0)
    {
      g_vfs_channel_connection_closed (channel);
      goto error_out;
    }

  if (channel->priv->reply_buffer_pos < G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE)
    {
      channel->priv->reply_buffer_pos += bytes_written;

      /* Write more of reply header if needed */
      if (channel->priv->reply_buffer_pos < G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE)
	{
	  g_output_stream_write_async (channel->priv->reply_stream,
				       channel->priv->reply_buffer + channel->priv->reply_buffer_pos,
				       G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE - channel->priv->reply_buffer_pos,
				       0, NULL,
				       send_reply_cb, channel);  
	  return;
	}
      bytes_written = 0;
    }

  channel->priv->output_data_pos += bytes_written;

  /* Write more of output_data if needed */
  if (channel->priv->output_data != NULL &&
      channel->priv->output_data_pos < channel->priv->output_data_size)
    {
      g_output_stream_write_async (channel->priv->reply_stream,
				   channel->priv->output_data + channel->priv->output_data_pos,
				   channel->priv->output_data_size - channel->priv->output_data_pos,
				   0, NULL,
				   send_reply_cb, channel);
      return;
    }

 error_out:
  
  /* Sent full reply */
  channel->priv->output_data = NULL;

  job = channel->priv->current_job;
  channel->priv->current_job = NULL;
  g_vfs_job_emit_finished (job);

  class = G_VFS_CHANNEL_GET_CLASS (channel);
  
  if (G_VFS_IS_JOB_CLOSE_READ (job) ||
      G_VFS_IS_JOB_CLOSE_WRITE (job))
    {
      g_vfs_job_source_closed (G_VFS_JOB_SOURCE (channel));
      channel->priv->backend_handle = NULL;
    }
  else if (channel->priv->connection_closed)
    {

      channel->priv->current_job = class->close (channel);
      channel->priv->current_job_seq_nr = 0;
      g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (channel), channel->priv->current_job);
    }
  /* Start queued request or readahead */
  else if (!start_queued_request (channel) &&
	   class->readahead)
    {
      /* No queued requests, maybe we want to do a readahead call */
      channel->priv->current_job = class->readahead (channel, job);
      channel->priv->current_job_seq_nr = 0;
      if (channel->priv->current_job)
	g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (channel), channel->priv->current_job);
    }

  g_object_unref (job);
}

/* Might be called on an i/o thread */
void
g_vfs_channel_send_reply (GVfsChannel *channel,
			  GVfsDaemonSocketProtocolReply *reply,
			  const void *data,
			  gsize data_len)
{
  
  channel->priv->output_data = data;
  channel->priv->output_data_size = data_len;
  channel->priv->output_data_pos = 0;

  if (reply != NULL)
    {
      memcpy (channel->priv->reply_buffer, reply, sizeof (GVfsDaemonSocketProtocolReply));
      channel->priv->reply_buffer_pos = 0;

      g_output_stream_write_async (channel->priv->reply_stream,
				   channel->priv->reply_buffer,
				   G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE,
				   0, NULL,
				   send_reply_cb, channel);  
    }
  else
    {
      channel->priv->reply_buffer_pos = G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE;
      g_output_stream_write_async (channel->priv->reply_stream,
				   channel->priv->output_data,
				   channel->priv->output_data_size,
				   0, NULL,
				   send_reply_cb, channel);  
    }
}

/* Might be called on an i/o thread
 */
void
g_vfs_channel_send_error (GVfsChannel *channel,
			  GError *error)
{
  char *data;
  gsize data_len;
  
  data = g_error_to_daemon_reply (error, channel->priv->current_job_seq_nr, &data_len);
  g_vfs_channel_send_reply (channel, NULL, data, data_len);
}

/* Might be called on an i/o thread
 */
void
g_vfs_channel_send_info (GVfsChannel *channel,
			 GFileInfo *info)
{
  GVfsDaemonSocketProtocolReply reply;
  char *data;
  gsize data_len;
  
  data = gvfs_file_info_marshal (info, &data_len);

  reply.type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_INFO);
  reply.seq_nr = g_htonl (g_vfs_channel_get_current_seq_nr (channel));
  reply.arg1 = 0;
  reply.arg2 = g_htonl (data_len);

  g_vfs_channel_send_reply (channel, &reply, data, data_len);
}

int
g_vfs_channel_steal_remote_fd (GVfsChannel *channel)
{
  int fd;
  fd = channel->priv->remote_fd;
  channel->priv->remote_fd = -1;
  return fd;
}

GVfsBackend *
g_vfs_channel_get_backend (GVfsChannel  *channel)
{
  return channel->priv->backend;
}

void
g_vfs_channel_set_backend_handle (GVfsChannel *channel,
				  GVfsBackendHandle backend_handle)
{
  channel->priv->backend_handle = backend_handle;
}

GVfsBackendHandle
g_vfs_channel_get_backend_handle (GVfsChannel *channel)
{
  return channel->priv->backend_handle;
}

guint32
g_vfs_channel_get_current_seq_nr (GVfsChannel *channel)
{
  return channel->priv->current_job_seq_nr;
}

GPid
g_vfs_channel_get_actual_consumer (GVfsChannel *channel)
{
  return channel->priv->actual_consumer;
}

