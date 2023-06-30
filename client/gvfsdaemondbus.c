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
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>

#include <gio/gio.h>
#include "gvfsdaemondbus.h"
#include <gvfsdaemonprotocol.h>
#include <gdaemonvfs.h>
#include <gvfsdbus.h>
#include <gvfsutils.h>

/* Extra vfs-specific data for GDBusConnections */
typedef struct {
  char *async_dbus_id;
} VfsConnectionData;

typedef struct _ThreadLocalConnections ThreadLocalConnections;
static void free_local_connections (ThreadLocalConnections *local);
static void invalidate_local_connection (const char *dbus_id, GError **error);

static GPrivate local_connections = G_PRIVATE_INIT((GDestroyNotify)free_local_connections);

/* dbus id -> async connection */
static GHashTable *async_map = NULL;
G_LOCK_DEFINE_STATIC(async_map);


GQuark
_g_vfs_error_quark (void)
{
  return g_quark_from_static_string ("g-vfs-error-quark");
}

static void
connection_data_free (gpointer p)
{
  VfsConnectionData *data = p;

  g_free (data->async_dbus_id);
  g_free (data);
}

static void
vfs_connection_closed (GDBusConnection *connection,
                       gboolean remote_peer_vanished,
                       GError *error,
                       gpointer user_data)
{
  VfsConnectionData *connection_data;

  connection_data = g_object_get_data (G_OBJECT (connection), "connection_data");
  g_assert (connection_data != NULL);

  if (connection_data->async_dbus_id)
    {
      _g_daemon_vfs_invalidate (connection_data->async_dbus_id, NULL);
      G_LOCK (async_map);
      g_hash_table_remove (async_map, connection_data->async_dbus_id);
      G_UNLOCK (async_map);
    }
}

static void
vfs_connection_setup (GDBusConnection *connection,
		      gboolean async)
{
  VfsConnectionData *connection_data;

  connection_data = g_new0 (VfsConnectionData, 1);
  
  g_object_set_data_full (G_OBJECT (connection), "connection_data", connection_data, connection_data_free);

  g_signal_connect (connection, "closed", G_CALLBACK (vfs_connection_closed), NULL);
}

/*******************************************************************
 *                Caching of async connections                     *
 *******************************************************************/


static GDBusConnection *
get_connection_for_async (const char *dbus_id)
{
  GDBusConnection *connection;

  connection = NULL;
  G_LOCK (async_map);
  if (async_map != NULL)
    connection = g_hash_table_lookup (async_map, dbus_id);
  if (connection)
    g_object_ref (connection);
  G_UNLOCK (async_map);
  
  return connection;
}

static void
close_and_unref_connection (void *data)
{
  GDBusConnection *connection = G_DBUS_CONNECTION (data);
  
  /* TODO: watch for the need to manually call g_dbus_connection_close_sync () */
  g_object_unref (connection);
}

static void
set_connection_for_async (GDBusConnection *connection, const char *dbus_id)
{
  VfsConnectionData *data;
  
  G_LOCK (async_map);
  data = g_object_get_data (G_OBJECT (connection), "connection_data");
  g_assert (data != NULL);
  data->async_dbus_id = g_strdup (dbus_id);

  if (async_map == NULL)
    async_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, close_and_unref_connection);
      
  g_hash_table_insert (async_map, g_strdup (dbus_id), connection);
  g_object_ref (connection);
  G_UNLOCK (async_map);
}

/**************************************************************************
 *                 Asynchronous daemon calls                              *
 *************************************************************************/

typedef struct {
  char *dbus_id;

  GVfsDBusDaemon *proxy;
  GDBusConnection *connection;
  GCancellable *cancellable;

  GVfsAsyncDBusCallback callback;
  gpointer callback_data;
  
  GError *io_error;
  gulong cancelled_tag;
} AsyncDBusCall;

static void
async_call_finish (AsyncDBusCall *async_call)
{
  if (async_call->callback)
    async_call->callback (async_call->io_error ? NULL : async_call->connection,
			  async_call->io_error, 
			  async_call->callback_data);

  g_clear_object (&async_call->proxy);
  g_clear_object (&async_call->connection);
  g_clear_object (&async_call->cancellable);
  g_clear_error (&async_call->io_error);
  g_free (async_call->dbus_id);
  g_free (async_call);
}

static void
async_got_private_connection_cb (GObject *source_object,
                                 GAsyncResult *res,
                                 gpointer user_data)
{
  AsyncDBusCall *async_call = user_data;
  GDBusConnection *connection, *existing_connection;
  GError *error = NULL;
  
  connection = g_dbus_connection_new_for_address_finish (res, &error);
  if (!connection)
    {
      async_call->io_error = g_error_copy (error);
      g_error_free (error);
      async_call_finish (async_call);
      return;
    }

  vfs_connection_setup (connection, TRUE);
  
  /* Maybe we already had a connection? This happens if we requested
   * the same owner several times in parallel.
   * If so, just drop this connection and use that.
   */
  
  existing_connection = get_connection_for_async (async_call->dbus_id);
  if (existing_connection != NULL)
    {
      async_call->connection = existing_connection;
      /* TODO: watch for the need to manually call g_dbus_connection_close_sync () */
      g_object_unref (connection);
    }
  else
    {  
      set_connection_for_async (connection, async_call->dbus_id);
      async_call->connection = connection;
    }

  /* Maybe we were canceled while setting up connection, then
   * avoid doing the operation */
  if (g_cancellable_set_error_if_cancelled (async_call->cancellable, &async_call->io_error))
    {
      async_call_finish (async_call);
      return;
    }

  async_call_finish (async_call);
}

static void
async_get_connection_response (GVfsDBusDaemon *proxy,
                               GAsyncResult *res,
                               gpointer user_data)
{
  AsyncDBusCall *async_call = user_data;
  GError *error = NULL;
  gchar *address1 = NULL;

  if (! gvfs_dbus_daemon_call_get_connection_finish (proxy,
                                                     &address1, NULL,
                                                     res,
                                                     &error))
    {
      /* If the error indicates that the dbus_id is invalid,
       * we invalidate the caches, and then caller needs to retry.
       */
      if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
        invalidate_local_connection (async_call->dbus_id, &async_call->io_error);
      else
        async_call->io_error = g_error_copy (error);
      g_error_free (error);
      g_free (address1);
      async_call_finish (async_call);
      return;
    }
  
  g_dbus_connection_new_for_address (address1,
                                     G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                     NULL, /* GDBusAuthObserver */
                                     async_call->cancellable,
                                     async_got_private_connection_cb,
                                     async_call);
  g_free (address1);
}

static void
socket_dir_query_info_cb (GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
  AsyncDBusCall *async_call = user_data;
  g_autoptr (GFileInfo) socket_dir_info = NULL;

  socket_dir_info = g_file_query_info_finish (G_FILE (source_object),
                                              res,
                                              &async_call->io_error);
  if (socket_dir_info == NULL ||
      !g_file_info_get_attribute_boolean (socket_dir_info,
                                          G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
    {
      if (!async_call->io_error)
        async_call->io_error = g_error_new_literal (G_IO_ERROR,
                                                    G_IO_ERROR_PERMISSION_DENIED,
                                                    _("Permission denied"));

      async_call_finish (async_call);
      return;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (async_call->proxy), G_VFS_DBUS_TIMEOUT_MSECS);

  gvfs_dbus_daemon_call_get_connection (async_call->proxy,
                                        async_call->cancellable,
                                        (GAsyncReadyCallback) async_get_connection_response,
                                        async_call);
}

static void
open_connection_async_cb (GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
  AsyncDBusCall *async_call = user_data;
  GError *error = NULL;
  g_autofree gchar *socket_dir_path = NULL;
  g_autoptr (GFile) socket_dir = NULL;

  async_call->proxy = gvfs_dbus_daemon_proxy_new_finish (res, &error);
  if (async_call->proxy == NULL)
    {
      async_call->io_error = g_error_copy (error);
      g_error_free (error);
      async_call_finish (async_call);
      return;
    }

  /* This is needed to prevent socket leaks. */
  socket_dir_path = gvfs_get_socket_dir ();
  socket_dir = g_file_new_for_path (socket_dir_path);
  g_file_query_info_async (socket_dir,
                           G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
                           G_FILE_QUERY_INFO_NONE,
                           G_PRIORITY_DEFAULT,
                           async_call->cancellable,
                           socket_dir_query_info_cb,
                           user_data);
}

static void
open_connection_async (AsyncDBusCall *async_call)
{
  gvfs_dbus_daemon_proxy_new (_g_daemon_vfs_get_async_bus (),
                              G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                              async_call->dbus_id,
                              G_VFS_DBUS_DAEMON_PATH,
                              async_call->cancellable,
                              open_connection_async_cb,
                              async_call);
}

void
_g_dbus_connection_get_for_async (const char *dbus_id,
                                  GVfsAsyncDBusCallback callback,
                                  gpointer callback_data,
                                  GCancellable *cancellable)
{
  AsyncDBusCall *async_call;

  async_call = g_new0 (AsyncDBusCall, 1);
  async_call->dbus_id = g_strdup (dbus_id);
  if (cancellable)
    async_call->cancellable = g_object_ref (cancellable);
  async_call->callback = callback;
  async_call->callback_data = callback_data;

  async_call->connection = get_connection_for_async (async_call->dbus_id);
  if (async_call->connection == NULL)
    open_connection_async (async_call);
  else
    {
      if (g_dbus_connection_is_closed (async_call->connection))
        {
          /* The mount for this connection died, we invalidate
           * the caches, and then caller needs to retry.
           */
          invalidate_local_connection (async_call->dbus_id, &async_call->io_error);
          async_call->connection = NULL;
        }
      async_call_finish (async_call);
    }
}


typedef struct {
  GDBusConnection *connection;
  guint32 serial;
} AsyncCallCancelData;

static void
async_call_cancel_data_free (gpointer _data)
{
  AsyncCallCancelData *data = _data;

  g_object_unref (data->connection);
  g_free (data);
}

static void
cancelled_got_proxy (GObject *source_object,
                     GAsyncResult *res,
                     gpointer user_data)
{
  guint32 serial = GPOINTER_TO_UINT (user_data);
  GVfsDBusDaemon *proxy;
  GError *error = NULL;

  proxy = gvfs_dbus_daemon_proxy_new_finish (res, &error);
  if (! proxy)
    {
      g_printerr ("Failed to construct daemon proxy for cancellation: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      return;
    }

  gvfs_dbus_daemon_call_cancel (proxy,
                                serial,
                                NULL,
                                NULL,  /* we don't need any reply */
                                NULL);
  g_object_unref (proxy);
}

static gboolean
async_call_cancelled_cb_on_idle (gpointer _data)
{
  AsyncCallCancelData *data = _data;

  /* TODO: use shared daemon proxy on private connection if possible */
  gvfs_dbus_daemon_proxy_new (data->connection,
                              G_DBUS_PROXY_FLAGS_NONE,
                              NULL,
                              G_VFS_DBUS_DAEMON_PATH,
                              NULL,
                              cancelled_got_proxy,
                              GUINT_TO_POINTER (data->serial));  /* not passing "data" in as long it may not exist anymore between async calls */

  return FALSE;
}

/* Might be called on another thread */
static void
async_call_cancelled_cb (GCancellable *cancellable,
                         gpointer _data)
{
  AsyncCallCancelData *data = _data;
  AsyncCallCancelData *idle_data;

  idle_data = g_new0 (AsyncCallCancelData, 1);
  idle_data->connection = g_object_ref (data->connection);
  idle_data->serial = data->serial;

  /* Call on idle to not block g_cancellable_disconnect() as it causes deadlocks
   * in gdbus codes, see: https://gitlab.gnome.org/GNOME/glib/issues/1023.
   */
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                   async_call_cancelled_cb_on_idle,
                   idle_data,
                   async_call_cancel_data_free);
}

gulong
_g_dbus_async_subscribe_cancellable (GDBusConnection *connection, GCancellable *cancellable)
{
  AsyncCallCancelData *cancel_data;
  gulong cancelled_tag = 0;

  if (cancellable)
    {
      cancel_data = g_new0 (AsyncCallCancelData, 1);
      cancel_data->connection = g_object_ref (connection);
      /* make sure we get the serial *after* the message has been sent, otherwise
       * it will be 0
       */
      cancel_data->serial = g_dbus_connection_get_last_serial (connection);
      cancelled_tag =
        g_signal_connect_data (cancellable, "cancelled",
                               G_CALLBACK (async_call_cancelled_cb),
                               cancel_data,
                               (GClosureNotify)async_call_cancel_data_free,
                               0);
    }

  return cancelled_tag;
}

void
_g_dbus_async_unsubscribe_cancellable (GCancellable *cancellable, gulong cancelled_tag)
{
  if (cancelled_tag)
    {
      g_assert (cancellable != NULL);
      g_signal_handler_disconnect (cancellable, cancelled_tag);
    }
}

void
_g_dbus_send_cancelled_with_serial_sync (GDBusConnection *connection,
                                         guint32 serial)
{
  GVfsDBusDaemon *proxy;
  GError *error = NULL;

  proxy = gvfs_dbus_daemon_proxy_new_sync (connection,
                                           G_DBUS_PROXY_FLAGS_NONE,
                                           NULL,
                                           G_VFS_DBUS_DAEMON_PATH,
                                           NULL,
                                           &error);
  if (! proxy)
    {
      g_printerr ("Failed to construct daemon proxy for cancellation: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      return;
    }

  gvfs_dbus_daemon_call_cancel (proxy,
                                serial,
                                NULL,
                                NULL,  /* we don't need any reply */
                                NULL);
  g_object_unref (proxy);
}

void
_g_dbus_send_cancelled_sync (GDBusConnection *connection)
{
  _g_dbus_send_cancelled_with_serial_sync (connection,
                                           g_dbus_connection_get_last_serial (connection));
}


/*************************************************************************
 *               get per-thread synchronous dbus connections             *
 *************************************************************************/

struct _ThreadLocalConnections {
  GHashTable *connections;
  GDBusConnection *session_bus;
};

static void
free_local_connections (ThreadLocalConnections *local)
{
  g_hash_table_destroy (local->connections);
  g_clear_object (&local->session_bus);
  g_free (local);
}

static void
invalidate_local_connection (const char *dbus_id,
			     GError **error)
{
  ThreadLocalConnections *local;
  
  _g_daemon_vfs_invalidate (dbus_id, NULL);

  local = g_private_get (&local_connections);
  if (local)
    g_hash_table_remove (local->connections, dbus_id);
  
  g_set_error_literal (error,
		       G_VFS_ERROR,
		       G_VFS_ERROR_RETRY,
		       "Cache invalid, retry (internally handled)");
}

GDBusConnection *
_g_dbus_connection_get_sync (const char *dbus_id,
                             GCancellable *cancellable,
			     GError **error)
{
  GDBusConnection *bus;
  ThreadLocalConnections *local;
  GError *local_error;
  GDBusConnection *connection;
  gchar *address1;
  GVfsDBusDaemon *daemon_proxy;
  gboolean res;
  g_autofree gchar *socket_dir_path = NULL;
  g_autoptr (GFile) socket_dir = NULL;
  g_autoptr (GFileInfo) socket_dir_info = NULL;

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;

  local = g_private_get (&local_connections);
  if (local == NULL)
    {
      local = g_new0 (ThreadLocalConnections, 1);
      local->connections = g_hash_table_new_full (g_str_hash, g_str_equal,
						  g_free, (GDestroyNotify)g_object_unref);
      g_private_set (&local_connections, local);
    }

  if (dbus_id == NULL)
    {
      /* Session bus */
      
      if (local->session_bus)
	{
	  if (! g_dbus_connection_is_closed (local->session_bus))
	    return local->session_bus;

	  /* Session bus was disconnected, re-connect */
	  g_object_unref (local->session_bus);
	  local->session_bus = NULL;
	}
    }
  else
    {
      /* Mount daemon connection */
      
      connection = g_hash_table_lookup (local->connections, dbus_id);
      if (connection != NULL)
	{
	  if (g_dbus_connection_is_closed (connection))
	    {
	      /* The mount for this connection died, we invalidate
	       * the caches, and then caller needs to retry.
	       */

	      invalidate_local_connection (dbus_id, error);
	      return NULL;
	    }
	  
	  return connection;
	}
    }

  if (local->session_bus == NULL)
    {
      bus = g_bus_get_sync (G_BUS_TYPE_SESSION, cancellable, error);
      if (bus == NULL)
        return NULL;
      
      local->session_bus = bus;

      if (dbus_id == NULL)
	return bus; /* We actually wanted the session bus, so done */
    }

  daemon_proxy = gvfs_dbus_daemon_proxy_new_sync (local->session_bus,
                                                  G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                                  dbus_id,
                                                  G_VFS_DBUS_DAEMON_PATH,
                                                  cancellable,
                                                  error);
  if (daemon_proxy == NULL)
    return NULL;

  /* This is needed to prevent socket leaks. */
  socket_dir_path = gvfs_get_socket_dir ();
  socket_dir = g_file_new_for_path (socket_dir_path);
  socket_dir_info = g_file_query_info (socket_dir,
                                       G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
                                       G_FILE_QUERY_INFO_NONE,
                                       cancellable,
                                       error);
  if (socket_dir_info == NULL ||
      !g_file_info_get_attribute_boolean (socket_dir_info,
                                          G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE))
    {
      if (error && !*error)
        *error = g_error_new_literal (G_IO_ERROR,
                                      G_IO_ERROR_PERMISSION_DENIED,
                                      _("Permission denied"));

      return NULL;
    }

  local_error = NULL;
  address1 = NULL;
  res = gvfs_dbus_daemon_call_get_connection_sync (daemon_proxy,
                                                   &address1,
                                                   NULL,
                                                   cancellable,
                                                   &local_error);
  g_object_unref (daemon_proxy);

  if (!res)
    {
      /* If the error indicates that the dbus_id is invalid,
       * we invalidate the caches, and then caller needs to retry.
       */
      if (g_error_matches (local_error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
        {
          invalidate_local_connection (dbus_id, error);
          g_error_free (local_error);
        }
      else
        g_propagate_error (error, local_error);
      g_free (address1);
      return NULL;
    }


  local_error = NULL;
  connection = g_dbus_connection_new_for_address_sync (address1,
                                                       G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                                       NULL, /* GDBusAuthObserver */
                                                       cancellable,
                                                       &local_error);
  g_free (address1);

  if (!connection)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "Error while getting peer-to-peer dbus connection: %s",
		   local_error->message);
      g_error_free (local_error);
      return NULL;
    }

  vfs_connection_setup (connection, FALSE);

  g_hash_table_insert (local->connections, g_strdup (dbus_id), connection);

  return connection;
}

void
_g_propagate_error_stripped (GError **dest, GError *src)
{
  g_propagate_error (dest, src);
  if (dest && *dest)
    g_dbus_error_strip_remote_error (*dest);
}
