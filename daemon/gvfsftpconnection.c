/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2009 Benjamin Otte <otte@gnome.org>
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
 * Author: Benjamin Otte <otte@gnome.org>
 */

#include <config.h>

#include <string.h>
#include <glib/gi18n.h>

#include "gvfsftpconnection.h"

struct _GVfsFtpConnection
{
  GSocketClient *       client;                 /* socket client used for opening connections */

  GIOStream *		commands;               /* ftp command stream */
  GDataInputStream *    commands_in;            /* wrapper around in stream to allow line-wise reading */

  GIOStream *		data;                   /* ftp data stream or NULL if not in use */
};

GVfsFtpConnection *
g_vfs_ftp_connection_new (GSocketConnectable *addr,
                          GCancellable *      cancellable,
                          GError **           error)
{
  GVfsFtpConnection *conn;

  g_return_val_if_fail (G_IS_SOCKET_CONNECTABLE (addr), NULL);

  conn = g_slice_new0 (GVfsFtpConnection);
  conn->client = g_socket_client_new ();
  conn->commands = G_IO_STREAM (g_socket_client_connect (conn->client,
                                                         addr,
                                                         cancellable,
                                                         error));
  if (conn->commands == NULL)
    {
      g_object_unref (conn->client);
      g_slice_free (GVfsFtpConnection, conn);
      return NULL;
    }

  conn->commands_in = G_DATA_INPUT_STREAM (g_data_input_stream_new (g_io_stream_get_input_stream (conn->commands)));
  g_data_input_stream_set_newline_type (conn->commands_in, G_DATA_STREAM_NEWLINE_TYPE_CR_LF);

  return conn;
}

void
g_vfs_ftp_connection_free (GVfsFtpConnection *conn)
{
  g_return_if_fail (conn != NULL);

  if (conn->data)
    g_vfs_ftp_connection_close_data_connection (conn);

  g_object_unref (conn->commands_in);
  g_object_unref (conn->commands);
  g_object_unref (conn->client);
  g_slice_free (GVfsFtpConnection, conn);
}

gboolean
g_vfs_ftp_connection_send (GVfsFtpConnection *conn,
                           const char *       command,
                           int                len,
                           GCancellable *     cancellable,
                           GError **          error)
{
  g_return_val_if_fail (conn != NULL, FALSE);
  g_return_val_if_fail (command != NULL, FALSE);
  g_return_val_if_fail (len >= -1, FALSE);
  if (len < 0)
    len = strlen (command);
  g_return_val_if_fail (command[len-2] == '\r' && command[len-1] == '\n', FALSE);

  return g_output_stream_write_all (g_io_stream_get_output_stream (conn->commands),
                                    command,
                                    len,
                                    NULL,
                                    cancellable,
                                    error);
}

guint
g_vfs_ftp_connection_receive (GVfsFtpConnection *conn,
                              char ***           reply,
                              GCancellable *     cancellable,
                              GError **          error)
{
  char *line;
  enum {
    FIRST_LINE,
    MULTILINE,
    DONE
  } reply_state = FIRST_LINE;
  GPtrArray *lines;
  guint response = 0;

  g_return_val_if_fail (conn != NULL, FALSE);

  if (reply)
    lines = g_ptr_array_new_with_free_func (g_free);
  else
    lines = NULL;

  while (reply_state != DONE)
    {
      line = g_data_input_stream_read_line (conn->commands_in, NULL, cancellable, error);
      if (line == NULL)
        {
          if (error && *error == NULL)
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                                 _("Unexpected end of stream"));

          goto fail;
        }

      DEBUG ("<-- %s\n", line);
      if (lines)
        g_ptr_array_add (lines, line);

      if (reply_state == FIRST_LINE)
	{
	  if (line[0] <= '0' || line[0] > '5' ||
	      line[1] < '0' || line[1] > '9' ||
	      line[2] < '0' || line[2] > '9')
	    {
	      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
				   _("Invalid reply"));
              goto fail;
	    }
	  response = 100 * (line[0] - '0') +
		      10 * (line[1] - '0') +
		 	   (line[2] - '0');
	  if (line[3] == ' ')
	    reply_state = DONE;
	  else if (line[3] == '-')
	    reply_state = MULTILINE;
	  else
	    {
	      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
				   _("Invalid reply"));
              goto fail;
	    }
	}
      else
	{
	  if (line[0] - '0' == response / 100 &&
              line[1] - '0' == (response / 10) % 10 &&
              line[2] - '0' == response % 10 && 
	      line[3] == ' ')
	    reply_state = DONE;
	}
      if (!lines)
        g_free (line);
    }

  if (lines)
    {
      g_ptr_array_add (lines, NULL);
      *reply = (char **) g_ptr_array_free (lines, FALSE);
    }
  return response;

fail:
  if (lines)
    g_ptr_array_free (lines, TRUE);
  else
    g_free (line);
  return 0;
}

GSocketAddress *
g_vfs_ftp_connection_get_address (GVfsFtpConnection *conn, GError **error)
{
  g_return_val_if_fail (conn != NULL, NULL);

  return g_socket_connection_get_remote_address (G_SOCKET_CONNECTION (conn->commands), error);
}

gboolean
g_vfs_ftp_connection_open_data_connection (GVfsFtpConnection *conn,
                                           GSocketAddress *   addr,
                                           GCancellable *     cancellable,
                                           GError **          error)
{
  g_return_val_if_fail (conn != NULL, FALSE);
  g_return_val_if_fail (conn->data == NULL, FALSE);

  conn->data = G_IO_STREAM (g_socket_client_connect (conn->client,
                                                     G_SOCKET_CONNECTABLE (addr),
                                                     cancellable,
                                                     error));

  return conn->data != NULL;
}

void
g_vfs_ftp_connection_close_data_connection (GVfsFtpConnection *conn)
{
  g_return_if_fail (conn != NULL);

  if (conn->data == NULL)
    return;

  g_object_unref (conn->data);
  conn->data = NULL;
}

gssize
g_vfs_ftp_connection_write_data (GVfsFtpConnection *conn,
                                 const char *       data,
                                 gsize              len,
                                 GCancellable *     cancellable,
                                 GError **          error)
{
  g_return_val_if_fail (conn != NULL, -1);
  g_return_val_if_fail (conn->data != NULL, -1);

  /* FIXME: use write_all here? */
  return g_output_stream_write (g_io_stream_get_output_stream (conn->data),
                                data,
                                len,
                                cancellable,
                                error);
}

gssize
g_vfs_ftp_connection_read_data (GVfsFtpConnection *conn,
                                char *             data,
                                gsize              len,
                                GCancellable *     cancellable,
                                GError **          error)
{
  g_return_val_if_fail (conn != NULL, -1);
  g_return_val_if_fail (conn->data != NULL, -1);

  return g_input_stream_read (g_io_stream_get_input_stream (conn->data),
                               data,
                               len,
                               cancellable,
                               error);
}

gboolean
g_vfs_ftp_connection_read_contents (GVfsFtpConnection *conn,
                                    char **            data,
                                    gsize *            len,
                                    GCancellable *     cancellable,
                                    GError **          error)
{
  g_return_val_if_fail (conn != NULL, -1);
  g_return_val_if_fail (conn->data != NULL, -1);

  g_assert_not_reached ();
}

