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
#include <dbus/dbus.h>
#include "gvfsdaemon.h"
#include "gvfsbackendtest.h"
#include <gvfsdaemonprotocol.h>
#include "mount.h"

int
main (int argc, char *argv[])
{
  GMainLoop *loop;
  GVfsDaemon *daemon;
  gboolean replace;
  GError *error;
  GOptionContext *context;
  const GOptionEntry options[] = {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace,  N_("Replace old daemon."), NULL },
    { NULL }
  };

  g_thread_init (NULL);
  
  g_set_application_name (_("GVFS Daemon"));
  context = g_option_context_new ("");

  g_option_context_set_summary (context, "Main daemon for GVFS");
  
  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);

  replace = FALSE;
  error = NULL;
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_print ("%s, use --help for usage\n", error->message);
      g_error_free (error);
      return 1;
    }

  dbus_threads_init_default ();
  
  g_type_init ();

  daemon = g_vfs_daemon_new (TRUE, replace);
  if (daemon == NULL)
    return 1;

  mount_init ();
  
  loop = g_main_loop_new (NULL, FALSE);

  g_print ("Entering mainloop\n");
  g_main_loop_run (loop);
  
  return 0;
}
