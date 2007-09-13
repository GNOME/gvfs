#include <config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>

#include <glib/gi18n-lib.h>

#include "gvfserror.h"
#include "gasynchelper.h"
#include "gvfsdaemondbus.h"
#include <gvfsdaemonprotocol.h>
#include "gdbusutils.h"
#include "gsysutils.h"

#define DBUS_TIMEOUT_DEFAULT 30 * 1000 /* 1/2 min */

typedef struct {
  GHashTable *connections;
} ThreadLocalConnections;

typedef struct {
  int fd;
  GetFdAsyncCallback callback;
  gpointer callback_data;
} OutstandingFD;

typedef struct {
  int extra_fd;
  int extra_fd_count;
  GHashTable *outstanding_fds;
  GSource *extra_fd_source;
} VfsConnectionData;

typedef struct DBusSource DBusSource;

static gint32 vfs_data_slot = -1;
static GOnce once_init_dbus = G_ONCE_INIT;

static GStaticPrivate local_connections = G_STATIC_PRIVATE_INIT;

/* bus name -> dbus id */
static GHashTable *bus_name_map = NULL;
G_LOCK_DEFINE_STATIC(bus_name_map);

/* dbus id -> async connection */
static GHashTable *owner_map = NULL;
G_LOCK_DEFINE_STATIC(owner_map);

static GHashTable *obj_path_map = NULL;
G_LOCK_DEFINE_STATIC(obj_path_map);


static DBusConnection *get_connection_for_owner             (const char      *owner);
static DBusConnection *get_connection_sync                  (const char      *bus_name,
							     GError         **error);

static gpointer
vfs_dbus_init (gpointer arg)
{
  if (!dbus_connection_allocate_data_slot (&vfs_data_slot))
    g_error ("Unable to allocate data slot");

  return NULL;
}

typedef struct {
  DBusHandleMessageFunction callback;
  GObject *data;
} PathMapEntry;

void
_g_dbus_register_vfs_filter (const char *obj_path,
			     DBusHandleMessageFunction callback,
			     GObject *data)
{
  PathMapEntry * entry;
  
  G_LOCK (obj_path_map);
  
  if (obj_path_map == NULL)
    obj_path_map = g_hash_table_new_full (g_str_hash, g_str_equal,
					  g_free, g_free);

  entry = g_new (PathMapEntry,1 );
  entry->callback = callback;
  entry->data = data;

  g_hash_table_insert  (obj_path_map, g_strdup (obj_path), entry);
  
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

static DBusHandlerResult
vfs_connection_filter (DBusConnection     *connection,
		       DBusMessage        *message,
		       void               *user_data)
{
  PathMapEntry *entry;
  DBusHandlerResult res;
  DBusHandleMessageFunction callback;
  GObject *data;

  callback = NULL;
  data = NULL;
  G_LOCK (obj_path_map);
  if (obj_path_map)
    {
      entry = g_hash_table_lookup (obj_path_map,
				   dbus_message_get_path (message));

      if (entry)
	{
	  callback = entry->callback;
	  data = g_object_ref (entry->data);
	}
    }
  G_UNLOCK (obj_path_map);

  res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  if (callback)
    {
      res = callback (connection, message, data);
      g_object_unref (data);
    }

  return res;
}

static void
outstanding_fd_free (OutstandingFD *outstanding)
{
  if (outstanding->fd != -1)
    close (outstanding->fd);

  g_free (outstanding);
}

static void
connection_data_free (gpointer p)
{
  VfsConnectionData *data = p;
  
  close (data->extra_fd);

  if (data->extra_fd_source)
    {
      g_source_destroy (data->extra_fd_source);
      g_source_unref (data->extra_fd_source);
    }
  g_free (data);
}


static void
accept_new_fd (VfsConnectionData *data,
	       GIOCondition condition,
	       int fd)
{
  int new_fd;
  int fd_id;
  OutstandingFD *outstanding_fd;

  
  fd_id = data->extra_fd_count;
  new_fd = _g_socket_receive_fd (data->extra_fd);
  if (new_fd != -1)
    {
      data->extra_fd_count++;

      outstanding_fd = g_hash_table_lookup (data->outstanding_fds, GINT_TO_POINTER (fd_id));
      
      if (outstanding_fd)
	{
	  outstanding_fd->callback (new_fd, outstanding_fd->callback_data);
	  g_hash_table_remove (data->outstanding_fds, GINT_TO_POINTER (fd_id));
	}
      else
	{
	  outstanding_fd = g_new0 (OutstandingFD, 1);
	  outstanding_fd->fd = new_fd;
	  outstanding_fd->callback = NULL;
	  outstanding_fd->callback_data = NULL;
	  g_hash_table_insert (data->outstanding_fds,
			       GINT_TO_POINTER (fd_id),
			       outstanding_fd);
	}
    }
}

static void
vfs_connection_setup (DBusConnection *connection,
		      int extra_fd,
		      gboolean async)
{
  VfsConnectionData *connection_data;
  
  connection_data = g_new (VfsConnectionData, 1);
  connection_data->extra_fd = extra_fd;
  connection_data->extra_fd_count = 0;

  if (async)
    {
      connection_data->outstanding_fds =
	g_hash_table_new_full (g_direct_hash,
			       g_direct_equal,
			       NULL,
			       (GDestroyNotify)outstanding_fd_free);


      connection_data->extra_fd_source =
	_g_fd_source_new (extra_fd, POLLIN, NULL);
      g_source_set_callback (connection_data->extra_fd_source,
			     (GSourceFunc)accept_new_fd, connection_data, NULL);
      
    }
  
  if (!dbus_connection_set_data (connection, vfs_data_slot, connection_data, connection_data_free))
    _g_dbus_oom ();

  if (!dbus_connection_add_filter (connection, vfs_connection_filter, NULL, NULL))
    _g_dbus_oom ();
}


static char *
get_owner_for_bus_name (const char *bus_name)
{
  char *owner = NULL;
  
  G_LOCK (bus_name_map);
  if (bus_name_map != NULL)
    {
      owner = g_hash_table_lookup (bus_name_map, bus_name);
      owner = g_strdup (owner);
    }
  G_UNLOCK (bus_name_map);
  
  return owner;
}

static void
set_owner_for_name (const char *bus_name, const char *owner)
{
  G_LOCK (bus_name_map);

  if (bus_name_map == NULL)
    bus_name_map = g_hash_table_new_full (g_str_hash, g_str_equal,
					  g_free, g_free);
  g_hash_table_insert (bus_name_map, g_strdup (bus_name), g_strdup (owner));
  G_UNLOCK (bus_name_map);
}

static DBusConnection *
get_connection_for_owner (const char *owner)
{
  DBusConnection *connection;

  connection = NULL;
  G_LOCK (owner_map);
  if (owner_map != NULL)
    connection = g_hash_table_lookup (owner_map, owner);
  if (connection)
    dbus_connection_ref (connection);
  G_UNLOCK (owner_map);
  
  return connection;
}

static void
set_connection_for_owner (DBusConnection *connection, const char *owner)
{
  G_LOCK (owner_map);
  if (owner_map == NULL)
    owner_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)dbus_connection_unref);
      
  g_hash_table_insert (owner_map, g_strdup (owner), connection);
  dbus_connection_ref (connection);
  G_UNLOCK (owner_map);
}

static void
free_local_connections (ThreadLocalConnections *local)
{
  g_hash_table_destroy (local->connections);
  g_free (local);
}

static void
free_mount_connection (gpointer data)
{
  DBusConnection *conn = data;
  
  dbus_connection_close (conn);
  dbus_connection_unref (conn);
}

int
_g_dbus_connection_get_fd_sync (DBusConnection *connection,
				int fd_id)
{
  VfsConnectionData *data;
  int fd;

  data = dbus_connection_get_data (connection, vfs_data_slot);
  g_assert (data != NULL);

  /* I don't think we can get reorders here, can we?
   * Its a sync per-thread connection after all
   */
  g_assert (fd_id == data->extra_fd_count);
  
  fd = _g_socket_receive_fd (data->extra_fd);
  if (fd != -1)
    data->extra_fd_count++;

  return fd;
}

void
_g_dbus_connection_get_fd_async (DBusConnection *connection,
				 int fd_id,
				 GetFdAsyncCallback callback,
				 gpointer callback_data)
{
  VfsConnectionData *data;
  OutstandingFD *outstanding_fd;
  int fd;
  
  data = dbus_connection_get_data (connection, vfs_data_slot);
  g_assert (data != NULL);

  outstanding_fd = g_hash_table_lookup (data->outstanding_fds, GINT_TO_POINTER (fd_id));

  if (outstanding_fd)
    {
      fd = outstanding_fd->fd;
      outstanding_fd->fd = -1;
      g_hash_table_remove (data->outstanding_fds, GINT_TO_POINTER (fd_id));
      callback (fd, callback_data);
    }
  else
    {
      outstanding_fd = g_new0 (OutstandingFD, 1);
      outstanding_fd->fd = -1;
      outstanding_fd->callback = callback;
      outstanding_fd->callback_data = callback_data;
      g_hash_table_insert (data->outstanding_fds,
			   GINT_TO_POINTER (fd_id),
			   outstanding_fd);
    }
}

typedef struct {
  const char *bus_name;
  char *owner;

  DBusMessage *message;
  DBusConnection *connection;
  GCancellable *cancellable;

  GVfsAsyncDBusCallback callback;
  gpointer callback_data;
  
  gpointer op_callback;
  gpointer op_callback_data;

  GError *io_error;
  gulong cancelled_tag;
  
  DBusConnection *private_bus;
} AsyncDBusCall;

static void
async_call_finish (AsyncDBusCall *async_call,
		   DBusMessage *reply)
{
  async_call->callback (reply, async_call->connection,
			async_call->io_error, 
			async_call->cancellable,
			async_call->op_callback,
			async_call->op_callback_data,
			async_call->callback_data);

  if (async_call->private_bus)
    {
      dbus_connection_close (async_call->private_bus);
      dbus_connection_unref (async_call->private_bus);
    }
  if (async_call->connection)
    dbus_connection_unref (async_call->connection);
  g_free (async_call->owner);
  dbus_message_unref (async_call->message);
  if (async_call->cancellable)
    g_object_unref (async_call->cancellable);
  if (async_call->io_error)
    g_error_free (async_call->io_error);
  g_free (async_call);
}

static gboolean
async_call_finish_at_idle (gpointer data)
{
  AsyncDBusCall *async_call = data;

  async_call_finish (async_call, NULL);
  
  return FALSE;
}

static void
async_dbus_response (DBusPendingCall *pending,
		     void            *data)
{
  AsyncDBusCall *async_call = data;
  DBusMessage *reply;
  DBusError derror;

  if (async_call->cancelled_tag)
    g_signal_handler_disconnect (async_call->cancellable,
				 async_call->cancelled_tag);

  reply = dbus_pending_call_steal_reply (pending);
  dbus_pending_call_unref (pending);

  dbus_error_init (&derror);
  if (dbus_set_error_from_message (&derror, reply))
    {
      _g_error_from_dbus (&derror, &async_call->io_error);
      dbus_error_free (&derror);
      async_call_finish (async_call, NULL);
    }
  else
    async_call_finish (async_call, reply);
  
  dbus_message_unref (reply);
}

typedef struct {
  DBusConnection *connection;
  dbus_uint32_t serial;
} AsyncCallCancelData;

static void
async_call_cancel_data_free (gpointer _data)
{
  AsyncCallCancelData *data = _data;

  dbus_connection_unref (data->connection);
  g_free (data);
}

/* Might be called on another thread */
static void
async_call_cancelled_cb (GCancellable *cancellable,
			 gpointer _data)
{
  AsyncCallCancelData *data = _data;
  DBusMessage *cancel_message;

  /* Send cancellation message, this just queues it, sending
   * will happen in mainloop */
  cancel_message = dbus_message_new_method_call (NULL,
						 G_VFS_DBUS_DAEMON_PATH,
						 G_VFS_DBUS_DAEMON_INTERFACE,
						 G_VFS_DBUS_OP_CANCEL);
  if (cancel_message != NULL)
    {
      if (dbus_message_append_args (cancel_message,
				    DBUS_TYPE_UINT32, &data->serial,
				    DBUS_TYPE_INVALID))
	dbus_connection_send (data->connection,
			      cancel_message, NULL);
      dbus_message_unref (cancel_message);
    }
}

static void
async_call_send (AsyncDBusCall *async_call)
{
  DBusPendingCall *pending;
  AsyncCallCancelData *cancel_data;

  /* If we had to create a private session, kill it now instead of later */
  if (async_call->private_bus)
    {
      dbus_connection_close (async_call->private_bus);
      dbus_connection_unref (async_call->private_bus);
      async_call->private_bus = NULL;
    }
    
  if (!dbus_connection_send_with_reply (async_call->connection,
					async_call->message,
					&pending,
					DBUS_TIMEOUT_DEFAULT))
    _g_dbus_oom ();

  if (pending == NULL)
    {
      g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   "Connection is closed");
      async_call_finish (async_call, NULL);
      return;
    }
  
  if (async_call->cancellable)
    {
      cancel_data = g_new0 (AsyncCallCancelData, 1);
      cancel_data->connection = dbus_connection_ref (async_call->connection);
      cancel_data->serial = dbus_message_get_serial (async_call->message);
      async_call->cancelled_tag =
	g_signal_connect_data (async_call->cancellable, "cancelled",
			       (GCallback)async_call_cancelled_cb,
			       cancel_data,
			       (GClosureNotify)async_call_cancel_data_free,
			       0);
    }
      
  if (!dbus_pending_call_set_notify (pending,
				     async_dbus_response,
				     async_call,
				     NULL))
    _g_dbus_oom ();

}

static gboolean
get_private_bus_async (AsyncDBusCall *async_call)
{
  DBusError derror;

  if (async_call->private_bus == NULL)
    {
      /* Unfortunately dbus doesn't have an async get */
      dbus_error_init (&derror);
      async_call->private_bus = dbus_bus_get_private (DBUS_BUS_SESSION, &derror);
      if (async_call->private_bus == NULL)
	{
	  g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       "Couldn't get main dbus connection: %s\n",
		       derror.message);
	  dbus_error_free (&derror);
	  g_idle_add (async_call_finish_at_idle, async_call);
	  return FALSE;
	}
      dbus_connection_set_exit_on_disconnect (async_call->private_bus, FALSE);
     
      /* Connect with mainloop */
      _g_dbus_connection_integrate_with_main (async_call->private_bus);
    }
  
  return TRUE;
}

static void
async_get_connection_response (DBusPendingCall *pending,
			       void            *data)
{
  AsyncDBusCall *async_call = data;
  GError *error;
  DBusError derror;
  DBusMessage *reply;
  char *address1, *address2;
  int extra_fd;
  DBusConnection *connection, *existing_connection;

  reply = dbus_pending_call_steal_reply (pending);
  dbus_pending_call_unref (pending);

  dbus_error_init (&derror);
  if (!dbus_message_get_args (reply, &derror,
			      DBUS_TYPE_STRING, &address1,
			      DBUS_TYPE_STRING, &address2,
			      DBUS_TYPE_INVALID))
    {
      _g_error_from_dbus (&derror, &async_call->io_error);
      dbus_error_free (&derror);
      async_call_finish (async_call, NULL);
      return;
    }

  /* I don't know of any way to do an async connect */
  error = NULL;
  extra_fd = _g_socket_connect (address2, &error);
  if (extra_fd == -1)
    {
      g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Error connecting to daemon: %s"), error->message);
      g_error_free (error);
      dbus_message_unref (reply);
      async_call_finish (async_call, NULL);
      return;
    }

  /* Unfortunately dbus doesn't have an async open */
  dbus_error_init (&derror);
  connection = dbus_connection_open_private (address1, &derror);
  if (!connection)
    {
      close (extra_fd);
      dbus_message_unref (reply);
      
      g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   derror.message);
      dbus_error_free (&derror);
      async_call_finish (async_call, NULL);
      return;
    }
  dbus_message_unref (reply);

  vfs_connection_setup (connection, extra_fd, TRUE);
  
  /* Maybe we already had a connection? This happens if we requested
   * the same owner several times in parallel.
   * If so, just drop this connection and use that.
   */
  
  existing_connection = get_connection_for_owner (async_call->owner);
  if (existing_connection != NULL)
    {
      async_call->connection = existing_connection;
      dbus_connection_close (connection);
      dbus_connection_unref (connection);
    }
  else
    {  
      _g_dbus_connection_integrate_with_main (connection);
      set_connection_for_owner (connection, async_call->owner);
      async_call->connection = connection;
    }

  /* Maybe we were canceled while setting up connection, then
   * avoid doing the operation */
  if (g_cancellable_is_cancelled (async_call->cancellable))
    {
      g_set_error (&async_call->io_error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      async_call_finish (async_call, NULL);
      return;
    }

  async_call_send (async_call);
}

static void
open_connection_async (AsyncDBusCall *async_call)
{
  DBusMessage *get_connection_message;
  DBusPendingCall *pending;

  if (!get_private_bus_async (async_call))
    return;
  
  get_connection_message = dbus_message_new_method_call (async_call->owner,
							 G_VFS_DBUS_DAEMON_PATH,
							 G_VFS_DBUS_DAEMON_INTERFACE,
							 G_VFS_DBUS_OP_GET_CONNECTION);
  
  if (get_connection_message == NULL)
    _g_dbus_oom ();

  if (!dbus_connection_send_with_reply (async_call->private_bus,
					get_connection_message, &pending,
					DBUS_TIMEOUT_DEFAULT))
    _g_dbus_oom ();
  
  dbus_message_unref (get_connection_message);
  
  if (pending == NULL)
    {
      g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   "Connection is closed");
      g_idle_add (async_call_finish_at_idle, async_call);
      return;
    }
  
  if (!dbus_pending_call_set_notify (pending,
				     async_get_connection_response,
				     async_call,
				     NULL))
    _g_dbus_oom ();
}

static void
async_call_got_owner (AsyncDBusCall *async_call)
{
  async_call->connection = get_connection_for_owner (async_call->owner);
  if (async_call->connection == NULL)
    open_connection_async (async_call);
  else
    async_call_send (async_call);
}

static void
async_get_name_owner_response (DBusPendingCall *pending,
			       void            *data)
{
  AsyncDBusCall *async_call = data;
  const char *owner;
  DBusError derror;
  DBusMessage *reply;

  reply = dbus_pending_call_steal_reply (pending);
  dbus_pending_call_unref (pending);

  if (dbus_message_is_error (reply, "org.freedesktop.DBus.Error.NameHasNoOwner"))
    {
      /* TODO: Not mounted */
    }
  
  dbus_error_init (&derror);
  if (dbus_set_error_from_message (&derror, reply))
    {
      _g_error_from_dbus (&derror, &async_call->io_error);
      dbus_error_free (&derror);
      async_call_finish (async_call, NULL);
      return;
    }
  
  if (!dbus_message_get_args (reply, &derror,
			      DBUS_TYPE_STRING, &owner,
			      DBUS_TYPE_INVALID))
    {
      _g_error_from_dbus (&derror, &async_call->io_error);
      dbus_error_free (&derror);
      async_call_finish (async_call, NULL);
      return;
    }

  async_call->owner = g_strdup (owner);
  
  async_call_got_owner (async_call);
}


static void
do_find_owner_async (AsyncDBusCall *async_call)
{
  DBusMessage *message;
  DBusPendingCall *pending;

  if (!get_private_bus_async (async_call))
    return;

  message = dbus_message_new_method_call (DBUS_SERVICE_DBUS,
                                          DBUS_PATH_DBUS,
                                          DBUS_INTERFACE_DBUS,
                                          "GetNameOwner");
  if (message == NULL)
    _g_dbus_oom ();
  
  if (!dbus_message_append_args (message,
				 DBUS_TYPE_STRING, &async_call->bus_name,
				 DBUS_TYPE_INVALID))
    _g_dbus_oom ();
  
  if (!dbus_connection_send_with_reply (async_call->private_bus,
					message, &pending,
					DBUS_TIMEOUT_DEFAULT))
    _g_dbus_oom ();
  
  dbus_message_unref (message);
  
  if (pending == NULL)
    {
      g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   "Connection is closed");
      g_idle_add (async_call_finish_at_idle, async_call);
      return;
    }
  
  if (!dbus_pending_call_set_notify (pending,
				     async_get_name_owner_response,
				     async_call,
				     NULL))
    _g_dbus_oom ();
}

void
_g_vfs_daemon_call_async (DBusMessage *message,
			  gpointer op_callback,
			  gpointer op_callback_data,
			  GVfsAsyncDBusCallback callback,
			  gpointer callback_data,
			  GCancellable *cancellable)
{
  AsyncDBusCall *async_call;

  g_once (&once_init_dbus, vfs_dbus_init, NULL);

  async_call = g_new0 (AsyncDBusCall, 1);
  async_call->bus_name = dbus_message_get_destination (message);
  async_call->message = dbus_message_ref (message);
  if (cancellable)
    async_call->cancellable = g_object_ref (cancellable);
  async_call->callback = callback;
  async_call->callback_data = callback_data;
  async_call->op_callback = op_callback;
  async_call->op_callback_data = op_callback_data;

  async_call->owner = get_owner_for_bus_name (async_call->bus_name);
  if (async_call->owner == NULL)
    do_find_owner_async (async_call);
  else
    async_call_got_owner (async_call);
}

DBusMessage *
_g_vfs_daemon_call_sync (DBusMessage *message,
			 DBusConnection **connection_out,
			 GCancellable *cancellable,
			 GError **error)
{
  DBusConnection *connection;
  DBusError derror;
  DBusMessage *reply;
  DBusPendingCall *pending;
  int dbus_fd;
  int cancel_fd;
  gboolean sent_cancel;
  DBusMessage *cancel_message;
  dbus_uint32_t serial;
  const char *bus_name = dbus_message_get_destination (message);

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }
	    
  connection = get_connection_sync (bus_name, error);
  if (connection == NULL)
    return NULL;

  /* TODO: Handle errors below due to unmount and invalidate the
     sync connection cache */
  
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }

  cancel_fd = g_cancellable_get_fd (cancellable);
  if (cancel_fd != -1)
    {
      if (!dbus_connection_send_with_reply (connection, message,
					    &pending, DBUS_TIMEOUT_DEFAULT))
	_g_dbus_oom ();
      
      if (pending == NULL)
	{
	  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       "Error while getting peer-to-peer dbus connection: %s",
		       "Connection is closed");
	  return NULL;
	}

      /* Make sure the message is sent */
      dbus_connection_flush (connection);

      if (!dbus_connection_get_socket (connection, &dbus_fd))
	{
	  dbus_pending_call_unref (pending);
	  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       "Error while getting peer-to-peer dbus connection: %s",
		       "No fd");
	  return NULL;
	}

      sent_cancel = FALSE;
      while (!dbus_pending_call_get_completed (pending))
	{
	  struct pollfd poll_fds[2];
	  int poll_ret;
	  
	  do
	    {
	      poll_fds[0].events = POLLIN;
	      poll_fds[0].fd = dbus_fd;
	      poll_fds[1].events = POLLIN;
	      poll_fds[1].fd = cancel_fd;
	      poll_ret = poll (poll_fds, sent_cancel?1:2, -1);
	    }
	  while (poll_ret == -1 && errno == EINTR);

	  if (poll_ret == -1)
	    {
	      dbus_pending_call_unref (pending);
	      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
			   "Error while getting peer-to-peer dbus connection: %s",
			   "poll error");
	      return NULL;
	    }
	  
	  if (!sent_cancel && g_cancellable_is_cancelled (cancellable))
	    {
	      sent_cancel = TRUE;
	      serial = dbus_message_get_serial (message);
	      cancel_message =
		dbus_message_new_method_call (NULL,
					      G_VFS_DBUS_DAEMON_PATH,
					      G_VFS_DBUS_DAEMON_INTERFACE,
					      G_VFS_DBUS_OP_CANCEL);
	      if (cancel_message != NULL)
		{
		  if (dbus_message_append_args (cancel_message,
						DBUS_TYPE_UINT32, &serial,
						DBUS_TYPE_INVALID))
		    {
		      dbus_connection_send (connection, cancel_message, NULL);
		      dbus_connection_flush (connection);
		    }
			    
		  dbus_message_unref (cancel_message);
		}
	    }

	  if (poll_fds[0].revents != 0)
	    {
	      dbus_connection_read_write (connection, DBUS_TIMEOUT_DEFAULT);

	      while (dbus_connection_dispatch (connection) == DBUS_DISPATCH_DATA_REMAINS)
		;
		
	    }
	}

      reply = dbus_pending_call_steal_reply (pending);
      dbus_pending_call_unref (pending);
    }
  else
    {
      dbus_error_init (&derror);
      reply = dbus_connection_send_with_reply_and_block (connection, message,
							 DBUS_TIMEOUT_DEFAULT,
							 &derror);
      if (!reply)
	{
	  _g_error_from_dbus (&derror, error);
	  dbus_error_free (&derror);
	  return NULL;
	}
    }

  if (connection_out)
    *connection_out = connection;

  if (dbus_set_error_from_message (&derror, reply))
    {
      _g_error_from_dbus (&derror, error);
      dbus_error_free (&derror);
      dbus_message_unref (reply);
      return NULL;
    }
  
  return reply;
}

static char *
get_name_owner_sync (const char *bus_name, GError **error)
{
  DBusMessage *message, *reply;
  DBusConnection *connection;
  char *owner;
  DBusError derror;

  dbus_error_init (&derror);
  connection = dbus_bus_get (DBUS_BUS_SESSION, &derror);
  if (connection == NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Couldn't get main dbus connection: %s\n",
		   derror.message);
      dbus_error_free (&derror);
      return NULL;
    }

  owner = NULL;

  message = dbus_message_new_method_call (DBUS_SERVICE_DBUS,
                                          DBUS_PATH_DBUS,
                                          DBUS_INTERFACE_DBUS,
                                          "GetNameOwner");
  if (message == NULL)
    _g_dbus_oom ();
  
  if (!dbus_message_append_args (message,
				 DBUS_TYPE_STRING, &bus_name,
				 DBUS_TYPE_INVALID))
    _g_dbus_oom ();
  
  reply = dbus_connection_send_with_reply_and_block (connection, message, -1, &derror);
  dbus_message_unref (message);

  if (reply == NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Couldn't get dbus name owner: %s\n",
		   derror.message);
      goto out;
    }

  if (dbus_message_is_error (reply, "org.freedesktop.DBus.Error.NameHasNoOwner"))
    {
      /* TODO: Not mounted */
    }

  if (dbus_set_error_from_message (&derror, reply))
    {
      _g_error_from_dbus (&derror, error);
      dbus_error_free (&derror);
      goto out;
    }
  
  if (!dbus_message_get_args (reply, &derror,
                              DBUS_TYPE_STRING, &owner,
                              DBUS_TYPE_INVALID))
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Couldn't get dbus name owner: %s\n",
		   derror.message);
      dbus_error_free (&derror);
      goto out;
    }
  
  owner = g_strdup (owner);

 out:
  dbus_connection_unref (connection);
  dbus_message_unref (reply);
  return owner;
  
}


static DBusConnection *
get_connection_sync (const char *bus_name,
		     GError **error)
{
  DBusConnection *bus;
  ThreadLocalConnections *local;
  GError *local_error;
  DBusConnection *connection;
  DBusMessage *message, *reply;
  DBusError derror;
  char *address1, *address2;
  char *owner;
  int extra_fd;

  g_once (&once_init_dbus, vfs_dbus_init, NULL);

  local = g_static_private_get (&local_connections);
  if (local == NULL)
    {
      local = g_new0 (ThreadLocalConnections, 1);
      local->connections = g_hash_table_new_full (g_str_hash, g_str_equal,
						  g_free, free_mount_connection);
      g_static_private_set (&local_connections, local, (GDestroyNotify)free_local_connections);
    }

  owner = get_owner_for_bus_name (bus_name);
  if (owner == NULL)
    {
      owner = get_name_owner_sync (bus_name, error);
      if (owner == NULL)
	return NULL;

      set_owner_for_name (bus_name, owner);
    }
  
  connection = g_hash_table_lookup (local->connections, owner);
  if (connection != NULL)
    {
      g_free (owner);
      return connection;
    }

  dbus_error_init (&derror);
  bus = dbus_bus_get (DBUS_BUS_SESSION, &derror);
  if (bus == NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Couldn't get main dbus connection: %s\n",
		   derror.message);
      dbus_error_free (&derror);
      g_free (owner);
      return NULL;
    }
  
  message = dbus_message_new_method_call (owner,
					  G_VFS_DBUS_DAEMON_PATH,
					  G_VFS_DBUS_DAEMON_INTERFACE,
					  G_VFS_DBUS_OP_GET_CONNECTION);
  
  reply = dbus_connection_send_with_reply_and_block (bus, message, -1,
						     &derror);
  dbus_message_unref (message);
  dbus_connection_unref (bus);

  if (!reply)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   derror.message);
      dbus_error_free (&derror);
      g_free (owner);
      return NULL;
    }

  if (dbus_set_error_from_message (&derror, reply))
    {
      _g_error_from_dbus (&derror, error);
      dbus_error_free (&derror);
      g_free (owner);
      return NULL;
    }
  
  dbus_message_get_args (reply, NULL,
			 DBUS_TYPE_STRING, &address1,
			 DBUS_TYPE_STRING, &address2,
			 DBUS_TYPE_INVALID);

  local_error = NULL;
  extra_fd = _g_socket_connect (address2, &local_error);
  if (extra_fd == -1)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Error connecting to daemon: %s"), local_error->message);
      g_error_free (local_error);
      dbus_message_unref (reply);
      g_free (owner);
      return NULL;
    }

  dbus_error_init (&derror);
  connection = dbus_connection_open_private (address1, &derror);
  if (!connection)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   derror.message);
      close (extra_fd);
      dbus_message_unref (reply);
      dbus_error_free (&derror);
      g_free (owner);
      return NULL;
    }
  dbus_message_unref (reply);

  vfs_connection_setup (connection, extra_fd, FALSE);

  g_hash_table_insert (local->connections, owner, connection);

  return connection;
}

GFileInfo *
_g_dbus_get_file_info (DBusMessageIter *iter,
		       GFileInfoRequestFlags requested,
		       GError **error)
{
  GFileInfo *info;
  DBusMessageIter struct_iter, array_iter;

  info = g_file_info_new ();

  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_STRUCT)
    goto error;

  dbus_message_iter_recurse (iter, &struct_iter);

  if (requested & G_FILE_INFO_FILE_TYPE)
    {
      guint16 type;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_UINT16)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &type);

      g_file_info_set_file_type (info, type);

      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_NAME)
    {
      char *str;
      const char *data;
      int len;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_ARRAY ||
	  dbus_message_iter_get_element_type (&struct_iter) != DBUS_TYPE_BYTE)
	goto error;

      dbus_message_iter_recurse (&struct_iter, &array_iter);
      dbus_message_iter_get_fixed_array (&array_iter, &data, &len);
      str = g_strndup (data, len);
      g_file_info_set_name (info, str);
      g_free (str);
      
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_DISPLAY_NAME)
    {
      const char *str;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_STRING)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &str);
      
      g_file_info_set_display_name (info, str);
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_EDIT_NAME)
    {
      const char *str;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_STRING)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &str);
      
      g_file_info_set_edit_name (info, str);
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_ICON)
    {
      const char *str;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_STRING)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &str);
      
      g_file_info_set_icon (info, str);
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_MIME_TYPE)
    {
      const char *str;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_STRING)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &str);
      
      g_file_info_set_mime_type (info, str);
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_SIZE)
    {
      guint64 size;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_UINT64)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &size);
      
      g_file_info_set_size (info, size);
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_MODIFICATION_TIME)
    {
      guint64 time;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_UINT64)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &time);
      
      g_file_info_set_modification_time (info, time);
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_ACCESS_RIGHTS)
    {
      guint32 rights;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_UINT32)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &rights);
      
      g_file_info_set_access_rights (info, rights);
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_STAT_INFO)
    {
      guint32 tmp;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_UINT32)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &tmp);
      
      /* TODO: implement statinfo */
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_SYMLINK_TARGET)
    {
      char *str;
      const char *data;
      int len;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_ARRAY ||
	  dbus_message_iter_get_element_type (&struct_iter) != DBUS_TYPE_BYTE)
	goto error;

      dbus_message_iter_recurse (&struct_iter, &array_iter);
      dbus_message_iter_get_fixed_array (&array_iter, &data, &len);
      str = g_strndup (data, len);
      g_file_info_set_symlink_target (info, str);
      g_free (str);
      
      dbus_message_iter_next (&struct_iter);
    }

  if (requested & G_FILE_INFO_IS_HIDDEN)
    {
      dbus_bool_t is_hidden;
      
      if (dbus_message_iter_get_arg_type (&struct_iter) != DBUS_TYPE_BOOLEAN)
	goto error;

      dbus_message_iter_get_basic (&struct_iter, &is_hidden);
      
      g_file_info_set_is_hidden (info, is_hidden);
      dbus_message_iter_next (&struct_iter);
    }

  /* TODO: Attributes */

  dbus_message_iter_next (iter);
  return info;

 error:
  g_object_unref (info);
  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
	       _("Invalid file info format"));
  return NULL;
}
