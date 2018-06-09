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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Benjamin Otte <otte@gnome.org>
 */

#include <config.h>

#include "gvfsftpconnection.h"

#include <string.h>
#include <glib/gi18n.h>
#include <gio/gnetworking.h>

#include "gvfsbackendftp.h"

/* used for identifying the connection during debugging */
static volatile int debug_id = 0;

struct _GVfsFtpConnection
{
  GSocketClient *       client;                 /* socket client used for opening connections */

  GIOStream *        	commands;               /* ftp command stream */
  GSocketConnection *   connection;             /* original connection */
  GDataInputStream *    commands_in;            /* wrapper around in stream to allow line-wise reading */
  gboolean              waiting_for_reply;           /* TRUE if a command was sent but no reply received yet */

  GSocket *             listen_socket;          /* socket we are listening on for active FTP connections */
  GIOStream *        	data;                   /* ftp data stream or NULL if not in use */

  int                   debug_id;               /* unique id for debugging purposes */
};

static void
enable_keepalive (GSocketConnection *conn)
{
  /* We enable keepalive on the socket, because data connections can be
   * idle for a long time while data is transferred using the data
   * connection. And there are still buggy routers in existance that purge
   * idle connections from time to time.
   * To work around this problem, we set the keep alive flag here. It's the
   * user's responsibility to configure his kernel properly so that the
   * keepalive packets are sent before the buggy router disconnects the
   * TCP connection. If a user asks, a howto is at
   * http://tldp.org/HOWTO/html_single/TCP-Keepalive-HOWTO/
   */
  g_socket_set_keepalive (g_socket_connection_get_socket (conn), TRUE);
}

/* Set TCP_NODELAY on the connection to avoid a bad interaction between Nagle's
 * algorithm and delayed acks when doing a write-write-read. */
static void
enable_nodelay (GSocketConnection *conn)
{
  GError *error = NULL;
  GSocket *socket = g_socket_connection_get_socket (conn);

  if (!g_socket_set_option (socket, IPPROTO_TCP, TCP_NODELAY, TRUE, &error))
    {
      g_warning ("Could not set TCP_NODELAY: %s\n", error->message);
      g_error_free (error);
    }
}

static void
create_input_stream (GVfsFtpConnection *conn)
{
  if (conn->commands_in)
    {
      g_filter_input_stream_set_close_base_stream (G_FILTER_INPUT_STREAM (conn->commands_in), FALSE);
      g_object_unref (conn->commands_in);
    }

  conn->commands_in = G_DATA_INPUT_STREAM (g_data_input_stream_new (g_io_stream_get_input_stream (conn->commands)));
  g_data_input_stream_set_newline_type (conn->commands_in, G_DATA_STREAM_NEWLINE_TYPE_CR_LF);
}

GVfsFtpConnection *
g_vfs_ftp_connection_new (GSocketConnectable *addr,
                          GCancellable *      cancellable,
                          GError **           error)
{
  GVfsFtpConnection *conn;

  g_return_val_if_fail (G_IS_SOCKET_CONNECTABLE (addr), NULL);

  conn = g_slice_new0 (GVfsFtpConnection);
  conn->client = g_socket_client_new ();
  conn->debug_id = g_atomic_int_add (&debug_id, 1);
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

  conn->connection = G_SOCKET_CONNECTION (conn->commands);
  enable_nodelay (conn->connection);
  enable_keepalive (conn->connection);
  create_input_stream (conn);
  /* The first thing that needs to happen is receiving the welcome message */
  conn->waiting_for_reply = TRUE;

  return conn;
}

static void
g_vfs_ftp_connection_stop_listening (GVfsFtpConnection *conn)
{
  if (conn->listen_socket)
    {
      g_object_unref (conn->listen_socket);
      conn->listen_socket = NULL;
    }
}

void
g_vfs_ftp_connection_free (GVfsFtpConnection *conn)
{
  g_return_if_fail (conn != NULL);

  g_vfs_ftp_connection_stop_listening (conn);
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
  g_return_val_if_fail (!conn->waiting_for_reply, FALSE);
  g_return_val_if_fail (command != NULL, FALSE);
  g_return_val_if_fail (len >= -1, FALSE);
  if (len < 0)
    len = strlen (command);
  g_return_val_if_fail (command[len-2] == '\r' && command[len-1] == '\n', FALSE);

  if (g_str_has_prefix (command, "PASS"))
    g_debug ("--%2d ->  PASS ***\r\n", conn->debug_id);
  else
    g_debug ("--%2d ->  %s", conn->debug_id, command);

  conn->waiting_for_reply = TRUE;
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

  g_return_val_if_fail (conn != NULL, 0);
  g_return_val_if_fail (conn->waiting_for_reply, 0);

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

      g_debug ("<-%2d --  %s\r\n", conn->debug_id, line);
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

  /* 1xx commands are intermediate commands and require a further
   * message from the server to complete
   */
  if (response >= 200)
    conn->waiting_for_reply = FALSE;

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

  return g_socket_connection_get_remote_address (conn->connection, error);
}

/**
 * g_vfs_ftp_connection_data_connection_enable_tls:
 * @conn: a connection with an active control connection
 * @server_identity: address of the server used to verify the certificate
 * @cb: callback called if there's a verification error
 * @user_data: user data passed to @cb
 * @cancellable: cancellable to interrupt wait
 * @error: %NULL or location to take a potential error
 *
 * Tries to enable TLS on the given @connection's data connection. If setting
 * up TLS fails, %FALSE will be returned and @error will be set.
 *
 * Returns: %TRUE on success, %FALSE otherwise.
 **/
gboolean
g_vfs_ftp_connection_data_connection_enable_tls (GVfsFtpConnection  *conn,
                                                 GSocketConnectable *server_identity,
                                                 CertificateCallback cb,
                                                 gpointer            user_data,
                                                 GCancellable *      cancellable,
                                                 GError **           error)
{
  GIOStream *secure;

  g_return_val_if_fail (conn != NULL, FALSE);
  g_return_val_if_fail (conn->commands != NULL, FALSE);

  secure = g_tls_client_connection_new (conn->data,
                                        server_identity,
                                        error);
  if (secure == NULL)
    return FALSE;

  g_object_unref (conn->data);
  conn->data = secure;

  g_tls_client_connection_copy_session_state (G_TLS_CLIENT_CONNECTION (secure),
                                              G_TLS_CLIENT_CONNECTION (conn->commands));

  g_signal_connect (secure, "accept-certificate", G_CALLBACK (cb), user_data);

  if (!g_tls_connection_handshake (G_TLS_CONNECTION (secure),
                                   cancellable,
                                   error))
    {
      /* Close here to be sure it won't get used anymore */
      g_io_stream_close (secure, cancellable, NULL);
      return FALSE;
    }

  return TRUE;
}

gboolean
g_vfs_ftp_connection_open_data_connection (GVfsFtpConnection *conn,
                                           GSocketAddress *   addr,
                                           GCancellable *     cancellable,
                                           GError **          error)
{
  g_return_val_if_fail (conn != NULL, FALSE);
  g_return_val_if_fail (conn->data == NULL, FALSE);

  g_vfs_ftp_connection_stop_listening (conn);

  conn->data = G_IO_STREAM (g_socket_client_connect (conn->client,
                                                     G_SOCKET_CONNECTABLE (addr),
                                                     cancellable,
                                                     error));
  if (conn->data)
    enable_nodelay (G_SOCKET_CONNECTION (conn->data));

  return conn->data != NULL;
}

/**
 * g_vfs_ftp_connection_listen_data_connection:
 * @conn: a connection
 * @error: %NULL or location to take potential errors
 *
 * Initiates a listening socket that the FTP server can connect to. To accept 
 * connections and initialize data transfers, use 
 * g_vfs_ftp_connection_accept_data_connection().
 * This function supports what is known as "active FTP", while
 * g_vfs_ftp_connection_open_data_connection() is to be used for "passive FTP".
 *
 * Returns: the actual address the socket is listening on or %NULL on error
 **/
GSocketAddress *
g_vfs_ftp_connection_listen_data_connection (GVfsFtpConnection *conn,
                                             GError **          error)
{
  GSocketAddress *local, *addr;

  g_return_val_if_fail (conn != NULL, NULL);
  g_return_val_if_fail (conn->data == NULL, NULL);

  g_vfs_ftp_connection_stop_listening (conn);

  local = g_socket_connection_get_local_address (conn->connection, error);
  if (local == NULL)
    return NULL;

  conn->listen_socket = g_socket_new (g_socket_address_get_family (local),
                                      G_SOCKET_TYPE_STREAM,
                                      G_SOCKET_PROTOCOL_TCP,
                                      error);
  if (conn->listen_socket == NULL)
    return NULL;

  g_assert (G_IS_INET_SOCKET_ADDRESS (local));
  addr = g_inet_socket_address_new (g_inet_socket_address_get_address (G_INET_SOCKET_ADDRESS (local)), 0);
  g_object_unref (local);

  if (!g_socket_bind (conn->listen_socket, addr, TRUE, error) ||
      !g_socket_listen (conn->listen_socket, error) ||
      !(local = g_socket_get_local_address (conn->listen_socket, error)))
    {
      g_object_unref (addr);
      g_vfs_ftp_connection_stop_listening (conn);
      return NULL;
    }

  g_object_unref (addr);
  return local;
}

static void
cancel_timer_cb (GCancellable *orig, GCancellable *to_cancel)
{
  g_cancellable_cancel (to_cancel);
}

static gboolean
cancel_cancellable (gpointer cancellable)
{
  g_cancellable_cancel (cancellable);
  return FALSE;
}

/**
 * g_vfs_ftp_connection_accept_data_connection:
 * @conn: a listening connection
 * @cancellable: cancellable to interrupt wait
 * @error: %NULL or location to take a potential error
 *
 * Opens a data connection for @conn by accepting an incoming connection on the
 * address it is listening on via g_vfs_ftp_connection_listen_data_connection(),
 * which must have been called prior to this function.
 * If this function succeeds, a data connection will have been opened, and calls
 * to g_vfs_ftp_connection_get_data_stream() will work.
 *
 * Returns: %TRUE if a connection was successfully acquired
 **/
gboolean
g_vfs_ftp_connection_accept_data_connection (GVfsFtpConnection *conn,
                                             GCancellable *     cancellable,
                                             GError **          error)
{
  GSocket *accepted;
  GCancellable *timer;
  gulong cancel_cb_id;
  GIOCondition condition;

  g_return_val_if_fail (conn != NULL, FALSE);
  g_return_val_if_fail (conn->data == NULL, FALSE);
  g_return_val_if_fail (G_IS_SOCKET (conn->listen_socket), FALSE);

  timer = g_cancellable_new ();
  cancel_cb_id = g_cancellable_connect (cancellable, 
                                        G_CALLBACK (cancel_timer_cb),
                                        timer,
                                        NULL);
  g_object_ref (timer);
  g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                              G_VFS_FTP_TIMEOUT_IN_SECONDS,
                              cancel_cancellable,
                              timer,
                              g_object_unref);

  condition = g_socket_condition_wait (conn->listen_socket, G_IO_IN, timer, error);

  g_cancellable_disconnect (cancellable, cancel_cb_id);
  g_object_unref (timer);

  if ((condition & G_IO_IN) == 0)
    {
      if (g_cancellable_is_cancelled (timer) &&
          !g_cancellable_is_cancelled (cancellable))
        {
          g_clear_error (error);
          g_set_error_literal (error, 
                               G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND,
                               _("Failed to create active FTP connection. "
                                 "Maybe your router does not support this?"));
        }
      else if (error && *error == NULL)
        {
          g_set_error_literal (error, 
                               G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND,
                               _("Failed to create active FTP connection."));
        }
      return FALSE;
    }

  accepted = g_socket_accept (conn->listen_socket, cancellable, error);
  if (accepted == NULL)
    return FALSE;

  conn->data = G_IO_STREAM (g_socket_connection_factory_create_connection (accepted));
  g_object_unref (accepted);
  enable_nodelay (G_SOCKET_CONNECTION (conn->data));
  return TRUE;
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

/**
 * g_vfs_ftp_connection_get_debug_id:
 * @conn: the connection
 *
 * Gets an ID that uniquely identifies this connection. Intended for use in 
 * debug print statements.
 *
 * Returns: the ID to use for referring to this connection in debug messages
 **/
guint
g_vfs_ftp_connection_get_debug_id (GVfsFtpConnection *conn)
{
  g_return_val_if_fail (conn != NULL, 0);

  return conn->debug_id;
}

/**
 * g_vfs_ftp_connection_get_data_stream:
 * @conn: a connection
 *
 * Gets the data stream in use by @conn. It is an error to call this function
 * when no data stream exists. Be sure to check the return value of
 * g_vfs_ftp_connection_open_data_connection().
 *
 * Returns: the data stream of @conn
 **/
GIOStream *
g_vfs_ftp_connection_get_data_stream (GVfsFtpConnection *conn)
{
  g_return_val_if_fail (conn != NULL, NULL);
  g_return_val_if_fail (conn->data != NULL, NULL);

  return conn->data;
}

/**
 * g_vfs_ftp_connection_is_usable:
 * @conn: a connection
 *
 * Checks if this connection can be used to send new commands. For
 * example, if the connection is in the process of a command sequence or
 * if the connection was closed, this is not possible and this function 
 * will return %FALSE.
 *
 * Returns: %TRUE if the connection is still usable
 **/
gboolean
g_vfs_ftp_connection_is_usable (GVfsFtpConnection *conn)
{
  GIOCondition cond;

  g_return_val_if_fail (conn != NULL, FALSE);

  if (conn->waiting_for_reply)
    return FALSE;

  cond = G_IO_ERR | G_IO_HUP | G_IO_IN;
  cond = g_socket_condition_check (g_socket_connection_get_socket (conn->connection), cond);
  if (cond)
    {
      g_debug ("##%2d ##  connection unusuable: %s%s%s\r\n", conn->debug_id,
                                                             cond & G_IO_IN ? "IN " : "",
                                                             cond & G_IO_HUP ? "HUP " : "",
                                                             cond & G_IO_ERR ? "ERR " : "");
      return FALSE;
    }

  return TRUE;
}

/**
 * g_vfs_ftp_connection_enable_tls:
 * @conn: a connection without an active data connection
 * @server_identity: address of the server used to verify the certificate
 * @cb: callback called if there's a verification error
 * @user_data: user data passed to @cb
 * @cancellable: cancellable to interrupt wait
 * @error: %NULL or location to take a potential error
 *
 * Tries to enable TLS on the given @connection. If setting up TLS fails,
 * %FALSE will be returned and @error will be set. When this function fails,
 * you need to check if the connection is still usable. It might have been
 * closed.
 *
 * Returns: %TRUE on success, %FALSE otherwise.
 **/
gboolean
g_vfs_ftp_connection_enable_tls (GVfsFtpConnection * conn,
                                 GSocketConnectable *server_identity,
                                 gboolean            implicit_tls,
                                 CertificateCallback cb,
                                 gpointer            user_data,
                                 GCancellable *      cancellable,
                                 GError **           error)
{
  GIOStream *secure;

  g_return_val_if_fail (conn != NULL, FALSE);
  g_return_val_if_fail (conn->data == NULL, FALSE);
  g_return_val_if_fail (implicit_tls || !conn->waiting_for_reply, FALSE);
  g_return_val_if_fail (g_buffered_input_stream_get_available (G_BUFFERED_INPUT_STREAM (conn->commands_in)) == 0, FALSE);

  secure = g_tls_client_connection_new (conn->commands,
                                        server_identity,
                                        error);
  if (secure == NULL)
    return FALSE;

  g_object_unref (conn->commands);
  conn->commands = secure;
  create_input_stream (conn);

  g_signal_connect (secure, "accept-certificate", G_CALLBACK (cb), user_data);

  if (!g_tls_connection_handshake (G_TLS_CONNECTION (secure),
                                   cancellable,
                                   error))
    {
      /* Close here to be sure it won't get used anymore */
      g_io_stream_close (secure, cancellable, NULL);
      return FALSE;
    }

  return TRUE;
}
