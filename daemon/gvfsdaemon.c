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
#include <sys/errno.h>
#include <errno.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <glib-object.h>
#include <gvfsdaemon.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsdaemonutils.h>
#include <gvfsjobmount.h>
#include <gvfsjobopenforread.h>
#include <gvfsjobopenforwrite.h>

enum {
  PROP_0
};

enum {
  SHUTDOWN,
  LAST_SIGNAL
};

typedef struct {
  char *obj_path;
  GVfsRegisterPathCallback callback;
  gpointer data;
  GDBusInterfaceSkeleton *session_skeleton;
  GHashTable *client_skeletons;
} RegisteredPath;

struct _GVfsDaemon
{
  GObject parent_instance;

  GMutex lock;
  gboolean main_daemon;

  GThreadPool *thread_pool;
  GHashTable *registered_paths;
  GHashTable *client_connections;
  GList *jobs;
  GList *job_sources;

  guint exit_tag;
  
  gint mount_counter;
  
  GDBusConnection *conn;
  GVfsDBusDaemon *daemon_skeleton;
  GVfsDBusMountable *mountable_skeleton;
  guint name_watcher;
  gboolean lost_main_daemon;
};

typedef struct {
  GVfsDaemon *daemon;
  char *socket_dir;
  GDBusServer *server;
  
  GDBusConnection *conn;
} NewConnectionData;

static guint signals[LAST_SIGNAL] = { 0 };

static void              g_vfs_daemon_get_property (GObject        *object,
						    guint           prop_id,
						    GValue         *value,
						    GParamSpec     *pspec);
static void              g_vfs_daemon_set_property (GObject        *object,
						    guint           prop_id,
						    const GValue   *value,
						    GParamSpec     *pspec);

static gboolean          handle_get_connection     (GVfsDBusDaemon        *object,
                                                    GDBusMethodInvocation *invocation,
                                                    gpointer               user_data);
static gboolean          handle_cancel             (GVfsDBusDaemon        *object,
                                                    GDBusMethodInvocation *invocation,
                                                    guint                  arg_serial,
                                                    gpointer               user_data);
static gboolean          daemon_handle_mount       (GVfsDBusMountable     *object,
                                                    GDBusMethodInvocation *invocation,
                                                    GVariant              *arg_mount_spec,
                                                    gboolean               arg_automount,
                                                    GVariant              *arg_mount_source,
                                                    gpointer               user_data);
static void              g_vfs_daemon_re_register_job_sources (GVfsDaemon *daemon);






G_DEFINE_TYPE (GVfsDaemon, g_vfs_daemon, G_TYPE_OBJECT)

static void
registered_path_free (RegisteredPath *data)
{
  g_free (data->obj_path);
  if (data->session_skeleton)
    {
      /* Unexport the interface skeleton on session bus */
      g_dbus_interface_skeleton_unexport (data->session_skeleton);
      g_object_unref (data->session_skeleton);
    }
  g_hash_table_destroy (data->client_skeletons);
  
  g_free (data);
}

static void
g_vfs_daemon_finalize (GObject *object)
{
  GVfsDaemon *daemon;

  daemon = G_VFS_DAEMON (object);

  g_assert (daemon->jobs == NULL);

  if (daemon->name_watcher)
    g_bus_unwatch_name (daemon->name_watcher);
  
  if (daemon->daemon_skeleton != NULL)
    {
      g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (daemon->daemon_skeleton));
      g_object_unref (daemon->daemon_skeleton);
    }
  if (daemon->mountable_skeleton != NULL)
    {
      g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (daemon->mountable_skeleton));
      g_object_unref (daemon->mountable_skeleton);
    }
  if (daemon->conn != NULL)
    g_object_unref (daemon->conn);
  
  g_hash_table_destroy (daemon->registered_paths);
  g_hash_table_destroy (daemon->client_connections);
  g_mutex_clear (&daemon->lock);

  if (G_OBJECT_CLASS (g_vfs_daemon_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_daemon_parent_class)->finalize) (object);
}

static void
g_vfs_daemon_class_init (GVfsDaemonClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_vfs_daemon_finalize;
  gobject_class->set_property = g_vfs_daemon_set_property;
  gobject_class->get_property = g_vfs_daemon_get_property;

  signals[SHUTDOWN] =
    g_signal_new ("shutdown",
		  G_TYPE_FROM_CLASS (gobject_class),
		  G_SIGNAL_RUN_LAST,
		  G_STRUCT_OFFSET (GVfsDaemonClass, shutdown),
		  NULL, NULL,
		  g_cclosure_marshal_VOID__VOID,
		  G_TYPE_NONE, 0);
}

static void
job_handler_callback (gpointer       data,
		      gpointer       user_data)
{
  GVfsJob *job = G_VFS_JOB (data);

  g_vfs_job_run (job);
}

static void
name_appeared_handler (GDBusConnection *connection,
                       const gchar *name,
                       const gchar *name_owner,
                       gpointer user_data)
{
  GVfsDaemon *daemon = G_VFS_DAEMON (user_data);

  if (strcmp (name, G_VFS_DBUS_DAEMON_NAME) == 0 &&
      *name_owner != 0 &&
      daemon->lost_main_daemon)
      {
        /* There is a new owner. Register mounts with it */
        g_vfs_daemon_re_register_job_sources (daemon);
      }
}

static void 
name_vanished_handler (GDBusConnection *connection,
                       const gchar *name,
                       gpointer user_data)
{
  GVfsDaemon *daemon = G_VFS_DAEMON (user_data);

  /* Ensure we react only to really lost daemon */ 
  daemon->lost_main_daemon = TRUE;
}

static void
g_vfs_daemon_init (GVfsDaemon *daemon)
{
  GError *error;
  gint max_threads = 1; /* TODO: handle max threads */

  daemon->thread_pool = g_thread_pool_new (job_handler_callback,
					   daemon,
					   max_threads,
					   FALSE, NULL);
  /* TODO: verify thread_pool != NULL in a nicer way */
  g_assert (daemon->thread_pool != NULL);

  g_mutex_init (&daemon->lock);

  daemon->mount_counter = 0;
  
  daemon->jobs = NULL;
  daemon->registered_paths =
    g_hash_table_new_full (g_str_hash, g_str_equal,
			   g_free, (GDestroyNotify)registered_path_free);

  /* This is where we store active client connections so when a new filter is registered,
   * we re-register them on all active connections */
  daemon->client_connections =
    g_hash_table_new_full (g_direct_hash, g_direct_equal, g_object_unref, NULL);

  daemon->conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  g_assert (daemon->conn != NULL);

  daemon->daemon_skeleton = gvfs_dbus_daemon_skeleton_new ();
  g_signal_connect (daemon->daemon_skeleton, "handle-get-connection", G_CALLBACK (handle_get_connection), daemon);
  g_signal_connect (daemon->daemon_skeleton, "handle-cancel", G_CALLBACK (handle_cancel), daemon);
  
  error = NULL;
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (daemon->daemon_skeleton),
                                         daemon->conn,
                                         G_VFS_DBUS_DAEMON_PATH,
                                         &error))
    {
      g_warning ("Error exporting daemon interface: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }

  daemon->mountable_skeleton = gvfs_dbus_mountable_skeleton_new ();
  g_signal_connect (daemon->mountable_skeleton, "handle-mount", G_CALLBACK (daemon_handle_mount), daemon);
  
  error = NULL;
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (daemon->mountable_skeleton),
      daemon->conn,
                                         G_VFS_DBUS_MOUNTABLE_PATH,
                                         &error))
    {
      g_warning ("Error exporting mountable interface: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }
}

static void
g_vfs_daemon_set_property (GObject         *object,
			   guint            prop_id,
			   const GValue    *value,
			   GParamSpec      *pspec)
{
  switch (prop_id)
    {
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
  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

GVfsDaemon *
g_vfs_daemon_new (gboolean main_daemon, gboolean replace)
{
  GVfsDaemon *daemon;
  GDBusConnection *conn;
  GError *error;

  error = NULL;
  conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!conn)
    {
      g_printerr ("Failed to connect to the D-BUS daemon: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      return NULL;
    }

  daemon = g_object_new (G_VFS_TYPE_DAEMON, NULL);
  daemon->main_daemon = main_daemon;
  
  if (! main_daemon)
    {
      daemon->name_watcher = g_bus_watch_name_on_connection (conn,
                                                             G_VFS_DBUS_DAEMON_NAME,
                                                             G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                                             name_appeared_handler,
                                                             name_vanished_handler,
                                                             daemon,
                                                             NULL);
    }

  g_object_unref (conn);
  
  return daemon;
}

void
g_vfs_daemon_set_max_threads (GVfsDaemon                    *daemon,
			      gint                           max_threads)
{
  g_thread_pool_set_max_threads (daemon->thread_pool, max_threads, NULL);
}

static gboolean
exit_at_idle (GVfsDaemon *daemon)
{
  g_signal_emit (daemon, signals[SHUTDOWN], 0);
  return FALSE;
}

static void
daemon_unschedule_exit (GVfsDaemon *daemon)
{
  if (daemon->exit_tag != 0)
    {
      g_source_remove (daemon->exit_tag);
      daemon->exit_tag = 0;
    }
}

static void
daemon_schedule_exit (GVfsDaemon *daemon)
{
  if (daemon->exit_tag == 0)
    daemon->exit_tag = g_timeout_add_seconds (1, (GSourceFunc)exit_at_idle, daemon);
}

static void
job_source_new_job_callback (GVfsJobSource *job_source,
			     GVfsJob *job,
			     GVfsDaemon *daemon)
{
  g_vfs_daemon_queue_job (daemon, job);
}

static void
job_source_closed_callback (GVfsJobSource *job_source,
			    GVfsDaemon *daemon)
{
  g_mutex_lock (&daemon->lock);
  
  daemon->job_sources = g_list_remove (daemon->job_sources,
				       job_source);
  
  g_signal_handlers_disconnect_by_func (job_source,
					(GCallback)job_source_new_job_callback,
					daemon);
  g_signal_handlers_disconnect_by_func (job_source,
					(GCallback)job_source_closed_callback,
					daemon);
  
  g_object_unref (job_source);

  if (daemon->job_sources == NULL)
    daemon_schedule_exit (daemon);
  
  g_mutex_unlock (&daemon->lock);
}

static void
re_register_jobs_cb (GVfsDBusMountTracker *proxy,
                     GAsyncResult *res,
                     gpointer user_data)
{
  GError *error = NULL;

  gvfs_dbus_mount_tracker_call_register_mount_finish (proxy,
                                                      res,
                                                      &error);
  g_debug ("re_register_jobs_cb, error: %p\n", error);
  g_clear_error (&error);
}

static void
g_vfs_daemon_re_register_job_sources (GVfsDaemon *daemon)
{
  GList *l;
  
  g_mutex_lock (&daemon->lock);

  for (l = daemon->job_sources; l != NULL; l = l->next)
    {
      if (G_VFS_IS_BACKEND (l->data))
	{
	  GVfsBackend *backend = G_VFS_BACKEND (l->data);

	  /* Only re-register if we registered before, not e.g
	     if we're currently mounting. */
	  if (g_vfs_backend_is_mounted (backend))
	    g_vfs_backend_register_mount (backend, (GAsyncReadyCallback) re_register_jobs_cb, NULL);
	}
    }
  
  g_mutex_unlock (&daemon->lock);
}

void
g_vfs_daemon_add_job_source (GVfsDaemon *daemon,
			     GVfsJobSource *job_source)
{
  g_debug ("Added new job source %p (%s)\n", job_source, g_type_name_from_instance ((gpointer)job_source));
  
  g_mutex_lock (&daemon->lock);

  daemon_unschedule_exit (daemon);
  
  g_object_ref (job_source);
  daemon->job_sources = g_list_append (daemon->job_sources,
					     job_source);
  g_signal_connect (job_source, "new_job",
		    (GCallback)job_source_new_job_callback, daemon);
  g_signal_connect (job_source, "closed",
		    (GCallback)job_source_closed_callback, daemon);
  
  g_mutex_unlock (&daemon->lock);
}

static void
unref_skeleton (gpointer object)
{
  GDBusInterfaceSkeleton *skeleton = object;

  g_dbus_interface_skeleton_unexport (skeleton);
  g_object_unref (skeleton);
}

static void
peer_register_skeleton (const gchar *obj_path,
                        RegisteredPath *reg_path,
                        GDBusConnection *dbus_conn)
{
  GDBusInterfaceSkeleton *skeleton;

  if (! g_hash_table_contains (reg_path->client_skeletons, dbus_conn))
    {
      skeleton = reg_path->callback (dbus_conn, obj_path, reg_path->data);
      g_hash_table_insert (reg_path->client_skeletons, dbus_conn, skeleton);
    }
  else
    {
      /* Interface skeleton has been already registered on the connection, skipping */
    }
}

static void
client_conn_register_skeleton (GDBusConnection *dbus_conn,
                               gpointer value,
                               RegisteredPath *reg_path)
{
  peer_register_skeleton (reg_path->obj_path, reg_path, dbus_conn);
}

/* This registers a dbus interface skeleton on *all* connections, client and session bus */
/* The object path needs to be unique globally. */
void
g_vfs_daemon_register_path (GVfsDaemon *daemon,
                            const char *obj_path,
                            GVfsRegisterPathCallback callback,
                            gpointer user_data)
{
  RegisteredPath *data;

  data = g_new0 (RegisteredPath, 1);
  data->obj_path = g_strdup (obj_path);
  data->callback = callback;
  data->data = user_data;
  data->client_skeletons = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)unref_skeleton);
  
  g_hash_table_insert (daemon->registered_paths, g_strdup (obj_path), data);
  
  /* Export the newly registered interface skeleton on session bus */
  /* TODO: change the way we export skeletons on connections once 
   *       https://bugzilla.gnome.org/show_bug.cgi?id=662718 is in place.
   */ 
  data->session_skeleton = callback (daemon->conn, obj_path, user_data);
  
  /* Export this newly registered path to all active client connections */
  g_hash_table_foreach (daemon->client_connections, (GHFunc) client_conn_register_skeleton, data);
}

void
g_vfs_daemon_unregister_path (GVfsDaemon *daemon,
			      const char *obj_path)
{
  g_hash_table_remove (daemon->registered_paths, obj_path);
}

/* NOTE: Might be emitted on a thread */
static void
job_new_source_callback (GVfsJob *job,
			 GVfsJobSource *job_source,
			 GVfsDaemon *daemon)
{
  g_vfs_daemon_add_job_source (daemon, job_source);
}

/* NOTE: Might be emitted on a thread */
static void
job_finished_callback (GVfsJob *job, 
		       GVfsDaemon *daemon)
{

  g_signal_handlers_disconnect_by_func (job,
					(GCallback)job_new_source_callback,
					daemon);
  g_signal_handlers_disconnect_by_func (job,
					(GCallback)job_finished_callback,
					daemon);

  g_mutex_lock (&daemon->lock);
  daemon->jobs = g_list_remove (daemon->jobs, job);
  g_mutex_unlock (&daemon->lock);
  
  g_object_unref (job);
}

void
g_vfs_daemon_queue_job (GVfsDaemon *daemon,
			GVfsJob *job)
{
  g_debug ("Queued new job %p (%s)\n", job, g_type_name_from_instance ((gpointer)job));
  
  g_object_ref (job);
  g_signal_connect (job, "finished", (GCallback)job_finished_callback, daemon);
  g_signal_connect (job, "new_source", (GCallback)job_new_source_callback, daemon);
  
  g_mutex_lock (&daemon->lock);
  daemon->jobs = g_list_prepend (daemon->jobs, job);
  g_mutex_unlock (&daemon->lock);
  
  /* Can we start the job immediately / async */
  if (!g_vfs_job_try (job))
    {
      /* Couldn't finish / run async, queue worker thread */
      g_thread_pool_push (daemon->thread_pool, job, NULL); /* TODO: Check error */
    }
}

static void
new_connection_data_free (void *memory)
{
  NewConnectionData *data = memory;
  gchar *socket;
  
  /* Remove the socket and dir after connected */
  if (data->socket_dir)
    {
      socket = g_strdup_printf ("%s/socket", data->socket_dir);
      g_unlink (socket);
      g_free (socket);
      rmdir (data->socket_dir);
      g_free (data->socket_dir);
    }

  g_free (data);
}

static void
peer_unregister_skeleton (const gchar *obj_path,
                          RegisteredPath *reg_path,
                          GDBusConnection *dbus_conn)
{
  g_hash_table_remove (reg_path->client_skeletons, dbus_conn);
}

static void
peer_connection_closed (GDBusConnection *connection,
                        gboolean         remote_peer_vanished,
                        GError          *error,
                        gpointer         user_data)
{
  GVfsDaemon *daemon = G_VFS_DAEMON (user_data);
  GList *l;
  GVfsDBusDaemon *daemon_skeleton;
  GVfsJob *job_to_cancel;

  do
    {
      job_to_cancel = NULL;

      g_mutex_lock (&daemon->lock);
      for (l = daemon->jobs; l != NULL; l = l->next)
        {
          GVfsJob *job = G_VFS_JOB (l->data);

          if (G_VFS_IS_JOB_DBUS (job) &&
              !g_vfs_job_is_cancelled (job) &&
              G_VFS_JOB_DBUS (job)->invocation &&
              g_dbus_method_invocation_get_connection (G_VFS_JOB_DBUS (job)->invocation) == connection)
            {
              job_to_cancel = g_object_ref (job);
              break;
            }
        }
      g_mutex_unlock (&daemon->lock);

      if (job_to_cancel)
        {
          g_vfs_job_cancel (job_to_cancel);
          g_object_unref (job_to_cancel);
        }
    }
  while (job_to_cancel != NULL);

  daemon_skeleton = g_object_get_data (G_OBJECT (connection), "daemon_skeleton");
  /* daemon_skeleton should be always valid in this case */
  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (daemon_skeleton));
  
  g_hash_table_remove (daemon->client_connections, connection);

  /* Unexport the registered interface skeletons */
  g_hash_table_foreach (daemon->registered_paths, (GHFunc) peer_unregister_skeleton, connection);

  /* The peer-to-peer connection was disconnected */
  g_signal_handlers_disconnect_by_data (connection, user_data);
  g_object_unref (connection);
}

static void
daemon_peer_connection_setup (GVfsDaemon *daemon,
                              GDBusConnection *dbus_conn,
			      NewConnectionData *data)
{
  GVfsDBusDaemon *daemon_skeleton;
  GError *error;

  daemon_skeleton = gvfs_dbus_daemon_skeleton_new ();
  g_signal_connect (daemon_skeleton, "handle-cancel", G_CALLBACK (handle_cancel), daemon);
  
  error = NULL;
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (daemon_skeleton),
                                         dbus_conn,
                                         G_VFS_DBUS_DAEMON_PATH,
                                         &error))
    {
      g_warning ("Failed to accept client: %s, %s (%s, %d)", "object registration failed", 
                 error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      g_object_unref (data->conn);
      goto error_out;
    }
  g_object_set_data_full (G_OBJECT (data->conn), "daemon_skeleton", daemon_skeleton, (GDestroyNotify) g_object_unref);
  
  /* Export registered interface skeletons on this new connection */
  g_hash_table_foreach (daemon->registered_paths, (GHFunc) peer_register_skeleton, dbus_conn);
  
  g_hash_table_insert (daemon->client_connections, g_object_ref (dbus_conn), NULL);

  g_signal_connect (data->conn, "closed", G_CALLBACK (peer_connection_closed), data->daemon);

 error_out:
  new_connection_data_free (data);
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
generate_address (char **address,
		  char **folder)
{
  *address = NULL;
  *folder = NULL;

#ifdef USE_ABSTRACT_SOCKETS
  {
    gchar  tmp[9];

    randomize_string (tmp);
    *address = g_strdup_printf ("unix:abstract=/dbus-vfs-daemon/socket-%s", tmp);
  }
#else
  {
    char *dir;
    
    dir = create_socket_dir ();
    *address = g_strdup_printf ("unix:path=%s/socket", dir);
    *folder = dir;
  }
#endif
}

static gboolean
daemon_new_connection_func (GDBusServer *server,
                            GDBusConnection *connection,
                            gpointer user_data)
{
  NewConnectionData *data;

  data = user_data;

  /* Take ownership */
  data->conn = g_object_ref (connection);

  daemon_peer_connection_setup (data->daemon, data->conn, data);

  /* Kill the server, no more need for it */
  g_dbus_server_stop (server);
  g_object_unref (server);
  
  return TRUE;
}

static gboolean
handle_get_connection (GVfsDBusDaemon *object,
                       GDBusMethodInvocation *invocation,
                       gpointer user_data)
{
  GVfsDaemon *daemon = G_VFS_DAEMON (user_data);
  GDBusServer *server;
  GError *error;
  gchar *address1;
  NewConnectionData *data;
  char *socket_dir;
  gchar *guid;
  
  generate_address (&address1, &socket_dir);

  data = g_new (NewConnectionData, 1);
  data->daemon = daemon;
  data->socket_dir = socket_dir;
  data->conn = NULL;

  guid = g_dbus_generate_guid ();
  error = NULL;
  server = g_dbus_server_new_sync (address1,
                                   G_DBUS_SERVER_FLAGS_NONE,
                                   guid,
                                   NULL, /* GDBusAuthObserver */
                                   NULL, /* GCancellable */
                                   &error);
  g_free (guid);

  if (server == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_printerr ("daemon: Error creating server at address %s: %s\n", address1, error->message);
      g_error_free (error);
      goto error_out;
    }

  g_dbus_server_start (server);
  data->server = server;

  g_signal_connect (server, "new-connection", G_CALLBACK (daemon_new_connection_func), data);
  
  gvfs_dbus_daemon_complete_get_connection (object,
                                            invocation,
                                            address1,
                                            "");

  g_free (address1);
  return TRUE;

 error_out:
  new_connection_data_free (data);
  g_free (address1);
  return TRUE;
}

static gboolean
handle_cancel (GVfsDBusDaemon *object,
               GDBusMethodInvocation *invocation,
               guint arg_serial,
               gpointer user_data)
{
  GVfsDaemon *daemon = G_VFS_DAEMON (user_data);
  GList *l;
  GVfsJob *job_to_cancel = NULL;

  g_mutex_lock (&daemon->lock);
  for (l = daemon->jobs; l != NULL; l = l->next)
    {
      GVfsJob *job = G_VFS_JOB (l->data);
      
      if (G_VFS_IS_JOB_DBUS (job) &&
          g_vfs_job_dbus_is_serial (G_VFS_JOB_DBUS (job),
                                    g_dbus_method_invocation_get_connection (invocation),
                                    arg_serial))
        {
          job_to_cancel = g_object_ref (job);
          break;
        }
    }
  g_mutex_unlock (&daemon->lock);

  if (job_to_cancel)
    {
      g_vfs_job_cancel (job_to_cancel);
      g_object_unref (job_to_cancel);
    }
  
  gvfs_dbus_daemon_complete_cancel (object, invocation);

  return TRUE;
}

static gboolean
daemon_handle_mount (GVfsDBusMountable *object,
                     GDBusMethodInvocation *invocation,
                     GVariant *arg_mount_spec,
                     gboolean arg_automount,
                     GVariant *arg_mount_source,
                     gpointer user_data)
{
  GVfsDaemon *daemon = G_VFS_DAEMON (user_data);
  GMountSpec *mount_spec;
  GMountSource *mount_source;
  
  mount_spec = g_mount_spec_from_dbus (arg_mount_spec);
  if (mount_spec == NULL)
    g_dbus_method_invocation_return_error_literal (invocation,
                                                   G_IO_ERROR,
                                                   G_IO_ERROR_INVALID_ARGUMENT,
                                                   "Error in mount spec");
  else 
    {
      mount_source = g_mount_source_from_dbus (arg_mount_source);
      g_vfs_daemon_initiate_mount (daemon, mount_spec, mount_source, arg_automount,
                                   object, invocation);
      g_object_unref (mount_source);
      g_mount_spec_unref (mount_spec);
    }
  
  return TRUE;
}

void
g_vfs_daemon_initiate_mount (GVfsDaemon *daemon,
			     GMountSpec *mount_spec,
			     GMountSource *mount_source,
			     gboolean is_automount,
			     GVfsDBusMountable *object,
			     GDBusMethodInvocation *invocation)
{
  const char *type;
  GType backend_type;
  char *obj_path;
  GVfsJob *job;
  GVfsBackend *backend;

  type = g_mount_spec_get_type (mount_spec);

  backend_type = G_TYPE_INVALID;
  if (type)
    backend_type = g_vfs_lookup_backend (type);

  if (backend_type == G_TYPE_INVALID)
    {
      if (invocation)
        g_dbus_method_invocation_return_error_literal (invocation,
                                                       G_IO_ERROR,
                                                       G_IO_ERROR_FAILED,
                                                       "Invalid backend type");
      else
	g_warning ("Error mounting: invalid backend type\n");
      return;
    }

  obj_path = g_strdup_printf ("/org/gtk/vfs/mount/%d", ++daemon->mount_counter);
  backend = g_object_new (backend_type,
			  "daemon", daemon,
			  "object-path", obj_path,
			  NULL);
  g_free (obj_path);
  
  g_vfs_daemon_add_job_source (daemon, G_VFS_JOB_SOURCE (backend));
  g_object_unref (backend);

  job = g_vfs_job_mount_new (mount_spec, mount_source, is_automount, object, invocation, backend);
  g_vfs_daemon_queue_job (daemon, job);
  g_object_unref (job);
}

/**
 * g_vfs_daemon_get_blocking_processes:
 * @daemon: A #GVfsDaemon.
 *
 * Gets all processes that blocks unmounting, e.g. processes with open
 * file handles.
 *
 * Returns: An array of #GPid. Free with g_array_unref().
 */
GArray *
g_vfs_daemon_get_blocking_processes (GVfsDaemon *daemon)
{
  GArray *processes;
  GList *l;

  processes = g_array_new (FALSE, FALSE, sizeof (GPid));
  for (l = daemon->job_sources; l != NULL; l = l->next)
    {
      if (G_VFS_IS_CHANNEL (l->data))
        {
          GPid pid;
          pid = g_vfs_channel_get_actual_consumer (G_VFS_CHANNEL (l->data));
          g_array_append_val (processes, pid);
        }
    }

  return processes;
}

void
g_vfs_daemon_run_job_in_thread (GVfsDaemon *daemon,
				GVfsJob    *job)
{
  g_thread_pool_push (daemon->thread_pool, job, NULL); /* TODO: Check error */
}

void
g_vfs_daemon_close_active_channels (GVfsDaemon *daemon,
				    GVfsBackend *backend)
{
  GList *l;

   for (l = daemon->job_sources; l != NULL; l = l->next)
      if (G_VFS_IS_CHANNEL (l->data) &&
          g_vfs_channel_get_backend (G_VFS_CHANNEL (l->data)) == backend)
        g_vfs_channel_force_close (G_VFS_CHANNEL (l->data));
}
