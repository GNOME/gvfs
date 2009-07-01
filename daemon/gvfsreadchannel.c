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
#include <gvfsdaemonprotocol.h>
#include <gvfsdaemonutils.h>
#include <gvfsjobread.h>
#include <gvfsjobseekread.h>
#include <gvfsjobqueryinforead.h>
#include <gvfsjobcloseread.h>
#include <gvfsfileinfo.h>

struct _GVfsReadChannel
{
  GVfsChannel parent_instance;

  guint read_count;
  int seek_generation;
};

G_DEFINE_TYPE (GVfsReadChannel, g_vfs_read_channel, G_VFS_TYPE_CHANNEL)

static GVfsJob *read_channel_close          (GVfsChannel  *channel);
static GVfsJob *read_channel_handle_request (GVfsChannel  *channel,
					     guint32       command,
					     guint32       seq_nr,
					     guint32       arg1,
					     guint32       arg2,
					     gpointer      data,
					     gsize         data_len,
					     GError      **error);
static GVfsJob *read_channel_readahead      (GVfsChannel  *channel,
					     GVfsJob       *job);
  
static void
g_vfs_read_channel_finalize (GObject *object)
{
  if (G_OBJECT_CLASS (g_vfs_read_channel_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_read_channel_parent_class)->finalize) (object);
}

static void
g_vfs_read_channel_class_init (GVfsReadChannelClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsChannelClass *channel_class = G_VFS_CHANNEL_CLASS (klass);

  gobject_class->finalize = g_vfs_read_channel_finalize;
  channel_class->close = read_channel_close;
  channel_class->handle_request = read_channel_handle_request;
  channel_class->readahead = read_channel_readahead;
}

static void
g_vfs_read_channel_init (GVfsReadChannel *channel)
{
}

static GVfsJob *
read_channel_close (GVfsChannel *channel)
{
  return g_vfs_job_close_read_new (G_VFS_READ_CHANNEL (channel),
				   g_vfs_channel_get_backend_handle (channel),
				   g_vfs_channel_get_backend (channel));
} 

/* Always request large chunks. Its very inefficient
   to do network requests for smaller chunks. */
static guint32
modify_read_size (GVfsReadChannel *channel,
		  guint32 requested_size)
{
  guint32 real_size;
  
  if (channel->read_count <= 1)
    real_size = 16*1024;
  else if (channel->read_count <= 2)
    real_size = 32*1024;
  else
    real_size = 64*1024;
  
  if (requested_size > real_size)
    real_size = requested_size;

  /* Don't do ridicoulously large requests as this
     is just stupid on the network */
  if (real_size > 512 * 1024)
    real_size = 512 * 1024;

  return real_size;
}

static GVfsJob *
read_channel_handle_request (GVfsChannel *channel,
			     guint32 command,
			     guint32 seq_nr,
			     guint32 arg1,
			     guint32 arg2,
			     gpointer data,
			     gsize data_len,
			     GError **error)
{
  GVfsJob *job;
  GSeekType seek_type;
  GVfsBackendHandle backend_handle;
  GVfsBackend *backend;
  GVfsReadChannel *read_channel;
  char *attrs;

  read_channel = G_VFS_READ_CHANNEL (channel);
  backend_handle = g_vfs_channel_get_backend_handle (channel);
  backend = g_vfs_channel_get_backend (channel);
  
  job = NULL;
  switch (command)
    {
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_READ:
      read_channel->read_count++;
      job = g_vfs_job_read_new (read_channel,
				backend_handle,
				modify_read_size (read_channel, arg1),
				backend);
      break;
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CLOSE:
      job = g_vfs_job_close_read_new (read_channel,
				      backend_handle,
				      backend);
      break;
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_END:
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_SET:
      seek_type = G_SEEK_SET;
      if (command == G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_END)
	seek_type = G_SEEK_END;
      
      read_channel->read_count = 0;
      read_channel->seek_generation++;
      job = g_vfs_job_seek_read_new (read_channel,
				     backend_handle,
				     seek_type,
				     ((goffset)arg1) | (((goffset)arg2) << 32),
				     backend);
      break;

    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_QUERY_INFO:
      attrs = g_strndup (data, data_len);
      job = g_vfs_job_query_info_read_new (read_channel,
					   backend_handle,
					   attrs,
					   backend);
      
      g_free (attrs);
      break;
      
    default:
      g_set_error (error, G_IO_ERROR,
		   G_IO_ERROR_FAILED,
		   "Unknown stream command %"G_GUINT32_FORMAT, command);
      break;
    }

  /* Ownership was passed */
  g_free (data);
  return job;
}

static GVfsJob *
read_channel_readahead (GVfsChannel  *channel,
			GVfsJob       *job)
{
  GVfsJob *readahead_job;
  GVfsReadChannel *read_channel;
  GVfsJobRead *read_job;

  readahead_job = NULL;
  if (!job->failed &&
      G_VFS_IS_JOB_READ (job))
    {
      read_job = G_VFS_JOB_READ (job);
      read_channel = G_VFS_READ_CHANNEL (channel);

      if (read_job->data_count != 0)
	{
	  read_channel->read_count++;
	  readahead_job = g_vfs_job_read_new (read_channel,
					      g_vfs_channel_get_backend_handle (channel),
					      modify_read_size (read_channel, 8192),
					      g_vfs_channel_get_backend (channel));
	}
    }
  
  return readahead_job;
}


/* Might be called on an i/o thread
 */
void
g_vfs_read_channel_send_seek_offset (GVfsReadChannel *read_channel,
				     goffset offset)
{
  GVfsDaemonSocketProtocolReply reply;
  GVfsChannel *channel;

  channel = G_VFS_CHANNEL (read_channel);
  
  reply.type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SEEK_POS);
  reply.seq_nr = g_htonl (g_vfs_channel_get_current_seq_nr (channel));
  reply.arg1 = g_htonl (offset & 0xffffffff);
  reply.arg2 = g_htonl (offset >> 32);

  g_vfs_channel_send_reply (channel, &reply, NULL, 0);
}

/* Might be called on an i/o thread
 */
void
g_vfs_read_channel_send_closed (GVfsReadChannel *read_channel)
{
  GVfsDaemonSocketProtocolReply reply;
  GVfsChannel *channel;

  channel = G_VFS_CHANNEL (read_channel);
  
  reply.type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_CLOSED);
  reply.seq_nr = g_htonl (g_vfs_channel_get_current_seq_nr (channel));
  reply.arg1 = g_htonl (0);
  reply.arg2 = g_htonl (0);

  g_vfs_channel_send_reply (channel, &reply, NULL, 0);
}

/* Might be called on an i/o thread
 */
void
g_vfs_read_channel_send_data (GVfsReadChannel  *read_channel,
			      char            *buffer,
			      gsize            count)
{
  GVfsDaemonSocketProtocolReply reply;
  GVfsChannel *channel;

  channel = G_VFS_CHANNEL (read_channel);

  reply.type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA);
  reply.seq_nr = g_htonl (g_vfs_channel_get_current_seq_nr (channel));
  reply.arg1 = g_htonl (count);
  reply.arg2 = g_htonl (read_channel->seek_generation);

  g_vfs_channel_send_reply (channel, &reply, buffer, count);
}


GVfsReadChannel *
g_vfs_read_channel_new (GVfsBackend *backend,
                        GPid         actual_consumer)
{
  return g_object_new (G_VFS_TYPE_READ_CHANNEL,
		       "backend", backend,
                       "actual-consumer", actual_consumer,
		       NULL);
}
