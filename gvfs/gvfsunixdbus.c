#include <config.h>

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "gvfsunixdbus.h"
#include <gvfsdaemonprotocol.h>

typedef struct {
  DBusConnection *bus;
  GHashTable *connections;
} ThreadLocalConnections;

static gint32 vfs_data_slot = -1;

static GStaticPrivate local_connections = G_STATIC_PRIVATE_INIT;

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
unix_socket_connect (const char *address)
{
  int fd;
  const char *path;
  size_t path_len;
  struct sockaddr_un addr;
  gboolean abstract;

  fd = socket (PF_UNIX, SOCK_STREAM, 0);
  if (fd == -1)
    return -1;

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
      close (fd);
      return -1;
    }

  return fd;
}

static void
close_wrapper (gpointer p)
{
  close (GPOINTER_TO_INT (p));
}

DBusConnection *
_g_vfs_unix_get_connection_sync (const char *mountpoint,
				 gint *fd,
				 GError **error)
{
  static GOnce once_init = G_ONCE_INIT;
  ThreadLocalConnections *local;
  DBusConnection *connection;
  DBusMessage *message, *reply;
  DBusError derror;
  GString *bus_name;
  char *address1, *address2;
  int extra_fd;

  g_once (&once_init, vfs_dbus_init, NULL);

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
    {
      if (fd)
	*fd = GPOINTER_TO_INT (dbus_connection_get_data (connection , vfs_data_slot));
      return connection;
    }
  
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

  extra_fd = unix_socket_connect (address2);
  g_print ("extra fd: %d\n", extra_fd);

  dbus_error_init (&derror);
  connection = dbus_connection_open_private (address1, &derror);
  if (!connection)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   derror.message);
      dbus_message_unref (reply);
      dbus_error_free (&derror);
      return NULL;
    }
  dbus_message_unref (reply);

  if (!dbus_connection_set_data (connection, vfs_data_slot, GINT_TO_POINTER (extra_fd), close_wrapper))
    g_error ("Out of memory");

  g_hash_table_insert (local->connections, g_strdup (mountpoint), connection);

  if (fd)
    *fd = extra_fd;
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

gboolean
_g_error_from_dbus_message (DBusMessage *message, GError **error)
{
  const char *str;
  const char *name;
  char *m, *gerror_name, *end;
  GQuark domain;
  int code;

  if (dbus_message_get_type (message) != DBUS_MESSAGE_TYPE_ERROR)
    return FALSE;

  str = NULL;
  dbus_message_get_args (message, NULL,
                         DBUS_TYPE_STRING, &str,
                         DBUS_TYPE_INVALID);


  name = dbus_message_get_error_name (message);
  if (g_str_has_prefix (name, "org.glib.GError."))
    {
      gerror_name = g_strdup (name + strlen ("org.glib.GError."));
      end = strchr (gerror_name, '.');
      if (end)
	*end++ = 0;

      domain = g_quark_from_string (gerror_name);
      g_free (gerror_name);

      if (end)
	code = atoi (end);
      
      g_set_error (error, domain, code, str);
    }
  /* TODO: Special case other types, like DBUS_ERROR_NO_MEMORY etc? */
  else
    {
      m = g_strdup_printf ("DBus error %s: %s", name, str);
      g_set_error (error, G_FILE_ERROR,
		   G_FILE_ERROR_IO, m);

    }

  return TRUE;
}
