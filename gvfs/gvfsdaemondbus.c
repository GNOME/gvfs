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

static GHashTable *bus_name_map = NULL;

G_LOCK_DEFINE_STATIC(bus_name_map);

static DBusConnection *get_connection_for_main_context (GMainContext    *context,
							const char      *owner);
static DBusSource     *set_connection_for_main_context (GMainContext    *context,
							const char      *owner,
							DBusConnection  *connection);
static void            dbus_source_destroy             (DBusSource      *dbus_source);
static DBusConnection *get_connection_sync             (const char      *bus_name,
							GError         **error);


static gpointer
vfs_dbus_init (gpointer arg)
{
  if (!dbus_connection_allocate_data_slot (&vfs_data_slot))
    g_error ("Unable to allocate data slot");

  return NULL;
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

static void
append_unescaped_dbus_name (GString *s,
			    const char *escaped,
			    const char *end)
{
  guchar c;

  while (escaped < end)
    {
      c = *escaped++;
      if (c == '_' &&
	  escaped < end)
	{
	  c = g_ascii_xdigit_value (*escaped++) << 4;

	  if (escaped < end)
	    c |= g_ascii_xdigit_value (*escaped++);
	}
      g_string_append_c (s, c);
    }
}

char *
_g_dbus_unescape_bus_name (const char *escaped, const char *end)
{
  GString *s = g_string_new ("");
  
  if (end == NULL)
    end = escaped + strlen (escaped);

  append_unescaped_dbus_name (s, escaped, end);
  return g_string_free (s, FALSE);
}

/* We use _ for escaping */
#define VALID_INITIAL_BUS_NAME_CHARACTER(c)         \
  ( ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') ||               \
   /*((c) == '_') || */((c) == '-'))
#define VALID_BUS_NAME_CHARACTER(c)                 \
  ( ((c) >= '0' && (c) <= '9') ||               \
    ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') ||               \
   /*((c) == '_')||*/  ((c) == '-'))

void
_g_dbus_append_escaped_bus_name (GString *s,
				 gboolean at_start,
				 const char *unescaped)
{
  char c;
  gboolean first;
  static const gchar hex[16] = "0123456789ABCDEF";

  while ((c = *unescaped++) != 0)
    {
      if (first && at_start)
	{
	  if (VALID_INITIAL_BUS_NAME_CHARACTER (c))
	    {
	      g_string_append_c (s, c);
	      continue;
	    }
	}
      else
	{
	  if (VALID_BUS_NAME_CHARACTER (c))
	    {
	      g_string_append_c (s, c);
	      continue;
	    }
	}

      first = FALSE;
      g_string_append_c (s, '_');
      g_string_append_c (s, hex[((guchar)c) >> 4]);
      g_string_append_c (s, hex[((guchar)c) & 0xf]);
    }
}


static int
daemon_socket_connect (const char *address, GError **error)
{
  int fd;
  const char *path;
  size_t path_len;
  struct sockaddr_un addr;
  gboolean abstract;

  fd = socket (PF_UNIX, SOCK_STREAM, 0);
  if (fd == -1)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Error connecting to daemon: %s"), g_strerror (errno));
      return -1;
    }

  if (g_str_has_prefix (address, "unix:abstract="))
    {
      path = address + strlen ("unix:abstract=");
      abstract = TRUE;
    }
  else
    {
      path = address + strlen ("unix:path=");
      abstract = FALSE;
    }
    
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  path_len = strlen (path);

  if (abstract)
    {
      addr.sun_path[0] = '\0'; /* this is what says "use abstract" */
      path_len++; /* Account for the extra nul byte added to the start of sun_path */

      strncpy (&addr.sun_path[1], path, path_len);
    }
  else
    {
      strncpy (addr.sun_path, path, path_len);
    }
  
  if (connect (fd, (struct sockaddr*) &addr, G_STRUCT_OFFSET (struct sockaddr_un, sun_path) + path_len) < 0)
    {      
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Error connecting to daemon: %s"), g_strerror (errno));
      close (fd);
      return -1;
    }

  return fd;
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


/* receive a file descriptor over file descriptor fd */
static int 
receive_fd (int connection_fd)
{
  struct msghdr msg;
  struct iovec iov;
  char buf[1];
  int rv;
  char ccmsg[CMSG_SPACE (sizeof(int))];
  struct cmsghdr *cmsg;

  iov.iov_base = buf;
  iov.iov_len = 1;
  msg.msg_name = 0;
  msg.msg_namelen = 0;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ccmsg;
  msg.msg_controllen = sizeof (ccmsg);
  
  rv = recvmsg (connection_fd, &msg, 0);
  if (rv == -1) 
    {
      perror ("recvmsg");
      return -1;
    }

  cmsg = CMSG_FIRSTHDR (&msg);
  if (!cmsg->cmsg_type == SCM_RIGHTS) {
    g_warning("got control message of unknown type %d", 
	      cmsg->cmsg_type);
    return -1;
  }

  return *(int*)CMSG_DATA(cmsg);
}

int
_g_dbus_connection_get_fd_sync (DBusConnection *connection,
				int fd_id)
{
  VfsConnectionData *data;
  int fd;

  data = dbus_connection_get_data (connection , vfs_data_slot);
  g_assert (data != NULL);

  /* I don't think we can get reorders here, can we?
   * Its a sync per-thread connection after all
   */
  g_assert (fd_id == data->extra_fd_count);
  
  fd = receive_fd (data->extra_fd);
  if (fd != -1)
    data->extra_fd_count++;

  return fd;
}

static void
outstanding_fd_free (OutstandingFD *outstanding)
{
  if (outstanding->fd != -1)
    close (outstanding->fd);

  g_free (outstanding);
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
  GMainContext *context;
  GCancellable *cancellable;

  GVfsAsyncDBusCallback callback;
  gpointer callback_data;
  
  gpointer op_callback;
  gpointer op_callback_data;

  GError *io_error;
  gulong cancelled_tag;
  
  DBusConnection *private_bus;
  DBusSource *dbus_source;
} AsyncDBusCall;

static void
async_dbus_call_finish (AsyncDBusCall *async_call,
			DBusMessage *reply)
{
  async_call->callback (reply, async_call->connection,
			async_call->io_error, 
			async_call->cancellable,
			async_call->op_callback,
			async_call->op_callback_data,
			async_call->callback_data);

  if (async_call->dbus_source != NULL)
    {
      dbus_source_destroy (async_call->dbus_source);
      g_source_unref ((GSource *)async_call->dbus_source);
    }
 
  g_free (async_call->owner);
  dbus_message_unref (async_call->message);
  if (async_call->context)
    g_main_context_unref (async_call->context);
  if (async_call->cancellable)
    g_object_unref (async_call->cancellable);
  if (async_call->io_error)
    g_error_free (async_call->io_error);
  g_free (async_call);
}

static gboolean
async_dbus_call_finish_at_idle (gpointer data)
{
  AsyncDBusCall *async_call = data;

  async_dbus_call_finish (async_call, NULL);
  
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
      async_dbus_call_finish (async_call, NULL);
    }
  else
    async_dbus_call_finish (async_call, reply);
  
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
do_call_async (AsyncDBusCall *async_call)
{
  DBusPendingCall *pending;
  AsyncCallCancelData *cancel_data;

  /* If we had to create a private session, kill it now instead of later */
  if (async_call->dbus_source != NULL)
    {
      dbus_source_destroy (async_call->dbus_source);
      g_source_unref ((GSource *)async_call->dbus_source);
      async_call->dbus_source = NULL;
    }
  
  if (!dbus_connection_send_with_reply (async_call->connection,
					async_call->message,
					&pending,
					DBUS_TIMEOUT_DEFAULT))
    g_error ("Failed to send message (oom)");

  if (pending == NULL)
    {
      g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   "Connection is closed");
      async_dbus_call_finish (async_call, NULL);
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
    g_error ("Failed to send message (oom)");

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
      dbus_connection_set_exit_on_disconnect (async_call->private_bus, FALSE);
      if (async_call->private_bus == NULL)
	{
	  g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       "Couldn't get main dbus connection: %s\n",
		       derror.message);
	  dbus_error_free (&derror);
	  g_idle_add (async_dbus_call_finish_at_idle, async_call);
	  return FALSE;
	}
      
      /* Connect with mainloop */
      async_call->dbus_source =
	set_connection_for_main_context (async_call->context, NULL,
					 async_call->private_bus);

      /* The connection is owned by the main context */
      dbus_connection_unref (async_call->private_bus);
    }
  
  return TRUE;
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
  new_fd = receive_fd (data->extra_fd);
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
  VfsConnectionData *connection_data;
  DBusSource *source;

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
      async_dbus_call_finish (async_call, NULL);
      return;
    }

  /* I don't know of any way to do an async connect */
  error = NULL;
  extra_fd = daemon_socket_connect (address2, &async_call->io_error);
  if (extra_fd == -1)
    {
      dbus_message_unref (reply);
      async_dbus_call_finish (async_call, NULL);
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
      async_dbus_call_finish (async_call, NULL);
      return;
    }
  dbus_message_unref (reply);

  connection_data = g_new0 (VfsConnectionData, 1);
  connection_data->extra_fd = extra_fd;
  connection_data->extra_fd_count = 0;
  connection_data->outstanding_fds =
    g_hash_table_new_full (g_direct_hash,
			   g_direct_equal,
			   NULL,
			   (GDestroyNotify)outstanding_fd_free);

  connection_data->extra_fd_source =
    _g_fd_source_new (extra_fd, POLLIN, async_call->context, NULL);
  g_source_set_callback (connection_data->extra_fd_source,
			 (GSourceFunc)accept_new_fd, connection_data, NULL);
  
  if (!dbus_connection_set_data (connection, vfs_data_slot, connection_data, connection_data_free))
    g_error ("Out of memory");

  /* Maybe we already had a connection? This happens if we requested
   * the same owner several times in parallel.
   * If so, just drop this connection and use that.
   */
  
  existing_connection = get_connection_for_main_context (async_call->context,
							 async_call->owner);
  if (existing_connection != NULL)
    {
      async_call->connection = existing_connection;
      dbus_connection_close (connection);
    }
  else
    {  
      async_call->connection = connection;
      source = set_connection_for_main_context (async_call->context,
						async_call->owner,
						connection);
      g_source_unref ((GSource *)source); /* Owned by context */
    }
  
  dbus_connection_unref (connection);

  /* Maybe we were canceled while setting up connection, then
   * avoid doing the operation */
  if (g_cancellable_is_cancelled (async_call->cancellable))
    {
      g_set_error (&async_call->io_error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      async_dbus_call_finish (async_call, NULL);
      return;
    }

  do_call_async (async_call);
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
    g_error ("Failed to allocate message");

  if (!dbus_connection_send_with_reply (async_call->private_bus,
					get_connection_message, &pending,
					DBUS_TIMEOUT_DEFAULT))
    g_error ("Failed to send message (oom)");
  
  dbus_message_unref (get_connection_message);
  
  
  if (pending == NULL)
    {
      g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   "Connection is closed");
      g_idle_add (async_dbus_call_finish_at_idle, async_call);
      return;
    }
  
  if (!dbus_pending_call_set_notify (pending,
				     async_get_connection_response,
				     async_call,
				     NULL))
    g_error ("Failed to send message (oom)");
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
      async_dbus_call_finish (async_call, NULL);
      return;
    }
  
  if (!dbus_message_get_args (reply, &derror,
			      DBUS_TYPE_STRING, &owner,
			      DBUS_TYPE_INVALID))
    {
      _g_error_from_dbus (&derror, &async_call->io_error);
      dbus_error_free (&derror);
      async_dbus_call_finish (async_call, NULL);
      return;
    }


  async_call->owner = g_strdup (owner);

  async_call->connection = get_connection_for_main_context (async_call->context, async_call->owner);
  if (async_call->connection == NULL)
    {
      open_connection_async (async_call);
      return;
    }
  
  do_call_async (async_call);
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
    g_error ("oom");
  
  if (!dbus_message_append_args (message,
				 DBUS_TYPE_STRING, &async_call->bus_name,
				 DBUS_TYPE_INVALID))
    g_error ("oom");
  
  if (!dbus_connection_send_with_reply (async_call->private_bus,
					message, &pending,
					DBUS_TIMEOUT_DEFAULT))
    g_error ("Failed to send message (oom)");
  
  dbus_message_unref (message);
  
  if (pending == NULL)
    {
      g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   "Connection is closed");
      g_idle_add (async_dbus_call_finish_at_idle, async_call);
      return;
    }
  
  if (!dbus_pending_call_set_notify (pending,
				     async_get_name_owner_response,
				     async_call,
				     NULL))
    g_error ("Failed to send message (oom)");
}

void
_g_vfs_daemon_call_async (DBusMessage *message,
			  GMainContext *context,
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
  if (context)
    async_call->context = g_main_context_ref (context);
  if (cancellable)
    async_call->cancellable = g_object_ref (cancellable);
  async_call->callback = callback;
  async_call->callback_data = callback_data;
  async_call->op_callback = op_callback;
  async_call->op_callback_data = op_callback_data;

  async_call->owner = get_owner_for_bus_name (async_call->bus_name);
  if (async_call->owner == NULL)
    {
      do_find_owner_async (async_call);
      return;
    }
    
  async_call->connection = get_connection_for_main_context (context, async_call->owner);
  if (async_call->connection == NULL)
    {
      open_connection_async (async_call);
      return;
    }
  
  do_call_async (async_call);
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
	g_error ("Failed to send message (oom)");
      
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
    g_error ("oom");
  
  if (!dbus_message_append_args (message,
				 DBUS_TYPE_STRING, &bus_name,
				 DBUS_TYPE_INVALID))
    g_error ("oom");
  
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
  DBusConnection *connection;
  DBusMessage *message, *reply;
  DBusError derror;
  char *address1, *address2;
  char *owner;
  int extra_fd;
  VfsConnectionData *connection_data;

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
      return NULL;
    }
  
  message = dbus_message_new_method_call (owner,
					  G_VFS_DBUS_DAEMON_PATH,
					  G_VFS_DBUS_DAEMON_INTERFACE,
					  G_VFS_DBUS_OP_GET_CONNECTION);
  g_free (owner);
  
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
      return NULL;
    }

  if (dbus_set_error_from_message (&derror, reply))
    {
      _g_error_from_dbus (&derror, error);
      dbus_error_free (&derror);
      return NULL;
    }
  
  dbus_message_get_args (reply, NULL,
			 DBUS_TYPE_STRING, &address1,
			 DBUS_TYPE_STRING, &address2,
			 DBUS_TYPE_INVALID);

  extra_fd = daemon_socket_connect (address2, error);
  if (extra_fd == -1)
    {
      dbus_message_unref (reply);
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
      return NULL;
    }
  dbus_message_unref (reply);

  connection_data = g_new (VfsConnectionData, 1);
  connection_data->extra_fd = extra_fd;
  connection_data->extra_fd_count = 0;

  if (!dbus_connection_set_data (connection, vfs_data_slot, connection_data, connection_data_free))
    g_error ("Out of memory");

  g_hash_table_insert (local->connections, g_strdup (owner), connection);

  return connection;
}

gboolean
_g_dbus_message_iter_append_filename (DBusMessageIter *iter, const char *filename)
{
  DBusMessageIter array;

  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_ARRAY,
					 DBUS_TYPE_BYTE_AS_STRING,
					 &array))
    return FALSE;
  
  if (!dbus_message_iter_append_fixed_array (&array,
					     DBUS_TYPE_BYTE,
					     &filename, strlen (filename)))
    return FALSE;
  
  if (!dbus_message_iter_close_container (iter, &array))
    return FALSE;

  return TRUE;
}

void
_g_error_from_dbus (DBusError *derror, 
		    GError **error)
{
  const char *name, *end;;
  char *m;
  GString *str;
  GQuark domain;
  int code;

  if (g_str_has_prefix (derror->name, "org.glib.GError."))
    {
      domain = 0;
      code = 0;

      name = derror->name + strlen ("org.glib.GError.");
      end = strchr (name, '.');
      if (end)
	{
	  str = g_string_new (NULL);
	  append_unescaped_dbus_name (str, name, end);
	  domain = g_quark_from_string (str->str);
	  g_string_free (str, TRUE);

	  end++; /* skip . */
	  if (*end++ == 'c')
	    code = atoi (end);
	}
      
      g_set_error (error, domain, code, "%s", derror->message);
    }
  /* TODO: Special case other types, like DBUS_ERROR_NO_MEMORY etc? */
  else
    {
      m = g_strdup_printf ("DBus error %s: %s", derror->name, derror->message);
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO, "%s", m);
      g_free (m);
    }
}


GList *
_g_dbus_bus_list_names_with_prefix_sync (DBusConnection *connection,
					 const char *prefix,
					 DBusError *error)
{
  DBusMessage *message, *reply;
  DBusMessageIter iter, array;
  GList *names;

  g_return_val_if_fail (connection != NULL, NULL);
  
  message = dbus_message_new_method_call (DBUS_SERVICE_DBUS,
                                          DBUS_PATH_DBUS,
                                          DBUS_INTERFACE_DBUS,
                                          "ListNames");
  if (message == NULL)
    return NULL;
  
  reply = dbus_connection_send_with_reply_and_block (connection, message, -1, error);
  dbus_message_unref (message);
  
  if (reply == NULL)
    return NULL;
  
  names = NULL;
  
  if (!dbus_message_iter_init (reply, &iter) ||
      (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_ARRAY) ||
      (dbus_message_iter_get_element_type (&iter) != DBUS_TYPE_STRING))
    goto out;

  for (dbus_message_iter_recurse (&iter, &array);  
       dbus_message_iter_get_arg_type (&array) == DBUS_TYPE_STRING;
       dbus_message_iter_next (&array))
    {
      char *name;
      dbus_message_iter_get_basic (&array, &name);
      if (g_str_has_prefix (name, prefix))
	names = g_list_prepend (names, g_strdup (name));
    }

  names = g_list_reverse (names);
  
 out:
  dbus_message_unref (reply);
  return names;
}

/*************************************************************************
 *                                                                       *
 *      dbus mainloop integration for async ops                          *
 *                                                                       *
 *************************************************************************/

/**
 * A GSource subclass for dispatching DBusConnection messages.
 * We need this on top of the IO handlers, because sometimes
 * there are messages to dispatch queued up but no IO pending.
 * 
 * The source is owned by the main context and keeps the connection
 * alive
 */
struct DBusSource
{
  GSource source;             /**< the parent GSource */
  gboolean in_finalize;
  char *owner;
  DBusConnection *connection; /**< the dbus connection, owned */
  GMainContext *context;      /**< the main context, not owed */
  GSList *ios;                /**< all IOHandler */
  GSList *timeouts;           /**< all TimeoutHandler */
};

typedef struct
{
  DBusSource *dbus_source;
  GSource *source; /* owned by context */
  DBusWatch *watch;
} IOHandler;

typedef struct
{
  DBusSource *dbus_source;
  GSource *source; /* owned by context */
  DBusTimeout *timeout;
} TimeoutHandler;

static GHashTable *context_map = NULL;
G_LOCK_DEFINE_STATIC(context_map);

static guint
dbus_source_hash (gconstpointer _key)
{
  const DBusSource *key = _key;
  return g_str_hash (key->owner) ^ (guint)key->context;
}
  
static gboolean
dbus_source_equal (gconstpointer a,
		   gconstpointer b)
{
  const DBusSource *aa = a;
  const DBusSource *bb = b;
  
  return
    strcmp (aa->owner, bb->owner) == 0 &&
    aa->context == bb->context;
}

static gboolean
dbus_source_prepare (GSource *source,
		     gint    *timeout)
{
  DBusConnection *connection = ((DBusSource *)source)->connection;
  
  *timeout = -1;

  return (dbus_connection_get_dispatch_status (connection) == DBUS_DISPATCH_DATA_REMAINS);  
}

static gboolean
dbus_source_check (GSource *source)
{
  return FALSE;
}

static gboolean
dbus_source_dispatch (GSource     *source,
		      GSourceFunc  callback,
		      gpointer     user_data)
{
  DBusConnection *connection = ((DBusSource *)source)->connection;

  dbus_connection_ref (connection);

  /* Only dispatch once - we don't want to starve other GSource */
  dbus_connection_dispatch (connection);
  
  dbus_connection_unref (connection);

  return TRUE;
}

static void
io_handler_source_finalized (gpointer data)
{
  IOHandler *handler = data;
  DBusSource *dbus_source;

  dbus_source = handler->dbus_source;

  /* Source was finalized */
  handler->source = NULL;
  
  if (dbus_source)
    dbus_source->ios = g_slist_remove (dbus_source->ios, handler);
  
  if (handler->watch)
    dbus_watch_set_data (handler->watch, NULL, NULL);
  
  g_free (handler);
}

static void
io_handler_destroy_source (void *data)
{
  IOHandler *handler = data;

  if (handler->source)
    g_source_destroy (handler->source);
}

static void
io_handler_watch_freed (void *data)
{
  IOHandler *handler = data;

  handler->watch = NULL;

  io_handler_destroy_source (handler);
}

static gboolean
io_handler_dispatch (gpointer data,
                     GIOCondition condition,
                     int fd)
{
  IOHandler *handler = data;
  guint dbus_condition = 0;
  DBusConnection *connection;

  connection = handler->dbus_source->connection;
  
  if (connection)
    dbus_connection_ref (connection);
  
  if (condition & G_IO_IN)
    dbus_condition |= DBUS_WATCH_READABLE;
  if (condition & G_IO_OUT)
    dbus_condition |= DBUS_WATCH_WRITABLE;
  if (condition & G_IO_ERR)
    dbus_condition |= DBUS_WATCH_ERROR;
  if (condition & G_IO_HUP)
    dbus_condition |= DBUS_WATCH_HANGUP;

  /* Note that we don't touch the handler after this, because
   * dbus may have disabled the watch and thus killed the
   * handler.
   */
  dbus_watch_handle (handler->watch, dbus_condition);
  handler = NULL;

  if (connection)
    dbus_connection_unref (connection);
  
  return TRUE;
}

static void
dbus_source_add_watch (DBusSource *dbus_source,
		       DBusWatch *watch)
{
  guint flags;
  GIOCondition condition;
  IOHandler *handler;

  if (!dbus_watch_get_enabled (watch))
    return;
  
  g_assert (dbus_watch_get_data (watch) == NULL);
  
  flags = dbus_watch_get_flags (watch);

  condition = G_IO_ERR | G_IO_HUP;
  if (flags & DBUS_WATCH_READABLE)
    condition |= G_IO_IN;
  if (flags & DBUS_WATCH_WRITABLE)
    condition |= G_IO_OUT;

  handler = g_new0 (IOHandler, 1);
  handler->dbus_source = dbus_source;
  handler->watch = watch;

  handler->source = _g_fd_source_new (dbus_watch_get_fd (watch),
				      condition, dbus_source->context, NULL);
  g_source_set_callback (handler->source, (GSourceFunc) io_handler_dispatch, handler,
                         io_handler_source_finalized);
  g_source_unref (handler->source);
  
  /* handler->source is owned by the context here */
  dbus_source->ios = g_slist_prepend (dbus_source->ios, handler);
  
  dbus_watch_set_data (watch, handler, io_handler_watch_freed);
}

static void
dbus_source_remove_watch (DBusSource *dbus_source,
			  DBusWatch *watch)
{
  IOHandler *handler;

  handler = dbus_watch_get_data (watch);

  if (handler == NULL)
    return;
  
  io_handler_destroy_source (handler);
}

static void
timeout_handler_source_finalized (gpointer data)
{
  TimeoutHandler *handler = data;
  DBusSource *dbus_source;

  dbus_source = handler->dbus_source;

  /* Source was finalized */
  handler->source = NULL;
  
  if (dbus_source)
    dbus_source->timeouts = g_slist_remove (dbus_source->timeouts, handler);
  
  if (handler->timeout)
    dbus_timeout_set_data (handler->timeout, NULL, NULL);
  
  g_free (handler);
}

static void
timeout_handler_destroy_source (void *data)
{
  TimeoutHandler *handler = data;

  if (handler->source)
    g_source_destroy (handler->source);
}

static void
timeout_handler_timeout_freed (void *data)
{
  TimeoutHandler *handler = data;

  handler->timeout = NULL;

  timeout_handler_destroy_source (handler);
}

static gboolean
timeout_handler_dispatch (gpointer      data)
{
  TimeoutHandler *handler = data;

  dbus_timeout_handle (handler->timeout);
  
  return TRUE;
}

static void
dbus_source_add_timeout (DBusSource *dbus_source,
			 DBusTimeout *timeout)
{
  TimeoutHandler *handler;
  
  if (!dbus_timeout_get_enabled (timeout))
    return;
  
  g_assert (dbus_timeout_get_data (timeout) == NULL);

  handler = g_new0 (TimeoutHandler, 1);
  handler->dbus_source = dbus_source;
  handler->timeout = timeout;

  handler->source = g_timeout_source_new (dbus_timeout_get_interval (timeout));
  g_source_set_callback (handler->source, timeout_handler_dispatch, handler,
                         timeout_handler_source_finalized);
  g_source_attach (handler->source, dbus_source->context);
  g_source_unref (handler->source);

  /* handler->source is owned by the context here */
  dbus_source->timeouts = g_slist_prepend (dbus_source->timeouts, handler);

  dbus_timeout_set_data (timeout, handler, timeout_handler_timeout_freed);
}

static void
dbus_source_remove_timeout (DBusSource *source,
			    DBusTimeout *timeout)
{
  TimeoutHandler *handler;
  
  handler = dbus_timeout_get_data (timeout);

  if (handler == NULL)
    return;
  
  timeout_handler_destroy_source (handler);
}

static dbus_bool_t
add_watch (DBusWatch *watch,
	   gpointer   data)
{
  DBusSource *dbus_source = data;

  dbus_source_add_watch (dbus_source, watch);
  
  return TRUE;
}

static void
remove_watch (DBusWatch *watch,
	      gpointer   data)
{
  DBusSource *dbus_source = data;

  dbus_source_remove_watch (dbus_source, watch);
}

static void
watch_toggled (DBusWatch *watch,
               void      *data)
{
  /* Because we just exit on OOM, enable/disable is
   * no different from add/remove */
  if (dbus_watch_get_enabled (watch))
    add_watch (watch, data);
  else
    remove_watch (watch, data);
}

static dbus_bool_t
add_timeout (DBusTimeout *timeout,
	     void        *data)
{
  DBusSource *source = data;
  
  if (!dbus_timeout_get_enabled (timeout))
    return TRUE;

  dbus_source_add_timeout (source, timeout);

  return TRUE;
}

static void
remove_timeout (DBusTimeout *timeout,
		void        *data)
{
  DBusSource *source = data;

  dbus_source_remove_timeout (source, timeout);
}

static void
timeout_toggled (DBusTimeout *timeout,
                 void        *data)
{
  /* Because we just exit on OOM, enable/disable is
   * no different from add/remove
   */
  if (dbus_timeout_get_enabled (timeout))
    add_timeout (timeout, data);
  else
    remove_timeout (timeout, data);
}

static void
wakeup_main (void *data)
{
  DBusSource *source = data;

  if (!source->in_finalize)
    g_main_context_wakeup (source->context);
}

static void
dbus_source_finalize (GSource *source)
{
  DBusSource *dbus_source = (DBusSource *)source;
  GSList *l;

  dbus_source->in_finalize = TRUE;

  if (dbus_source->owner)
    {
      G_LOCK (context_map);
      if (context_map)
	g_hash_table_remove (context_map,
			     dbus_source);
      G_UNLOCK (context_map);
      g_free (dbus_source->owner);
    }
      
  dbus_connection_close (dbus_source->connection);
  dbus_connection_unref (dbus_source->connection);
  
  /* At this point we can't free the sources, because
   * that results in a deadlock on the context lock.
   * However, there is only two reasons for the dbus
   * source to be finalized, either the main context
   * died, and then all sources will be unref:ed anyway,
   * or because we manually destroyed it, and
   * dbus_source_destroy() frees all the sources anyway.
   * We just need to free any lists here.
   */

  for (l = dbus_source->ios; l != NULL; l = l->next)
    {
      IOHandler *handler = l->data;
      handler->dbus_source = NULL;
    }
  g_slist_free (dbus_source->ios);
  dbus_source->ios = NULL;
  
  for (l = dbus_source->timeouts; l != NULL; l = l->next)
    {
      TimeoutHandler *handler = l->data;
      handler->dbus_source = NULL;
    }
  g_slist_free (dbus_source->timeouts);
  dbus_source->timeouts = NULL;
}

static const GSourceFuncs dbus_source_funcs = {
  dbus_source_prepare,
  dbus_source_check,
  dbus_source_dispatch,
  dbus_source_finalize,
};

static DBusConnection *
get_connection_for_main_context (GMainContext *context,
				 const char *owner)
{
  DBusConnection *connection;
  DBusSource key;
  DBusSource *dbus_source;

  if (context == NULL)
    context = g_main_context_default ();
  
  connection = NULL;
  G_LOCK (context_map);
  
  if (context_map != NULL)
    {
      key.owner = (char *)owner;
      key.context = context;
      
      dbus_source = g_hash_table_lookup (context_map, &key);
      
      if (dbus_source)
	connection = dbus_source->connection;
    }
  
  G_UNLOCK (context_map);
  
  return connection;
}

static void
dbus_source_destroy (DBusSource *dbus_source)
{
  while (dbus_source->ios)
    io_handler_destroy_source (dbus_source->ios->data);

  while (dbus_source->timeouts)
    timeout_handler_destroy_source (dbus_source->timeouts->data);

  g_source_destroy ((GSource *)dbus_source);
}

static DBusSource *
set_connection_for_main_context (GMainContext *context,
				 const char *owner,
				 DBusConnection *connection)
{
  DBusSource *dbus_source;
  
  g_assert (connection != NULL);

  if (context == NULL)
    context = g_main_context_default ();
 
  dbus_source = (DBusSource *)
    g_source_new ((GSourceFuncs*)&dbus_source_funcs,
		  sizeof (DBusSource));
  dbus_source->context = context;
  dbus_source->connection = dbus_connection_ref (connection);
  dbus_source->owner = g_strdup (owner);
  
  if (!dbus_connection_set_watch_functions (connection,
                                            add_watch,
                                            remove_watch,
                                            watch_toggled,
                                            dbus_source, NULL))
    goto nomem;

  if (!dbus_connection_set_timeout_functions (connection,
                                              add_timeout,
                                              remove_timeout,
                                              timeout_toggled,
                                              dbus_source, NULL))
    goto nomem;
    
  dbus_connection_set_wakeup_main_function (connection,
					    wakeup_main,
					    dbus_source, NULL);

  g_source_attach ((GSource *)dbus_source, context);

  if (owner)
    {
      G_LOCK (context_map);
      if (context_map == NULL)
	context_map = g_hash_table_new_full (dbus_source_hash, dbus_source_equal,
					     NULL, NULL);
      g_hash_table_insert (context_map, dbus_source, dbus_source);
      G_UNLOCK (context_map);
    }

  return dbus_source;
  
 nomem:
  g_error ("Not enough memory to set up DBusConnection for use with GLib");
  return NULL;
}

void
_g_dbus_connection_setup_with_main (DBusConnection         *connection,
				    GMainContext           *context)
{
  DBusSource *source;
  
  source = set_connection_for_main_context (context, NULL, connection);
  g_source_unref ((GSource *)source);
}
