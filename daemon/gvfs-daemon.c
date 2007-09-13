#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib-object.h>
#include <dbus-gmain.h>
#include <gvfsdaemonprotocol.h>

static gint32 extra_fd_slot = -1;

static void              daemon_unregistered_func (DBusConnection *conn,
						   gpointer        data);
static DBusHandlerResult daemon_message_func      (DBusConnection *conn,
						   DBusMessage    *message,
						   gpointer        data);

static int
send_fd (int connection_fd, 
	 int fd)
{
  struct msghdr msg;
  struct iovec vec;
  char buf[1] = {'x'};
  char ccmsg[CMSG_SPACE (sizeof (fd))];
  struct cmsghdr *cmsg;
  int ret;
  
  msg.msg_name = NULL;
  msg.msg_namelen = 0;

  vec.iov_base = buf;
  vec.iov_len = 1;
  msg.msg_iov = &vec;
  msg.msg_iovlen = 1;
  msg.msg_control = ccmsg;
  msg.msg_controllen = sizeof (ccmsg);
  cmsg = CMSG_FIRSTHDR (&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN (sizeof(fd));
  *(int*)CMSG_DATA (cmsg) = fd;
  msg.msg_controllen = cmsg->cmsg_len;
  msg.msg_flags = 0;

  ret = sendmsg (connection_fd, &msg, 0);
  g_print ("sendmesg ret: %d\n", ret);
  return ret;
}

static void
daemon_handle_read_file (DBusConnection *conn,
			 DBusMessage *message)
{
  DBusMessage *reply;
  char *str = "YAY";
  int fd;
  int socket_fds[2];
  int ret;

  reply = dbus_message_new_method_return (message);
  dbus_message_append_args (reply,
			    DBUS_TYPE_STRING, &str,
			    DBUS_TYPE_INVALID);
  dbus_connection_send (conn, reply, NULL);
  dbus_message_unref (reply);

  ret = socketpair (AF_UNIX, SOCK_STREAM, 0, socket_fds);
  fd = GPOINTER_TO_INT (dbus_connection_get_data (conn, extra_fd_slot));
  send_fd (fd, socket_fds[0]);
}

static DBusObjectPathVTable daemon_vtable = {
	daemon_unregistered_func,
	daemon_message_func,
	NULL
};

static void
close_wrapper (gpointer p)
{
  close (GPOINTER_TO_INT (p));
}

static void
daemon_peer_connection_setup (DBusConnection  *dbus_conn,
			      int extra_fd)
{
  if (extra_fd_slot == -1)
    {
      if (!dbus_connection_allocate_data_slot (&extra_fd_slot))
	g_error ("Unable to allocate data slot");
    }

  dbus_connection_setup_with_g_main (dbus_conn, NULL);

  if (!dbus_connection_register_object_path (dbus_conn,
					     G_VFS_DBUS_DAEMON_PATH,
					     &daemon_vtable,
					     NULL))
    {
      dbus_connection_unref (dbus_conn);
      close (extra_fd);
      g_printerr ("Failed to register object with new dbus peer connection.\n");
    }

  if (!dbus_connection_set_data (dbus_conn, extra_fd_slot, GINT_TO_POINTER (extra_fd), close_wrapper))
    g_error ("Out of memory");
  
}

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

static void
generate_addresses (char **address1,
		    char **address2,
		    char **folder)
{
  *address1 = NULL;
  *address2 = NULL;
  *folder = NULL;

#ifdef USE_ABSTRACT_SOCKETS
  {
    gchar  tmp[9];

    randomize_string (tmp);
    *address1 = g_strdup_printf ("unix:abstract=/dbus-vfs-daemon/socket-%s", tmp);

    randomize_string (tmp);
    *address2 = g_strdup_printf ("unix:abstract=/dbus-vfs-daemon/socket-%s", tmp);
  }
#else
  {
    char *dir;
    
    dir = create_socket_dir ();
    *address1 = g_strdup_printf ("unix:path=%s/socket1", dir);
    *address2 = g_strdup_printf ("unix:path=%s/socket2", dir);
    *folder = dir;
  }
#endif
}

typedef struct {
  char *socket_dir;
  guint io_watch;
  int socket;
} NewConnectionData;

static void
new_connection_data_free (void *memory)
{
  NewConnectionData *data = memory;
  
  /* Remove the socket and dir after connected */
  if (data->socket_dir) 
    rmdir (data->socket_dir);

  if (data->io_watch)
    g_source_remove (data->io_watch);
  
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
  
  daemon_peer_connection_setup (conn, data->socket);

  /* Kill the server, no more need for it */
  dbus_server_disconnect (server);
  dbus_server_unref (server);
}

static int
unix_socket_at (const char *address)
{
  int fd;
  const char *path;
  size_t path_len;
  struct sockaddr_un addr;

  fd = socket (PF_UNIX, SOCK_STREAM, 0);
  if (fd == -1)
    return -1;

#ifdef USE_ABSTRACT_SOCKETS  
  path = address + strlen ("unix:abstract=");
#else
  path = address + strlen ("unix:path=");
#endif
    
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  path_len = strlen (path);

#ifdef USE_ABSTRACT_SOCKETS
  addr.sun_path[0] = '\0'; /* this is what says "use abstract" */
  path_len++; /* Account for the extra nul byte added to the start of sun_path */

  strncpy (&addr.sun_path[1], path, path_len);
#else /* USE_ABSTRACT_SOCKETS */
  strncpy (addr.sun_path, path, path_len);
  unlink (path);
#endif /* ! USE_ABSTRACT_SOCKETS */
  
  if (bind (fd, (struct sockaddr*) &addr, 
	    G_STRUCT_OFFSET (struct sockaddr_un, sun_path) + path_len) < 0)
    {
      close (fd);
      return -1;
    }

  if (listen (fd, 30 /* backlog */) < 0)
    {
      close (fd);
      return -1;
    }
  
  return fd;
}

static gboolean
accept_new_fd_client (GIOChannel  *channel,
		      GIOCondition cond,
		      gpointer     callback_data)
{
  NewConnectionData *data = callback_data;
  int fd;
  int new_fd;
  struct sockaddr_un addr;
  socklen_t addrlen;

  fd = g_io_channel_unix_get_fd (channel);
  
  addrlen = sizeof (addr);
  new_fd = accept (fd, (struct sockaddr *) &addr, &addrlen);
  data->socket = new_fd;
  data->io_watch = 0;

  return FALSE;
}

static void
daemon_handle_get_connection (DBusConnection *conn,
			      DBusMessage *message)
{
  DBusServer    *server;
  DBusError      error;
  DBusMessage   *reply;
  gchar         *address1;
  gchar         *address2;
  NewConnectionData *data;
  GIOChannel *channel;
  char *socket_dir;
  int fd;
  
  generate_addresses (&address1, &address2, &socket_dir);

  data = g_new (NewConnectionData, 1);
  data->socket = -1;
  data->socket_dir = socket_dir;
  
  dbus_error_init (&error);
  server = dbus_server_listen (address1, &error);
  if (!server)
    {
      reply = dbus_message_new_error (message,
				      G_VFS_DBUS_ERROR_SOCKET_FAILED,
				      "Failed to create new socket");
      if (reply)
	{
	  dbus_connection_send (conn, reply, NULL);
	  dbus_message_unref (reply);
	}
      
      goto error_out;
    }

  dbus_server_set_new_connection_function (server,
					   daemon_new_connection_func,
					   data, new_connection_data_free);
  dbus_server_setup_with_g_main (server, NULL);
  
  fd = unix_socket_at (address2);
  if (fd == -1)
    goto error_out;

  channel = g_io_channel_unix_new (fd);
  data->io_watch = g_io_add_watch (channel, G_IO_IN | G_IO_HUP, accept_new_fd_client, data);
  g_io_channel_unref (channel);
    
  reply = dbus_message_new_method_return (message);
  dbus_message_append_args (reply,
			    DBUS_TYPE_STRING, &address1,
			    DBUS_TYPE_STRING, &address2,
			    DBUS_TYPE_INVALID);
  dbus_connection_send (conn, reply, NULL);
  dbus_message_unref (reply);

  g_free (address1);
  g_free (address2);

  return;

 error_out:
  g_free (data);
  g_free (address1);
  g_free (address2);
  if (socket_dir)
    {
      rmdir (socket_dir);
      g_free (socket_dir);
    }
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
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_DAEMON_INTERFACE,
				   G_VFS_DBUS_OP_GET_CONNECTION))
    daemon_handle_get_connection (conn, message);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_DAEMON_INTERFACE,
					G_VFS_DBUS_OP_READ_FILE))
    daemon_handle_read_file (conn, message);
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
					     G_VFS_DBUS_DAEMON_PATH,
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
