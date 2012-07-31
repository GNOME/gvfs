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

#include <gio/gio.h>
#include "gvfsdaemondbus.h"
#include <gvfsdaemonprotocol.h>
#include <gdaemonvfs.h>
#include "gvfsdbusutils.h"
#include "gsysutils.h"
#include <gvfsdbus.h>

/* Extra vfs-specific data for DBusConnections */
typedef struct {
  int extra_fd;
  int extra_fd_count;
  char *async_dbus_id;
  
  /* Only used for async connections */
  GHashTable *outstanding_fds;
  GSource *extra_fd_source;
} VfsConnectionData;

typedef struct _ThreadLocalConnections ThreadLocalConnections;
static void free_local_connections (ThreadLocalConnections *local);

static GPrivate local_connections = G_PRIVATE_INIT((GDestroyNotify)free_local_connections);

/* dbus id -> async connection */
static GHashTable *async_map = NULL;
G_LOCK_DEFINE_STATIC(async_map);

/* dbus object path -> dbus message filter */
static GHashTable *obj_path_map = NULL;
G_LOCK_DEFINE_STATIC(obj_path_map);


GQuark
_g_vfs_error_quark (void)
{
  return g_quark_from_static_string ("g-vfs-error-quark");
}

/**************************************************************************
 *               message filters for vfs dbus connections                 *
 *************************************************************************/

typedef struct {
  GVfsRegisterVfsFilterCallback callback;
  GObject *data;
  GHashTable *skeletons;
} PathMapEntry;

static void
free_path_map_entry (PathMapEntry *entry)
{
  g_hash_table_destroy (entry->skeletons);
  g_free (entry);
}

static void
unref_skeleton (gpointer object)
{
  GDBusInterfaceSkeleton *skeleton = object;

  g_print ("unref_skeleton: unreffing skeleton %p\n", skeleton);
  g_dbus_interface_skeleton_unexport (skeleton);
  g_object_unref (skeleton);
}

/*  Please note the obj_path has to be unique even for different interfaces  */
void
_g_dbus_register_vfs_filter (const char *obj_path,
                             GVfsRegisterVfsFilterCallback callback,
			     GObject *data)
{
  PathMapEntry *entry;
  
  g_print ("_g_dbus_register_vfs_filter: obj_path = '%s'\n", obj_path);

  G_LOCK (obj_path_map);
  
  if (obj_path_map == NULL)
    obj_path_map = g_hash_table_new_full (g_str_hash, g_str_equal,
					  g_free, (GDestroyNotify)free_path_map_entry);

  entry = g_new (PathMapEntry, 1);
  entry->callback = callback;
  entry->data = data;
  entry->skeletons = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)unref_skeleton);

  g_hash_table_insert (obj_path_map, g_strdup (obj_path), entry);
  
  G_UNLOCK (obj_path_map);
}

void
_g_dbus_unregister_vfs_filter (const char *obj_path)
{
  G_LOCK (obj_path_map);

  if (obj_path_map)
      g_hash_table_remove (obj_path_map, obj_path);
  
  G_UNLOCK (obj_path_map);
}

static void
register_skeleton (const char *obj_path,
                   PathMapEntry *entry,
                   GDBusConnection *dbus_conn)
{
  GDBusInterfaceSkeleton *skeleton;

  if (! g_hash_table_contains (entry->skeletons, dbus_conn))
    {
      /* Note that the newly created GDBusInterfaceSkeleton instance refs the connection so it's not needed to watch for connection being destroyed */ 
      skeleton = entry->callback (dbus_conn, obj_path, entry->data);
      g_print ("registering interface skeleton %p for path '%s' on the %p connection\n", skeleton, obj_path, dbus_conn);
      
      g_hash_table_insert (entry->skeletons, dbus_conn, skeleton);
    }
  else
    {
      g_print ("interface skeleton '%s' already registered on the %p connection, skipping\n", obj_path, dbus_conn);
    }
}

/*
 *  Export registered interface skeletons on this connection. This should ideally
 *  be called only once for every connection, but we often share/reuse existing
 *  connections.
 *  
 *  Already exported interface skeletons should live as long as possible
 *  since there might be ongoing data transfer and re-exporting (unreffing + creation)
 *  breaks running jobs randomly.
 *  
 */
void
_g_dbus_connect_vfs_filters (GDBusConnection *connection)
{
  g_print ("_g_dbus_connect_vfs_filters: connection = %p\n", connection);
  
  G_LOCK (obj_path_map);

  if (obj_path_map)
    {
      /* Export new interface skeletons */
      g_hash_table_foreach (obj_path_map, (GHFunc) register_skeleton, connection);
    }

  G_UNLOCK (obj_path_map);
}

static void
connection_data_free (gpointer p)
{
  VfsConnectionData *data = p;

  if (data->extra_fd != -1)
    close (data->extra_fd);

  if (data->extra_fd_source)
    {
      g_source_destroy (data->extra_fd_source);
      g_source_unref (data->extra_fd_source);
    }

  if (data->outstanding_fds)
    g_hash_table_destroy (data->outstanding_fds);

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

  g_print ("gvfsdaemondbus.c: vfs_connection_closed()\n");
  
  connection_data = g_object_get_data (G_OBJECT (connection), "connection_data");
  g_assert (connection_data != NULL);

  g_print ("   async_dbus_id = '%s'\n", connection_data->async_dbus_id);

  if (connection_data->async_dbus_id)
    {
      _g_daemon_vfs_invalidate_dbus_id (connection_data->async_dbus_id);
      G_LOCK (async_map);
      g_hash_table_remove (async_map, connection_data->async_dbus_id);
      G_UNLOCK (async_map);
    }
}

static void
vfs_connection_setup (GDBusConnection *connection,
		      int extra_fd,
		      gboolean async)
{
  VfsConnectionData *connection_data;

  connection_data = g_new0 (VfsConnectionData, 1);
  connection_data->extra_fd = extra_fd;
  connection_data->extra_fd_count = 0;
  
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
  GDBusConnection *connection = data;
  
  g_print ("close_and_unref_connection: closing connection\n");
  
  /* TODO: watch for the need to manually call g_dbus_connection_close_sync () */
  g_object_unref (connection);
}

static void
set_connection_for_async (GDBusConnection *connection, const char *dbus_id)
{
  VfsConnectionData *data;
  
  g_print ("set_connection_for_async: connection = %p, dbus_id = '%s'\n", connection, dbus_id);
  
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
  const char *dbus_id;

  GDBusConnection *connection;
  int extra_fd;
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

  if (async_call->connection)
    g_object_unref (async_call->connection);
  if (async_call->cancellable)
    g_object_unref (async_call->cancellable);
  if (async_call->io_error)
    g_error_free (async_call->io_error);
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
  g_print ("async_got_private_connection_cb, connection = %p\n", connection);
  if (!connection)
    {
      close (async_call->extra_fd);
      async_call->io_error = g_error_copy (error);
      g_error_free (error);
      async_call_finish (async_call);
      return;
    }

  vfs_connection_setup (connection, async_call->extra_fd, TRUE);
  
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
  gchar *address1, *address2;

  g_print ("async_get_connection_response\n");

  if (! gvfs_dbus_daemon_call_get_connection_finish (proxy,
                                                     &address1, &address2,
                                                     res,
                                                     &error))
    {
      async_call->io_error = g_error_copy (error);
      g_error_free (error);
      async_call_finish (async_call);
      return;
    }
  
  /* I don't know of any way to do an async connect */
  error = NULL;
  async_call->extra_fd = _g_socket_connect (address2, &error);
  if (async_call->extra_fd == -1)
    {
      g_set_error (&async_call->io_error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Error connecting to daemon: %s"), error->message);
      g_error_free (error);
      async_call_finish (async_call);
      return;
    }
  
  g_dbus_connection_new_for_address (address1,
                                     G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                     NULL, /* GDBusAuthObserver */
                                     async_call->cancellable,
                                     async_got_private_connection_cb,
                                     async_call);
}

static void
open_connection_async_cb (GObject *source_object,
                          GAsyncResult *res,
                          gpointer user_data)
{
  GVfsDBusDaemon *proxy;
  AsyncDBusCall *async_call = user_data;
  GError *error = NULL;
 
  proxy = gvfs_dbus_daemon_proxy_new_finish (res, &error);
  g_print ("open_connection_async_cb, proxy = %p\n", proxy);
  
  if (proxy == NULL)
    {
      async_call->io_error = g_error_copy (error);
      g_error_free (error);
      async_call_finish (async_call);
      return;
    }
  
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), G_VFS_DBUS_TIMEOUT_MSECS);
  
  gvfs_dbus_daemon_call_get_connection (proxy,
                                        async_call->cancellable,
                                        (GAsyncReadyCallback) async_get_connection_response,
                                        async_call);
  
  g_object_unref (proxy);
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

  g_print ("_g_dbus_connection_get_for_async\n");
  
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
      g_print ("got connection from cache\n");
      async_call_finish (async_call);
    }
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
  g_print ("free_local_connections()\n");
  g_hash_table_destroy (local->connections);
  if (local->session_bus)
    g_object_unref (local->session_bus);
  g_free (local);
}

static void
invalidate_local_connection (const char *dbus_id,
			     GError **error)
{
  ThreadLocalConnections *local;
  
  _g_daemon_vfs_invalidate_dbus_id (dbus_id);

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
  gchar *address1, *address2;
  int extra_fd;
  GVfsDBusDaemon *daemon_proxy;
  gboolean res;

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

  address1 = address2 = NULL;
  daemon_proxy = gvfs_dbus_daemon_proxy_new_sync (local->session_bus,
                                                  G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                                  dbus_id,
                                                  G_VFS_DBUS_DAEMON_PATH,
                                                  cancellable,
                                                  error);
  if (daemon_proxy == NULL)
    return NULL;

  res = gvfs_dbus_daemon_call_get_connection_sync (daemon_proxy,
                                                   &address1,
                                                   &address2,
                                                   cancellable,
                                                   error);
  g_object_unref (daemon_proxy);

  if (!res)
    return NULL;

  local_error = NULL;
  extra_fd = _g_socket_connect (address2, &local_error);
  if (extra_fd == -1)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("Error connecting to daemon: %s"), local_error->message);
      g_error_free (local_error);
      g_free (address1);
      g_free (address2);
      return NULL;
    }

  connection = g_dbus_connection_new_for_address_sync (address1,
                                                       G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
                                                       NULL, /* GDBusAuthObserver */
                                                       cancellable,
                                                       &local_error);
  if (!connection)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "Error while getting peer-to-peer dbus connection: %s",
		   local_error->message);
      close (extra_fd);
      g_error_free (local_error);
      g_free (address1);
      g_free (address2);
      return NULL;
    }

  vfs_connection_setup (connection, extra_fd, FALSE);

  g_hash_table_insert (local->connections, g_strdup (dbus_id), connection);

  return connection;
}

/**
 * _g_simple_async_result_complete_with_cancellable:
 * @result: the result
 * @cancellable: a cancellable to check
 *
 * If @cancellable is cancelled, sets @result into the cancelled error
 * state. Then calls g_simple_async_result_complete().
 * This function is useful to ensure that @result is properly set into
 * an error state on cancellation.
 **/
void
_g_simple_async_result_complete_with_cancellable (GSimpleAsyncResult *result,
                                                  GCancellable       *cancellable)
{
  if (cancellable &&
      g_cancellable_is_cancelled (cancellable))
    g_simple_async_result_set_error (result,
                                     G_IO_ERROR,
                                     G_IO_ERROR_CANCELLED,
                                     "%s", _("Operation was cancelled"));

  g_simple_async_result_complete (result);
}

