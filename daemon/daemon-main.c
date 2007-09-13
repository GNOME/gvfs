#include <config.h>

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <dbus/dbus.h>
#include "daemon-main.h"
#include <glib/gi18n.h>
#include <gvfsdaemon.h>
#include <gvfsbackend.h>

void
daemon_init (void)
{
  DBusConnection *connection;
  DBusError derror;
  
  dbus_threads_init_default ();
  g_thread_init (NULL);
  g_type_init ();

  dbus_error_init (&derror);
  connection = dbus_bus_get (DBUS_BUS_SESSION, &derror);
  if (connection == NULL)
    {
      g_printerr (_("Error connecting dbus: %s\n"), derror.message);
      dbus_error_free (&derror);
      exit (1);
    }
}

GMountSource *
daemon_parse_args (int argc, char *argv[], const char *default_type)
{
  GMountSource *mount_source;
  
  mount_source = NULL;
  if (argc > 1 && strcmp (argv[1], "--dbus") == 0)
    {
      if (argc < 4)
	{
	  g_printerr ("Usage: %s --dbus dbus-id object_path\n", argv[0]);
	  exit (1);
	}
      
      mount_source = g_mount_source_new_dbus (argv[2], argv[3], NULL);
    }
  else if (argc > 1 || default_type != NULL)
    {
      GMountSpec *mount_spec;
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
	      g_printerr ("Usage: %s key=value key=value ...\n", argv[0]);
	      exit (1);
	    }
	  
	  key = g_strndup (argv[i], p - argv[i]);
	  if (strcmp (key, "type") == 0)
	    found_type = TRUE;
	  
	  g_mount_spec_set (mount_spec, key, p+1);
	  g_print ("setting '%s' to '%s'\n", key, p+1);
	  g_free (key);
	}

      if (!found_type)
	{
	  g_printerr ("No mount type specified\n");
	  g_printerr ("Usage: %s key=value key=value ...\n", argv[0]);
	  exit (1);
	}
      
      mount_source = g_mount_source_new_null (mount_spec);
      g_mount_spec_unref (mount_spec);
    }

  return mount_source;
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
  GMountSource *mount_source;
  GError *error;
  int res;
  const char *type;

  mount_source = daemon_parse_args (argc, argv, default_type);

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
      dbus_error_init (&derror);
      connection = dbus_bus_get (DBUS_BUS_SESSION, &derror);
      if (connection == NULL)
	{
	  g_printerr (_("Error connecting dbus: %s\n"), derror.message);
	  dbus_error_free (&derror);
	  exit (1);
	}
      
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
	  g_mount_source_failed (mount_source, error);
	  exit (1);
	}
    }
  
  daemon = g_vfs_daemon_new (FALSE, FALSE);
  if (daemon == NULL)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   _("error starting mount daemon"));
      g_mount_source_failed (mount_source, error);
      exit (1);
    }

  g_vfs_daemon_set_max_threads (daemon, max_job_threads);  
  
  if (mount_source)
    {
      g_vfs_daemon_initiate_mount (daemon, mount_source);
      g_object_unref (mount_source);
    }
  
  loop = g_main_loop_new (NULL, FALSE);

  g_print ("Entering mainloop\n");
  g_main_loop_run (loop);
}

