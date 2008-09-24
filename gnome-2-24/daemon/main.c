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

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <dbus/dbus.h>
#include "gvfsdaemon.h"
#include "gvfsbackendtest.h"
#include <gvfsdaemonprotocol.h>
#include "mount.h"
#include <locale.h>

int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  GVfsDaemon *daemon;
  gboolean replace;
  gboolean no_fuse;
  GError *error;
  GOptionContext *context;
  const GOptionEntry options[] = {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace,  N_("Replace old daemon."), NULL },
    { "no-fuse", 0, 0, G_OPTION_ARG_NONE, &no_fuse,  N_("Don't start fuse."), NULL },
    { NULL }
  };

  setlocale (LC_ALL, "");

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
  
  g_thread_init (NULL);
  
  g_set_application_name (_("GVFS Daemon"));
  context = g_option_context_new ("");

  g_option_context_set_summary (context, _("Main daemon for GVFS"));
  
  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);

  replace = FALSE;
  no_fuse = FALSE;

  if (g_getenv ("GVFS_DISABLE_FUSE") != NULL)
    no_fuse = TRUE;
  
  error = NULL;
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      /* Translators: the first %s is the application name, */
      /* the second %s is the error message                 */
      g_printerr (_("%s: %s"), g_get_application_name(), error->message);
      g_printerr ("\n");
      g_printerr (_("Try \"%s --help\" for more information."),
                  g_get_prgname ());
      g_printerr ("\n");
      g_error_free (error);
      g_option_context_free (context);
      return 1;
    }

  g_option_context_free (context);

  dbus_threads_init_default ();
  
  g_type_init ();

  daemon = g_vfs_daemon_new (TRUE, replace);
  if (daemon == NULL)
    return 1;

  mount_init ();
  
  loop = g_main_loop_new (NULL, FALSE);


#ifdef HAVE_FUSE
  if (!no_fuse)
    {
      char *fuse_path;
      char *argv2[3];
      
      fuse_path = g_build_filename (g_get_home_dir (), ".gvfs", NULL);
      
      if (!g_file_test (fuse_path, G_FILE_TEST_EXISTS))
	g_mkdir (fuse_path, 0700);
      
      argv2[0] = LIBEXEC_DIR "/gvfs-fuse-daemon";
      argv2[1] = fuse_path;
      argv2[2] = NULL;
      
      g_spawn_async (NULL,
		     argv2,
		     NULL,
		     G_SPAWN_STDOUT_TO_DEV_NULL |
		     G_SPAWN_STDERR_TO_DEV_NULL, 
		     NULL, NULL,
		     NULL, NULL);
      
      g_free (fuse_path);
    }
#endif
  
  g_main_loop_run (loop);
  
  return 0;
}
