#include <config.h>

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <sys/un.h>
#include <errno.h>

#include <glib/gi18n-lib.h>

#include "gvfserror.h"
#include "gvfsdaemondbus.h"
#include <gvfsdaemonprotocol.h>

#define DBUS_TIMEOUT_DEFAULT 30 * 1000 /* 1/2 min */

typedef struct {
  DBusConnection *bus;
  GHashTable *connections;
} ThreadLocalConnections;

typedef struct {
  int extra_fd;
  int extra_fd_count;
} VfsConnectionData;

typedef struct DBusSource DBusSource;

static gint32 vfs_data_slot = -1;
static GOnce once_init_dbus = G_ONCE_INIT;

static GStaticPrivate local_connections = G_STATIC_PRIVATE_INIT;

static DBusConnection *get_connection_for_main_context (GMainContext    *context,
							const char      *mountpoint);
static DBusSource     *set_connection_for_main_context (GMainContext    *context,
							const char      *mountpoint,
							DBusConnection  *connection);
static void            dbus_source_destroy             (DBusSource      *dbus_source);
static DBusConnection *get_connection_sync             (const char      *mountpoint,
							GError         **error);


static gpointer
vfs_dbus_init (gpointer arg)
{
  if (!dbus_connection_allocate_data_slot (&vfs_data_slot))
    g_error ("Unable to allocate data slot");

  return NULL;
}

static void
free_local_connections (ThreadLocalConnections *local)
{
  if (local->bus)
    {
      dbus_connection_close (local->bus);
      dbus_connection_unref (local->bus);
    }
  
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


static void
append_escaped_bus_name (GString *s,
			 const char *unescaped)
{
  char c;
  gboolean first;
  static const gchar hex[16] = "0123456789ABCDEF";

  while ((c = *unescaped++) != 0)
    {
      if (first)
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

  /* I don't think we can get reorders here, can we?
   * Its a sync per-thread connection after all
   */
  g_assert (fd_id == data->extra_fd_count);
  
  fd = receive_fd (data->extra_fd);
  if (fd != -1)
    data->extra_fd_count++;

  return fd;
}

typedef struct {
  char *mountpoint;
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
  DBusSource *get_connection_source;

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

  g_free (async_call->mountpoint);
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

  if (async_call->cancelled_tag)
    g_signal_handler_disconnect (async_call->cancellable,
				 async_call->cancelled_tag);

  reply = dbus_pending_call_steal_reply (pending);
  dbus_pending_call_unref (pending);

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
  DBusConnection *connection;
  VfsConnectionData *connection_data;
  DBusSource *source;

  reply = dbus_pending_call_steal_reply (pending);
  dbus_pending_call_unref (pending);

  /* Disconnect from mainloop and destroy connection */
  dbus_source_destroy (async_call->get_connection_source);
  g_source_unref ((GSource *)async_call->get_connection_source);
  async_call->get_connection_source = NULL;

  dbus_message_get_args (reply, NULL,
			 DBUS_TYPE_STRING, &address1,
			 DBUS_TYPE_STRING, &address2,
			 DBUS_TYPE_INVALID);

  /* TODO: This connect i sync, should be async */
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

  connection_data = g_new (VfsConnectionData, 1);
  connection_data->extra_fd = extra_fd;
  connection_data->extra_fd_count = 0;

  if (!dbus_connection_set_data (connection, vfs_data_slot, connection_data, connection_data_free))
    g_error ("Out of memory");

  async_call->connection = connection;
  source = set_connection_for_main_context (async_call->context,
					    async_call->mountpoint,
					    connection);
  g_source_unref ((GSource *)source); /* Owned by context */

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
  DBusError derror;
  GString *bus_name;
  DBusMessage *get_connection_message;
  DBusPendingCall *pending;
  DBusConnection *bus;

  /* Unfortunately dbus doesn't have an async get */
  dbus_error_init (&derror);
  bus = dbus_bus_get_private (DBUS_BUS_SESSION, &derror);
  dbus_connection_set_exit_on_disconnect (bus, FALSE);
  if (bus == NULL)
    {
      g_set_error (&async_call->io_error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Couldn't get main dbus connection: %s\n",
		   derror.message);
      dbus_error_free (&derror);
      g_idle_add (async_dbus_call_finish_at_idle, async_call);
      return;
    }
  
  /* Connect with mainloop */
  async_call->get_connection_source =
    set_connection_for_main_context (async_call->context, NULL, bus);
  
  bus_name = g_string_new (G_VFS_DBUS_MOUNTPOINT_NAME);
  append_escaped_bus_name (bus_name, async_call->mountpoint);
  get_connection_message = dbus_message_new_method_call (bus_name->str,
							 G_VFS_DBUS_DAEMON_PATH,
							 G_VFS_DBUS_DAEMON_INTERFACE,
							 G_VFS_DBUS_OP_GET_CONNECTION);
  g_string_free (bus_name, TRUE);
  
  if (get_connection_message == NULL)
    g_error ("Failed to allocate message");

  if (!dbus_connection_send_with_reply (bus,
					get_connection_message, &pending,
					DBUS_TIMEOUT_DEFAULT))
    g_error ("Failed to send message (oom)");
  
  dbus_message_unref (get_connection_message);
  
  /* The connection is also owned by the pending call & main context */
  dbus_connection_unref (bus);
  
  if (pending == NULL)
    {
      dbus_source_destroy (async_call->get_connection_source);
      g_source_unref ((GSource *)async_call->get_connection_source);
      async_call->get_connection_source = NULL;
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

void
_g_vfs_daemon_call_async (const char *mountpoint,
			  DBusMessage *message,
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
  async_call->mountpoint = g_strdup (mountpoint);
  async_call->message = dbus_message_ref (message);
  if (context)
    async_call->context = g_main_context_ref (context);
  if (cancellable)
    async_call->cancellable = g_object_ref (cancellable);
  async_call->callback = callback;
  async_call->callback_data = callback_data;
  async_call->op_callback = op_callback,
  async_call->op_callback_data = op_callback_data,
  
  async_call->connection = get_connection_for_main_context (context, mountpoint);
  if (async_call->connection != NULL)
    do_call_async (async_call);
  else
    open_connection_async (async_call);
}

DBusMessage *
_g_vfs_daemon_call_sync (const char *mountpoint,
			 DBusMessage *message,
			 DBusConnection **connection_out,
			 GCancellable *cancellable,
			 GError **error)
{
  DBusConnection *connection;
  DBusError derror;
  DBusMessage *reply;
  
  connection = get_connection_sync (mountpoint, error);
  if (connection == NULL)
    return NULL;

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return NULL;
    }

  /* TODO: We should handle cancellation while waiting for the reply if cancellable != NULL */
  dbus_error_init (&derror);
  reply = dbus_connection_send_with_reply_and_block (connection, message, -1,
						     &derror);
  if (!reply)
    {
      _g_error_from_dbus (&derror, error);
      dbus_error_free (&derror);
      return NULL;
    }

  if (connection_out)
    *connection_out = connection;
  
  return reply;
}

static DBusConnection *
get_connection_sync (const char *mountpoint,
		     GError **error)
{
  ThreadLocalConnections *local;
  DBusConnection *connection;
  DBusMessage *message, *reply;
  DBusError derror;
  GString *bus_name;
  char *address1, *address2;
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
  
  connection = g_hash_table_lookup (local->connections, mountpoint);
  if (connection != NULL)
    return connection;
  
  if (local->bus == NULL)
    {
      dbus_error_init (&derror);
      local->bus = dbus_bus_get_private (DBUS_BUS_SESSION, &derror);
      if (local->bus == NULL)
	{
	  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       "Couldn't get main dbus connection: %s\n",
		       derror.message);
	  dbus_error_free (&derror);
	  return NULL;
	}
    }

  bus_name = g_string_new (G_VFS_DBUS_MOUNTPOINT_NAME);
  append_escaped_bus_name (bus_name, mountpoint);
  message = dbus_message_new_method_call (bus_name->str,
					  G_VFS_DBUS_DAEMON_PATH,
					  G_VFS_DBUS_DAEMON_INTERFACE,
					  G_VFS_DBUS_OP_GET_CONNECTION);
  g_string_free (bus_name, TRUE);
  
  dbus_error_init (&derror);
  reply = dbus_connection_send_with_reply_and_block (local->bus, message, -1,
						     &derror);
  dbus_message_unref (message);

  if (!reply)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   derror.message);
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

  g_hash_table_insert (local->connections, g_strdup (mountpoint), connection);

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
  DBusConnection *connection; /**< the dbus connection, owned */
  GMainContext *context;      /**< the main context, not owed */
  char *mountpoint;
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
dbus_source_hash (gconstpointer key)
{
  const DBusSource *source = key;
  return g_str_hash (source->mountpoint) ^ (guint)source->context;
}
  
static gboolean
dbus_source_equal (gconstpointer a,
		   gconstpointer b)
{
  const DBusSource *aa = a;
  const DBusSource *bb = b;
  
  return
    strcmp (aa->mountpoint, bb->mountpoint) == 0 &&
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
io_handler_dispatch (GIOChannel   *source,
                     GIOCondition  condition,
                     gpointer      data)
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
  GIOChannel *channel;
  IOHandler *handler;
  int fd;
  
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

  /* TODO: We really don't need a full giochannel here... */
  fd = dbus_watch_get_fd (watch);
  channel = g_io_channel_unix_new (fd);
  
  handler->source = g_io_create_watch (channel, condition);
  g_source_set_callback (handler->source, (GSourceFunc) io_handler_dispatch, handler,
                         io_handler_source_finalized);
  g_source_attach (handler->source, dbus_source->context);
  g_source_unref (handler->source);
  
  /* handler->source is owned by the context here */
  dbus_source->ios = g_slist_prepend (dbus_source->ios, handler);
  
  dbus_watch_set_data (watch, handler, io_handler_watch_freed);
  g_io_channel_unref (channel);
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
  
  if (dbus_source->mountpoint)
    {
      G_LOCK (context_map);
      g_hash_table_remove (context_map, dbus_source);
      G_UNLOCK (context_map);
    }

  dbus_connection_close (dbus_source->connection);
  dbus_connection_unref (dbus_source->connection);
  
  g_free (dbus_source->mountpoint);

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
				 const char *mountpoint)
{
  DBusConnection *connection;
  DBusSource *dbus_source;
  
  connection = NULL;
  G_LOCK (context_map);
  
  if (context_map != NULL)
    {
      DBusSource dbus_source_key;
      
      dbus_source_key.mountpoint = (char *)mountpoint;
      dbus_source_key.context = context;
      
      dbus_source = g_hash_table_lookup (context_map, &dbus_source_key);
      
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
				 const char *mountpoint,
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
  dbus_source->mountpoint = g_strdup (mountpoint);

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


  if (mountpoint != NULL)
    {
      G_LOCK (context_map);
      
      if (context_map == NULL)
	context_map = g_hash_table_new (dbus_source_hash,
					dbus_source_equal);
      
      g_hash_table_insert (context_map,
			   dbus_source, dbus_source);
      
      G_UNLOCK (context_map);
    }

  g_source_attach ((GSource *)dbus_source, context);

  return dbus_source;
  
 nomem:
  g_error ("Not enough memory to set up DBusConnection for use with GLib");
  return NULL;
}
