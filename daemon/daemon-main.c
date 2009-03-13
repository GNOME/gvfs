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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#include <glib.h>
#include <dbus/dbus.h>
#include "daemon-main.h"
#include <glib/gi18n.h>
#include <gvfsdaemon.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsbackend.h>

static char *spawner_id = NULL;
static char *spawner_path = NULL;

static gboolean print_debug = FALSE;

static void
log_debug (const gchar   *log_domain,
	   GLogLevelFlags log_level,
	   const gchar   *message,
	   gpointer	      unused_data)
{
  if (print_debug)
    g_print ("%s", message);
}

void
daemon_init (void)
{
  DBusConnection *connection;
  DBusError derror;

  setlocale (LC_ALL, "");

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
  
  dbus_threads_init_default ();
  g_thread_init (NULL);
  g_type_init ();

  g_log_set_handler (NULL, G_LOG_LEVEL_DEBUG, log_debug, NULL);

  
  dbus_error_init (&derror);
  connection = dbus_bus_get (DBUS_BUS_SESSION, &derror);
  if (connection == NULL)
    {
      g_printerr (_("Error connecting to D-Bus: %s"), derror.message);
      g_printerr ("\n");
      dbus_error_free (&derror);
      exit (1);
    }
}

void
daemon_setup (void)
{
  char *name, *up;

  up = g_ascii_strup (G_STRINGIFY (DEFAULT_BACKEND_TYPE), -1);
  /* translators: This is the default daemon's application name, 
   * the %s is the type of the backend, like "ftp" */
  name = g_strdup_printf (_("%s Filesystem Service"), up);
  g_set_application_name (name);
  g_free (name);
  g_free (up);
}

static void
send_spawned (DBusConnection *connection, gboolean succeeded, char *error_message)
{
  DBusMessage *message;
  dbus_bool_t dbus_succeeded;

  if (error_message == NULL)
    error_message = "";

  if (spawner_id == NULL || spawner_path == NULL)
    {
      if (!succeeded)
	{
	  g_printerr (_("Error: %s"), error_message);
	  g_printerr ("\n");
	}
      return;
    }
  
  message = dbus_message_new_method_call (spawner_id,
					  spawner_path,
					  G_VFS_DBUS_SPAWNER_INTERFACE,
					  G_VFS_DBUS_OP_SPAWNED);
  dbus_message_set_no_reply (message, TRUE);

  dbus_succeeded = succeeded;
  if (!dbus_message_append_args (message,
				 DBUS_TYPE_BOOLEAN, &dbus_succeeded,
				 DBUS_TYPE_STRING, &error_message,
				 DBUS_TYPE_INVALID))
    _g_dbus_oom ();
    
  dbus_connection_send (connection, message, NULL);
  /* Make sure the message is sent */
  dbus_connection_flush (connection);
}

GMountSpec *
daemon_parse_args (int argc, char *argv[], const char *default_type)
{
  GMountSpec *mount_spec;

  if (argc > 1 && strcmp (argv[1], "--debug") == 0)
    {
      print_debug = TRUE;
      argc--;
      argv++;
    }
  else if (g_getenv ("GVFS_DEBUG"))
    {
      print_debug = TRUE;
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
 	      g_printerr (_("Usage: %s key=value key=value ..."), argv[0]);
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
	  g_printerr (_("Usage: %s key=value key=value ..."), argv[0]);
          g_printerr ("\n");
	  exit (1);
	}
    }

  return mount_spec;
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
  DBusConnection *connection;
  GMainLoop *loop;
  GVfsDaemon *daemon;
  DBusError derror;
  GMountSpec *mount_spec;
  GMountSource *mount_source;
  GError *error;
  int res;
  const char *type;

  dbus_error_init (&derror);
  connection = dbus_bus_get (DBUS_BUS_SESSION, &derror);
  if (connection == NULL)
    {
      g_printerr (_("Error connecting to D-Bus: %s"), derror.message);
      g_printerr ("\n");
      dbus_error_free (&derror);
      exit (1);
    }
  
  mount_spec = daemon_parse_args (argc, argv, default_type);

  va_start (var_args, first_type_name);

  type = first_type_name;

  while (type != NULL)
    {
      GType backend_type = va_arg (var_args, GType);
      
      g_vfs_register_backend (backend_type, type);

      type = va_arg (var_args, char *);
    }

  error = NULL;
  if (mountable_name)
    {
      
      res = dbus_bus_request_name (connection,
				   mountable_name,
				   0, &derror);

      if (res != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
	{
	  if (res == -1)
	    _g_error_from_dbus (&derror, &error);
	  else
	    g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
			 _("mountpoint for %s already running"), mountable_name);

	  send_spawned (connection, FALSE, error->message);
	  g_error_free (error);
	  exit (1);
	}
    }
  
  daemon = g_vfs_daemon_new (FALSE, FALSE);
  if (daemon == NULL)
    {
      send_spawned (connection, FALSE, _("error starting mount daemon"));
      exit (1);
    }

  g_vfs_daemon_set_max_threads (daemon, max_job_threads);  
  
  send_spawned (connection, TRUE, NULL);
	  
  if (mount_spec)
    {
      mount_source = g_mount_source_new_dummy ();
      g_vfs_daemon_initiate_mount (daemon, mount_spec, mount_source, FALSE, NULL);
      g_mount_spec_unref (mount_spec);
      g_object_unref (mount_source);
    }
  
  loop = g_main_loop_new (NULL, FALSE);

  g_main_loop_run (loop);
}

