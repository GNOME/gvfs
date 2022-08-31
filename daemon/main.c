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

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include "gvfsdaemon.h"
#include "gvfsbackendtest.h"
#include <gvfsdaemonprotocol.h>
#include <gvfsutils.h>
#include "mount.h"
#include <locale.h>



static GMainLoop *loop;
static gboolean already_acquired = FALSE;
static int process_result = 0;

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
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
          g_printerr ("Failed to acquire daemon name, perhaps the VFS daemon is already running?\n");
          process_result = 1;
        }
    }
  g_main_loop_quit (loop);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  already_acquired = TRUE;

#ifdef HAVE_FUSE
  gboolean no_fuse = GPOINTER_TO_UINT (user_data);

  if (!no_fuse)
    {
      char *fuse_path;
      char *argv2[6];
      
      /* Use the old .gvfs location as fallback, not .cache/gvfs */
      if (g_strcmp0 (g_get_user_runtime_dir(), g_get_user_cache_dir ()) == 0)
        fuse_path = g_build_filename (g_get_home_dir(), ".gvfs", NULL);
      else
        fuse_path = g_build_filename (g_get_user_runtime_dir (), "gvfs", NULL);
      
      if (!g_file_test (fuse_path, G_FILE_TEST_EXISTS))
        g_mkdir (fuse_path, 0700);
      
      /* The -f (foreground) option prevent libfuse to call daemon(). */
      /* First, this is not required as g_spawn_async() already       */
      /* detach the process. Secondly, calling daemon() and then      */
      /* pthread_create() produce an undefined result accoring to     */
      /* Opengroup. On system with the uClibc library this will badly */
      /* hang the process.                                            */
      argv2[0] = LIBEXEC_DIR "/gvfsd-fuse";
      argv2[1] = fuse_path;
      argv2[2] = "-f";
      argv2[3] = NULL;
      
      g_spawn_async (NULL,
                     argv2,
                     NULL,
                     G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                     NULL, NULL,
                     NULL, NULL);
      
      g_free (fuse_path);
    }
#endif
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  if (!mount_init ())
    {
      /* we were not able to properly initialize ourselves, bail out */
      g_main_loop_quit (loop);
      return;
    }
}

static void
daemon_shutdown (GVfsDaemon *daemon,
                 GMainLoop  *loop)
{
  if (g_main_loop_is_running (loop))
    g_main_loop_quit (loop);
}

int
main (int argc, char *argv[])
{
  GVfsDaemon *daemon;
  gboolean replace;
  gboolean no_fuse;
  gboolean debugging;
  gboolean show_version;
  GError *error;
  guint name_owner_id;
  GBusNameOwnerFlags flags;
  GOptionContext *context;
  gchar *socket_dir;
  const GOptionEntry options[] = {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace,  N_("Replace old daemon."), NULL },
    { "no-fuse", 0, 0, G_OPTION_ARG_NONE, &no_fuse,  N_("Don’t start fuse."), NULL },
    { "debug", 'd', 0, G_OPTION_ARG_NONE, &debugging,  N_("Enable debug output."), NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, N_("Show program version."), NULL},
    { NULL }
  };

  setlocale (LC_ALL, "");

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  gvfs_setup_debug_handler ();

#ifdef SIGPIPE
  signal (SIGPIPE, SIG_IGN);
#endif
  
  g_set_application_name (_("GVFS Daemon"));
  context = g_option_context_new ("");

  g_option_context_set_summary (context, _("Main daemon for GVFS"));
  
  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);

  replace = FALSE;
  no_fuse = FALSE;
  debugging = FALSE;
  show_version = FALSE;

  if (g_getenv ("GVFS_DISABLE_FUSE") != NULL)
    no_fuse = TRUE;
  
  error = NULL;
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      /* Translators: the first %s is the application name, */
      /* the second %s is the error message                 */
      g_printerr (_("%s: %s"), g_get_application_name(), error->message);
      g_printerr ("\n");
      g_printerr (_("Try “%s --help” for more information."),
                  g_get_prgname ());
      g_printerr ("\n");
      g_error_free (error);
      g_option_context_free (context);
      return 1;
    }

  g_option_context_free (context);

  if (g_getenv ("GVFS_DEBUG"))
    debugging = TRUE;

  gvfs_set_debug (debugging);

  if (show_version)
    {
      g_print (PACKAGE_STRING "\n");
      return 0;
    }

  loop = g_main_loop_new (NULL, FALSE);

  daemon = g_vfs_daemon_new (TRUE, replace);
  if (daemon == NULL)
    return 1;

  /* This is needed for gvfsd-admin to ensure correct ownership. */
  socket_dir = gvfs_get_socket_dir ();
  g_mkdir (socket_dir, 0700);
  g_free (socket_dir);

  g_signal_connect (daemon, "shutdown",
		    G_CALLBACK (daemon_shutdown), loop);

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  if (replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  name_owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                  G_VFS_DBUS_DAEMON_NAME,
                                  flags,
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  GUINT_TO_POINTER (no_fuse),
                                  NULL);

  g_main_loop_run (loop);

  mount_finalize ();

  g_clear_object (&daemon);
  if (name_owner_id != 0)
    g_bus_unown_name (name_owner_id);
  if (loop != NULL)
    g_main_loop_unref (loop);
  
  return process_result;
}
