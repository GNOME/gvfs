#include <config.h>

#include "gvfsunixdbus.h"

typedef struct {
  DBusConnection *bus;
  GHashTable *connections;
} ThreadLocalConnections;

static GStaticPrivate local_connections = G_STATIC_PRIVATE_INIT;

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

DBusConnection *
_g_vfs_unix_get_connection_sync (const char *mountpoint)
{
  ThreadLocalConnections *local;
  DBusConnection *connection;
  DBusMessage *message, *reply;
  DBusError error;
  char *bus_name;
  char *address;

  local = g_static_private_get (&local_connections);
  if (local == NULL)
    {
      local = g_new (ThreadLocalConnections, 1);
      local->connections = g_hash_table_new_full (g_str_hash, g_str_equal,
						  g_free, free_mount_connection);
      g_static_private_set (&local_connections, local, (GDestroyNotify)free_local_connections);
    }

  connection = g_hash_table_lookup (local->connections, mountpoint);
  if (connection != NULL)
    return connection;

  if (local->bus == NULL)
    {
      dbus_error_init (&error);
      local->bus = dbus_bus_get_private (DBUS_BUS_SESSION, &error);
      if (local->bus == NULL)
	{
	  g_printerr ("Couldn't get main dbus connection: %s\n",
		      error.message);
	  dbus_error_free (&error);
	  return NULL;
	}
    }

  /* TODO: Verify that mountpoint is ok as dbus name, if not, encode */
  bus_name = g_strconcat ("org.gtk.vfs.mountpoint.", mountpoint, NULL);
  message = dbus_message_new_method_call (bus_name,
					  "/org/gtk/vfs/Daemon",
					  "org.gtk.vfs.Daemon",
					  "GetConnection");
  g_free (bus_name);

  reply = dbus_connection_send_with_reply_and_block (local->bus, message, -1,
						     &error);
  dbus_message_unref (message);

  if (!reply)
    {
      g_warning ("Error while getting peer-to-peer connection: %s",
		 error.message);
      dbus_error_free (&error);
      return NULL;
    }
  
  dbus_message_get_args (reply, NULL,
			 DBUS_TYPE_STRING, &address,
			 DBUS_TYPE_INVALID);

  connection = dbus_connection_open_private (address, &error);
  if (!connection)
    {
      g_warning ("Failed to connect to peer-to-peer address (%s): %s",
		 address, error.message);
      dbus_message_unref (reply);
      dbus_error_free (&error);
      return NULL;
    }
  dbus_message_unref (reply);

  g_hash_table_insert (local->connections, g_strdup (mountpoint), connection);

  return connection;
}

