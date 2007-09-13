#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib-object.h>
#include <dbus-gmain.h>
#include <gvfsdaemon.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsdaemonutils.h>
#include <gvfsjobopenforread.h>

G_DEFINE_TYPE (GVfsDaemon, g_vfs_daemon, G_TYPE_OBJECT);

enum {
  PROP_0,
  PROP_MOUNTPOINT
};

struct _GVfsDaemonPrivate
{
  GMutex *lock; /* protects the parts that are accessed by multiple threads */
  char *mountpoint;
  gboolean active;

  GQueue *pending_jobs; 
  GQueue *jobs; /* protected by lock */
  guint queued_job_start; /* protected by lock */
  GList *read_streams; /* protected by lock */
};

static void g_vfs_daemon_get_property (GObject    *object,
				       guint       prop_id,
				       GValue     *value,
				       GParamSpec *pspec);
static void g_vfs_daemon_set_property (GObject         *object,
				       guint            prop_id,
				       const GValue    *value,
				       GParamSpec      *pspec);
static GObject*g_vfs_daemon_constructor (GType                  type,
					 guint                  n_construct_properties,
					 GObjectConstructParam *construct_params);
static void              daemon_unregistered_func (DBusConnection *conn,
						   gpointer        data);
static DBusHandlerResult daemon_message_func      (DBusConnection *conn,
						   DBusMessage    *message,
						   gpointer        data);
static void              start_or_queue_job       (GVfsDaemon     *daemon,
						   GVfsJob        *job);

static DBusObjectPathVTable daemon_vtable = {
	daemon_unregistered_func,
	daemon_message_func,
	NULL
};

static void
g_vfs_daemon_finalize (GObject *object)
{
  GVfsDaemon *daemon;

  daemon = G_VFS_DAEMON (object);

  g_assert (daemon->priv->jobs->head == NULL);
  g_assert (daemon->priv->pending_jobs->head == NULL);
  g_queue_free (daemon->priv->jobs);
  g_queue_free (daemon->priv->pending_jobs);

  g_object_unref (daemon->backend);
  g_mutex_free (daemon->priv->lock);

  g_free (daemon->priv->mountpoint);

  if (G_OBJECT_CLASS (g_vfs_daemon_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_daemon_parent_class)->finalize) (object);
}

static void
g_vfs_daemon_class_init (GVfsDaemonClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GVfsDaemonPrivate));
  
  gobject_class->finalize = g_vfs_daemon_finalize;
  gobject_class->set_property = g_vfs_daemon_set_property;
  gobject_class->get_property = g_vfs_daemon_get_property;
  gobject_class->constructor = g_vfs_daemon_constructor;

  g_object_class_install_property (gobject_class,
				   PROP_MOUNTPOINT,
				   g_param_spec_string ("mountpoint",
							"Mountpoint",
							"Mountpoint of the daemon.",
							NULL,
							G_PARAM_READWRITE | 
							G_PARAM_CONSTRUCT_ONLY));
}

static void
g_vfs_daemon_init (GVfsDaemon *daemon)
{
  daemon->priv = G_TYPE_INSTANCE_GET_PRIVATE (daemon,
					      G_TYPE_VFS_DAEMON,
					      GVfsDaemonPrivate);
  daemon->priv->lock = g_mutex_new ();
  daemon->priv->jobs = g_queue_new ();
  daemon->priv->pending_jobs = g_queue_new ();
}

static GObject*
g_vfs_daemon_constructor (GType                  type,
			  guint                  n_construct_properties,
			  GObjectConstructParam *construct_params)
{
  DBusConnection *conn;
  DBusError error;
  GObject *object;
  GVfsDaemon *daemon;
  int ret;

  object =
    G_OBJECT_CLASS (g_vfs_daemon_parent_class)->constructor (type,
							     n_construct_properties,
							     construct_params);

  daemon = G_VFS_DAEMON (object);

  conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);
  
  dbus_error_init (&error);

  ret = dbus_bus_request_name (conn, daemon->priv->mountpoint, 0, &error);
  g_print ("ret: %d\n", ret);
  if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
    {
      g_printerr ("Failed to acquire vfs-daemon service: %s", error.message);
      dbus_error_free (&error);
    }
  else
    {
      if (!dbus_connection_register_object_path (conn,
						 G_VFS_DBUS_DAEMON_PATH,
						 &daemon_vtable,
						 daemon))
	g_printerr ("Failed to register object with D-BUS.\n");
      else
	daemon->priv->active = TRUE;
    }
  
  return object;
}


static void
g_vfs_daemon_set_property (GObject         *object,
			   guint            prop_id,
			   const GValue    *value,
			   GParamSpec      *pspec)
{
  GVfsDaemon *daemon;
  gchar *tmp;
  
  daemon = G_VFS_DAEMON (object);
  
  switch (prop_id)
    {
    case PROP_MOUNTPOINT:
      tmp = daemon->priv->mountpoint;
      daemon->priv->mountpoint = g_value_dup_string (value);
      g_free (tmp);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_vfs_daemon_get_property (GObject    *object,
			   guint       prop_id,
			   GValue     *value,
			   GParamSpec *pspec)
{
  GVfsDaemon *daemon;
  
  daemon = G_VFS_DAEMON (object);

  switch (prop_id)
    {
    case PROP_MOUNTPOINT:
      g_value_set_string (value, daemon->priv->mountpoint);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

GVfsDaemon *
g_vfs_daemon_new (const char *mountpoint,
		  GVfsBackend *backend)
{
  GVfsDaemon *daemon;

  daemon = g_object_new (G_TYPE_VFS_DAEMON,
			 "mountpoint", mountpoint,
			 NULL);
  daemon->backend = g_object_ref (backend);

  return daemon;
}

static gboolean
start_jobs_at_idle (gpointer data)
{
  GVfsDaemon *daemon = data;
  GList *l, *next;
  GVfsJob *job;

  g_mutex_lock (daemon->priv->lock);
  daemon->priv->queued_job_start = 0;
  g_mutex_unlock (daemon->priv->lock);

  l = daemon->priv->pending_jobs->head;
  while (l != NULL)
    {
      job = l->data;
      next = l->next;

      if (g_vfs_job_start (job))
	g_queue_delete_link (daemon->priv->pending_jobs, l);
      
      l = next;
    }
  
  return FALSE;
}

/* Called with lock held */
static void
queue_start_jobs_at_idle (GVfsDaemon *daemon)
{
  if (daemon->priv->queued_job_start == 0)
    daemon->priv->queued_job_start = g_idle_add (start_jobs_at_idle, daemon);
}


static void
handle_new_job_callback (GVfsReadStream *stream,
			 GVfsJob *job,
			 GVfsDaemon *daemon)
{
  g_print ("handle_new_job_callback() job=%p daemon=%p\n", job, daemon);
  g_object_ref (job);
  start_or_queue_job (daemon, job);
}

/* NOTE: Might be emitted on a thread */
static void
job_finished_callback (GVfsJob *job, 
		       GVfsDaemon *daemon)
{
  g_mutex_lock (daemon->priv->lock);
  
  g_queue_remove (daemon->priv->jobs, job);
  
  queue_start_jobs_at_idle (daemon);

  if (G_IS_VFS_JOB_OPEN_FOR_READ (job))
    {
      GVfsJobOpenForRead *open_job = G_VFS_JOB_OPEN_FOR_READ (job);
      GVfsReadStream *stream;
      
      stream = g_vfs_job_open_for_read_steal_stream (open_job);

      if (stream)
	{
	  g_print ("Got new read stream %p for daemon %p\n", stream, daemon);
	  daemon->priv->read_streams = g_list_append (daemon->priv->read_streams,
						      stream);
	  g_signal_connect (stream, "new_job", (GCallback)handle_new_job_callback, daemon);
	}
    }
  
  g_mutex_unlock (daemon->priv->lock);
  
  g_object_unref (job);
}

static void
start_or_queue_job (GVfsDaemon *daemon,
		    GVfsJob *job)
{
  g_vfs_job_set_backend (job, daemon->backend);
  g_mutex_lock (daemon->priv->lock);
  g_queue_push_tail (daemon->priv->jobs, job);
  g_mutex_unlock (daemon->priv->lock);
  g_signal_connect (job, "finished", (GCallback)job_finished_callback, daemon);
  
  /* Can we start the job immediately */
  if (!g_vfs_job_start (job))
    {
      /* Didn't start, queue as pending */
      g_queue_push_tail (daemon->priv->pending_jobs, job);
    }
}

static void
daemon_peer_connection_setup (GVfsDaemon *daemon,
			      DBusConnection  *dbus_conn,
			      int extra_fd)
{
  dbus_connection_setup_with_g_main (dbus_conn, NULL);

  if (!dbus_connection_register_object_path (dbus_conn,
					     G_VFS_DBUS_DAEMON_PATH,
					     &daemon_vtable,
					     daemon))
    {
      dbus_connection_unref (dbus_conn);
      close (extra_fd);
      g_printerr ("Failed to register object with new dbus peer connection.\n");
    }

  dbus_connection_add_fd_send_fd (dbus_conn, extra_fd);
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
    tmp[i] = chars[g_random_int_range (0, strlen(chars))];
  
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
  GVfsDaemon *daemon;
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

  daemon_peer_connection_setup (data->daemon, conn, data->socket);

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
			      DBusMessage *message,
			      GVfsDaemon *daemon)
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
  data->daemon = daemon;
  data->socket = -1;
  data->socket_dir = socket_dir;
  
  dbus_error_init (&error);
  server = dbus_server_listen (address1, &error);
  if (!server)
    {
      reply = dbus_message_new_error_printf (message,
					     G_VFS_DBUS_ERROR_SOCKET_FAILED,
					     "Failed to create new socket: %s", 
					     error.message);
      dbus_error_free (&error);
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
  /* TODO: Check OOM */
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
  GVfsDaemon *daemon = data;
  GVfsJob *job;
  
  job = NULL;
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_DAEMON_INTERFACE,
				   G_VFS_DBUS_OP_GET_CONNECTION))
    daemon_handle_get_connection (conn, message, daemon);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_DAEMON_INTERFACE,
					G_VFS_DBUS_OP_OPEN_FOR_READ))
    job = g_vfs_job_open_for_read_new (conn, message);
  else
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  
  if (job)
    start_or_queue_job (daemon, job);

  return DBUS_HANDLER_RESULT_HANDLED;
}

gboolean
 g_vfs_daemon_is_active (GVfsDaemon *daemon)
{
  return daemon->priv->active;
}

