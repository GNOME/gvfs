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
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib/gthread.h>
#include <glib/gi18n.h>

#include <gio/gio.h>
#include "gdbusutils.h"
#include "gsysutils.h"
#include "gvfsdaemonutils.h"
#include "gvfsdaemonprotocol.h"

static gint32 extra_fd_slot = -1;
static GStaticMutex extra_lock = G_STATIC_MUTEX_INIT;

typedef struct {
  int extra_fd;
  int fd_count;
} ConnectionExtra;

static void
free_extra (gpointer p)
{
  ConnectionExtra *extra = p;
  close (extra->extra_fd);
  g_free (extra);
}

void
dbus_connection_add_fd_send_fd (DBusConnection *connection,
				int extra_fd)
{
  ConnectionExtra *extra;

  if (extra_fd_slot == -1 && 
      !dbus_connection_allocate_data_slot (&extra_fd_slot))
    g_error ("Unable to allocate data slot");

  extra = g_new0 (ConnectionExtra, 1);
  extra->extra_fd = extra_fd;
  
  if (!dbus_connection_set_data (connection, extra_fd_slot, extra, free_extra))
    _g_dbus_oom ();
}

gboolean 
dbus_connection_send_fd (DBusConnection *connection,
			 int fd, 
			 int *fd_id,
			 GError **error)
{
  ConnectionExtra *extra;

  g_assert (extra_fd_slot != -1);
  extra = dbus_connection_get_data (connection, extra_fd_slot);
  g_assert (extra != NULL);

  if (extra->extra_fd == -1)
    {
      g_set_error (error, G_IO_ERROR,
		   G_IO_ERROR_FAILED,
		   _("Internal Error (%s)"), "No fd passing socket available");
      return FALSE;
    }

  g_static_mutex_lock (&extra_lock);

  if (_g_socket_send_fd (extra->extra_fd, fd) == -1)
    {
      g_set_error (error, G_IO_ERROR,
		   g_io_error_from_errno (errno),
		   _("Error sending fd: %s"),
		   g_strerror (errno));
      g_static_mutex_unlock (&extra_lock);
      return FALSE;
    }

  *fd_id = extra->fd_count++;

  g_static_mutex_unlock (&extra_lock);

  return TRUE;
}

char *
g_error_to_daemon_reply (GError *error, guint32 seq_nr, gsize *len_out)
{
  char *buffer;
  const char *domain;
  gsize domain_len, message_len;
  GVfsDaemonSocketProtocolReply *reply;
  gsize len;
  
  domain = g_quark_to_string (error->domain);
  domain_len = strlen (domain);
  message_len = strlen (error->message);

  len = G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE +
    domain_len + 1 + message_len + 1;
  buffer = g_malloc (len);

  reply = (GVfsDaemonSocketProtocolReply *)buffer;
  reply->type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR);
  reply->seq_nr = g_htonl (seq_nr);
  reply->arg1 = g_htonl (error->code);
  reply->arg2 = g_htonl (domain_len + 1 + message_len + 1);

  memcpy (buffer + G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE,
	  domain, domain_len + 1);
  memcpy (buffer + G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE + domain_len + 1,
	  error->message, message_len + 1);
  
  *len_out = len;
  
  return buffer;
}
