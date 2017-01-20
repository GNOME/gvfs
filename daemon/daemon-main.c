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

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#include <glib.h>
#include "daemon-main.h"
#include <glib/gi18n.h>
#include <gvfsdaemon.h>
#include <gvfsbackend.h>
#include <gvfsdbus.h>
#include <gvfsutils.h>

static char *spawner_id = NULL;
static char *spawner_path = NULL;

static gboolean already_acquired = FALSE;
static int process_result = 0;

static GMainLoop *loop;


static void
log_debug (const gchar   *log_domain,
	   GLogLevelFlags log_level,
	   const gchar   *message,
           gpointer       user_data)
{
  if (gvfs_get_debug ())
    g_print ("%s: %s", (const gchar *)user_data, message);
}

void
daemon_init (void)
{
  GDBusConnection *conn;
  GError *error;

  setlocale (LC_ALL, "");

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
  
  g_log_set_handler (NULL, G_LOG_LEVEL_DEBUG, log_debug, G_STRINGIFY (DEFAULT_BACKEND_TYPE));

  gvfs_setup_debug_handler ();

#ifdef SIGPIPE
  /* Ignore SIGPIPE to avoid killing daemons on cancelled transfer *
   * See https://bugzilla.gnome.org/show_bug.cgi?id=649041         *
   */
  signal (SIGPIPE, SIG_IGN);
#endif

  error = NULL;
  conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!conn)
    {
      g_printerr ("Error connecting to D-Bus: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      exit (1);
    }
  g_object_unref (conn);
}

void
daemon_setup (void)
{
  char *name, *up;

  up = g_ascii_strup (G_STRINGIFY (DEFAULT_BACKEND_TYPE), -1);
  /* translators: This is the default daemon's application name, 
   * the %s is the type of the backend, like "FTP" */
  name = g_strdup_printf (_("%s File System Service"), up);
  g_set_application_name (name);
  g_free (name);
  g_free (up);
}

typedef struct {
  GVfsDaemon *daemon;
  GMountSpec *mount_spec;
  int max_job_threads;
  char *mountable_name;
} DaemonData;

typedef struct {
  GDestroyNotify callback;
  gpointer user_data;
} SpawnData;

static void
spawned_failed_cb (gpointer user_data)
{
  process_result = 1;
  g_main_loop_quit (loop);
}

static void
spawned_succeeded_cb (gpointer user_data)
{
  DaemonData *data = user_data;
  GMountSource *mount_source;
  
  if (data->mount_spec)
    {
      mount_source = g_mount_source_new_dummy ();
      g_vfs_daemon_initiate_mount (data->daemon, data->mount_spec, mount_source, FALSE, NULL, NULL);
      g_mount_spec_unref (data->mount_spec);
      g_object_unref (mount_source);
    }
}

static void
call_spawned_cb (GVfsDBusSpawner *proxy,
                 GAsyncResult  *res,
                 gpointer user_data)
{
  GError *error = NULL;
  SpawnData *data = user_data;

  if (! gvfs_dbus_spawner_call_spawned_finish (proxy, res, &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_printerr ("call_spawned_cb: Error sending a message: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }
  
  data->callback (data->user_data);
  g_free (data);
}

static void
send_spawned (gboolean succeeded, 
              char *error_message,
              guint32 error_code,
              GDestroyNotify callback,
              gpointer user_data)
{
  GVfsDBusSpawner *proxy;
  GError *error;
  SpawnData *data;

  if (error_message == NULL)
    error_message = "";

  if (spawner_id == NULL || spawner_path == NULL)
    {
      if (!succeeded)
	{
	  g_printerr (_("Error: %s"), error_message);
	  g_printerr ("\n");
	}
      callback (user_data);
      return;
    }

  error = NULL;
  proxy = gvfs_dbus_spawner_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                    G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS | G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                    spawner_id,
                                                    spawner_path,
                                                    NULL,
                                                    &error);
  if (proxy == NULL)
    {
      g_printerr ("Error creating proxy: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      return;
    }
  
  data = g_new0 (SpawnData, 1);
  data->callback = callback;
  data->user_data = user_data;
  
  gvfs_dbus_spawner_call_spawned (proxy, 
                                  succeeded,
                                  error_message,
                                  error_code,
                                  NULL,
                                  (GAsyncReadyCallback) call_spawned_cb,
                                  data);

  g_object_unref (proxy);
}

GMountSpec *
daemon_parse_args (int argc, char *argv[], const char *default_type)
{
  GMountSpec *mount_spec;

  if (argc > 1 && strcmp (argv[1], "--debug") == 0)
    {
      gvfs_set_debug (TRUE);
      argc--;
      argv++;
    }
  else if (g_getenv ("GVFS_DEBUG"))
    {
      gvfs_set_debug (TRUE);
    }
  
  mount_spec = NULL;
  if (argc > 1 && strcmp (argv[1], "--spawner") == 0)
    {
      if (argc < 4)
	{
	  g_printerr (_("Usage: %s --spawner dbus-id object_path"), argv[0]);
          g_printerr ("\n");
	  exit (1);
	}

      spawner_id = argv[2];
      spawner_path = argv[3];
    }
  else if (argc > 1 || default_type != NULL)
    {
      gboolean found_type;
      int i;
      
      mount_spec = g_mount_spec_new (default_type);
      found_type = default_type != NULL;

      for (i = 1; i < argc; i++)
	{
	  char *p;
	  char *key;
	  
	  p = strchr (argv[i], '=');
	  if (p == NULL || p[1] == 0 || p == argv[i])
	    {
 	      g_printerr (_("Usage: %s key=value key=value …"), argv[0]);
              g_printerr ("\n");
	      exit (1);
	    }
	  
	  key = g_strndup (argv[i], p - argv[i]);
	  if (strcmp (key, "type") == 0)
	    found_type = TRUE;
	  
	  g_mount_spec_set (mount_spec, key, p+1);
	  g_debug ("setting '%s' to '%s'\n", key, p+1);
	  g_free (key);
	}

      if (!found_type)
	{
	  g_printerr (_("No mount type specified"));
          g_printerr ("\n");
	  g_printerr (_("Usage: %s key=value key=value …"), argv[0]);
          g_printerr ("\n");
	  exit (1);
	}
    }

  return mount_spec;
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  DaemonData *data = user_data;
  gchar *s;
  
  if (connection == NULL)
    {
      g_printerr ("A connection to the bus can't be made\n");
      process_result = 1;
    }
  else
    {
      if (already_acquired)
        {
          g_printerr ("Got NameLost, some other instance replaced us\n");
        }
      else
        {
          s = g_strdup_printf (_("mountpoint for %s already running"), data->mountable_name);
          send_spawned (FALSE, s, G_IO_ERROR_ALREADY_MOUNTED, spawned_failed_cb, data);
          g_free (s);
          return;
        }
    }
  g_main_loop_quit (loop);
}

static void
daemon_shutdown (GVfsDaemon *daemon,
		 GMainLoop  *loop)
{
  if (g_main_loop_is_running (loop))
    g_main_loop_quit (loop);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  DaemonData *data = user_data;

  already_acquired = TRUE;
  
  data->daemon = g_vfs_daemon_new (FALSE, FALSE);
  if (data->daemon == NULL)
    {
      send_spawned (FALSE, _("error starting mount daemon"), G_IO_ERROR_FAILED, spawned_failed_cb, data);
      return;
    }

  g_vfs_daemon_set_max_threads (data->daemon, data->max_job_threads);
  g_signal_connect (data->daemon, "shutdown",
		    G_CALLBACK (daemon_shutdown), loop);

  send_spawned (TRUE, NULL, 0, spawned_succeeded_cb, data);
}

void
daemon_main (int argc,
	     char *argv[],
	     int max_job_threads,
	     const char *default_type,
	     const char *mountable_name,
	     const char *first_type_name,
	     ...)
{
  va_list var_args;
  const char *type;
  guint name_owner_id;
  DaemonData *data;

  data = g_new0 (DaemonData, 1);
  data->mountable_name = g_strdup (mountable_name);
  data->max_job_threads = max_job_threads;
  data->mount_spec = daemon_parse_args (argc, argv, default_type);
  
  va_start (var_args, first_type_name);

  type = first_type_name;

  while (type != NULL)
    {
      GType backend_type = va_arg (var_args, GType);
      
      g_vfs_register_backend (backend_type, type);

      type = va_arg (var_args, char *);
    }
  va_end (var_args);

  loop = g_main_loop_new (NULL, FALSE);
  
  name_owner_id = 0;
  /* We want to own *some* name on the org.gtk.vfs.* namespace so that
     filtering for us works from a sandbox */
  if (data->mountable_name == NULL)
    data->mountable_name = g_strdup_printf ("org.gtk.vfs.mountpoint_%d", getpid ());

  name_owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                  data->mountable_name,
                                  G_BUS_NAME_OWNER_FLAGS_NONE,
                                  NULL,
                                  on_name_acquired,
                                  on_name_lost,
                                  data,
                                  NULL);

  g_main_loop_run (loop);
  
  g_clear_object (&data->daemon);
  g_free (data->mountable_name);
  g_free (data);
  if (name_owner_id != 0)
    g_bus_unown_name (name_owner_id);
  if (loop != NULL)
    g_main_loop_unref (loop);
  
  if (process_result)
    exit (process_result);
}
