#include <config.h>

#include <unistd.h>

#include <glib.h>
#include <glib-object.h>
#include <dbus-gmain.h>
#include <gvfsdaemonprotocol.h>

static void              daemon_unregistered_func (DBusConnection *conn,
						   gpointer        data);
static DBusHandlerResult daemon_message_func      (DBusConnection *conn,
						   DBusMessage    *message,
						   gpointer        data);

static void
daemon_connection_setup (DBusConnection  *dbus_conn)
{
  g_print ("Got connection: %p\n", dbus_conn);
}

static DBusObjectPathVTable daemon_vtable = {
	daemon_unregistered_func,
	daemon_message_func,
	NULL
};

#ifdef __linux__
#define USE_ABSTRACT_SOCKETS
#endif

static void
randomize_string (char tmp[9])
{
  int i;
  const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

  for (i = 0; i < 8; i++)
    tmp[i] = chars[g_random_int_range (0, sizeof(chars))];
  
  tmp[8] = '\0';
}

#ifndef USE_ABSTRACT_SOCKETS
static gboolean
test_safe_socket_dir (const char *dirname)
{
  struct stat statbuf;
  
  if (g_stat (dirname, &statbuf) != 0)
    return FALSE;
	
#ifndef G_PLATFORM_WIN32
  if (statbuf.st_uid != getuid ())
    return FALSE;
  
  if ((statbuf.st_mode & (S_IRWXG|S_IRWXO)) ||
      !S_ISDIR (statbuf.st_mode))
    return FALSE;
#endif

  return TRUE;
}


static char *
create_socket_dir (void)
{
  char *dirname;
  long iteration = 0;
  char *safe_dir;
  gchar tmp[9];
  int i;
  
  safe_dir = NULL;
  do
    {
      g_free (safe_dir);

      randomize_string (tmp);
		
      dirname = g_strdup_printf ("gvfs-%s-%s",
				 g_get_user_name (), tmp);
      safe_dir = g_build_filename (g_get_tmp_dir (), dirname, NULL);
      g_free (dirname);
      
      if (g_mkdir (safe_dir, 0700) < 0)
	{
	  switch (errno)
	    {
	    case EACCES:
	      g_error ("I can't write to '%s', daemon init failed",
		       safe_dir);
	      break;
	      
	    case ENAMETOOLONG:
	      g_error ("Name '%s' too long your system is broken",
		       safe_dir);
	      break;
	      
	    case ENOMEM:
#ifdef ELOOP
	    case ELOOP:
#endif
	    case ENOSPC:
	    case ENOTDIR:
	    case ENOENT:
	      g_error ("Resource problem creating '%s'", safe_dir);
	      break;
	      
	    default: /* carry on going */
	      break;
	    }
	}
      /* Possible race - so we re-scan. */
      
      if (iteration++ == 1000) 
	g_error ("Cannot find a safe socket path in '%s'", g_get_tmp_dir ());
    }
  while (!test_safe_socket_dir (safe_dir));

  return safe_dir;
}
#endif

static gchar *
generate_address (char **folder)
{
  gchar *path;
  
#ifdef USE_ABSTRACT_SOCKETS
  {
    gchar  tmp[9];
    randomize_string (tmp);
 
    path = g_strdup_printf ("unix:abstract=/dbus-vfs-daemon/socket-%s", tmp);
    *folder = NULL;
  }
#else
  {
    char *dir;
    
    dir = create_socket_dir ();
    path = g_strdup_printf ("unix:path=%s/socket", dir);
    *folder = dir;
  }
#endif

  return path;
}

typedef struct {
  char *socket_dir;
} NewConnectionData;

static void
new_connection_data_free (void *memory)
{
  NewConnectionData *data = memory;
  
  /* Remove the socket and dir after connected */
  if (data->socket_dir) 
    rmdir (data->socket_dir);
  
  g_free (data->socket_dir);
  g_free (data);
}

static void
daemon_new_connection_func (DBusServer     *server,
			    DBusConnection *conn,
			    gpointer        user_data)
{
  NewConnectionData *data;

  data = user_data;

  /* Take ownership */
  dbus_connection_ref (conn);
  
  daemon_connection_setup (conn);
  
  /* Kill the server, no more need for it */
  dbus_server_disconnect (server);
  dbus_server_unref (server);
}

static void
daemon_handle_get_connection (DBusConnection *conn,
			      DBusMessage *message)
{
  DBusServer    *server;
  DBusError      error;
  DBusMessage   *reply;
  gchar         *address;
  NewConnectionData *data;
  char *socket_dir;
  
  address = generate_address (&socket_dir);
  
  dbus_error_init (&error);
  
  server = dbus_server_listen (address, &error);
  if (!server)
    {
      reply = dbus_message_new_error (message,
				      G_VFS_DBUS_ERROR_SOCKET_FAILED,
				      "Failed to create new socket");
      if (!reply)
	g_error ("Out of memory");
    
      dbus_connection_send (conn, reply, NULL);
      dbus_message_unref (reply);
      
      g_free (address);
      if (socket_dir)
	{
	  rmdir (socket_dir);
	  g_free (socket_dir);
	}
      
      return;
    }
  
  data = g_new (NewConnectionData, 1);
  data->socket_dir = socket_dir;
  
  dbus_server_set_new_connection_function (server,
					   daemon_new_connection_func,
					   data, new_connection_data_free);
  dbus_server_setup_with_g_main (server, NULL);
  
  reply = dbus_message_new_method_return (message);
  dbus_message_append_args (reply,
			    DBUS_TYPE_STRING, &address,
			    DBUS_TYPE_INVALID);
  dbus_connection_send (conn, reply, NULL);
  dbus_message_unref (reply);
  g_free (address);
}


static void
daemon_unregistered_func (DBusConnection *conn,
			  gpointer        data)
{
}

static DBusHandlerResult
daemon_message_func (DBusConnection *conn,
		     DBusMessage    *message,
		     gpointer        data)
{
  g_print ("daemon_message_func\n");
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_DAEMON_INTERFACE,
				   G_VFS_DBUS_OP_GET_CONNECTION))
    daemon_handle_get_connection (conn, message);
  else
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  
  return DBUS_HANDLER_RESULT_HANDLED;
}

static gboolean
setup_daemon (char *mountpoint)
{
  DBusConnection *conn;
  DBusError error;
  int ret;

  dbus_error_init (&error);

  conn = dbus_bus_get (DBUS_BUS_SESSION, &error);
  if (!conn)
    {
      g_printerr ("Failed to connect to the D-BUS daemon: %s\n",
		  error.message);
      
      dbus_error_free (&error);
      return FALSE;
    }

  dbus_connection_setup_with_g_main (conn, NULL);
  
  ret = dbus_bus_request_name (conn, G_VFS_DBUS_MOUNTPOINT_NAME "foo_3A_2F_2F", 
			       0, &error);
  if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
    {
      g_warning ("Failed to acquire vfs-daemon service: %s", error.message);
      dbus_error_free (&error);
    }

  if (!dbus_connection_register_object_path (conn,
					     "/org/gtk/vfs/Daemon",
					     &daemon_vtable,
					     NULL))
    {
      g_printerr ("Failed to register object with D-BUS.\n");
      return FALSE;
    }
  
  
  return TRUE;
}

int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  
  g_type_init ();

  if (!setup_daemon ("foo"))
    return 1;
  
  loop = g_main_loop_new (NULL, FALSE);

  g_print ("Entering mainloop\n");
  g_main_loop_run (loop);
  
  return 0;
}
