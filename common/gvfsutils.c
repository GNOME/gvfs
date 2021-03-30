/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2014 Ross Lagerwall
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
 * Public License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include "gvfsutils.h"

#ifdef G_OS_UNIX
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* Indicates whether debug output is enabled. */
static gboolean debugging = FALSE;

/**
 * gvfs_randomize_string:
 * @str: the string to randomize
 * @len: the length of the string
 *
 * Takes a string and fills it with @len random chars.
 **/
void
gvfs_randomize_string (char *str,
                       int len)
{
  int i;
  const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

  for (i = 0; i < len; i++)
    str[i] = chars[g_random_int_range (0, strlen(chars))];
}

/**
 * gvfs_have_session_bus:
 *
 * Returns: %TRUE if we can connect to a session or user bus without
 *  triggering X11 autolaunching.
 */
gboolean
gvfs_have_session_bus (void)
{
  if (g_getenv ("DBUS_SESSION_BUS_ADDRESS") != NULL)
    return TRUE;

#ifdef G_OS_UNIX
    {
      gboolean ret = FALSE;
      gchar *bus;
      GStatBuf buf;

      bus = g_build_filename (g_get_user_runtime_dir (), "bus", NULL);

      if (g_stat (bus, &buf) < 0)
        goto out;

      if (buf.st_uid != geteuid ())
        goto out;

      if ((buf.st_mode & S_IFMT) != S_IFSOCK)
        goto out;

      ret = TRUE;
out:
      g_free (bus);
      return ret;
    }
#else
  return FALSE;
#endif
}

gboolean
gvfs_get_debug (void)
{
  return debugging;
}

void
gvfs_set_debug (gboolean debugging_)
{
  debugging = debugging_;
}

static void
toggle_debugging (int signum)
{
  debugging = !debugging;
}

/**
 * gvfs_setup_debugging_handler:
 *
 * Sets up a handler for SIGUSR2 that toggles the debugging flag when the
 * signal is received.
 **/
void
gvfs_setup_debug_handler (void)
{
  struct sigaction sa;

  sigemptyset (&sa.sa_mask);
  sa.sa_handler = toggle_debugging;
  sa.sa_flags = 0;
  sigaction (SIGUSR2, &sa, NULL);
}

gboolean
gvfs_is_ipv6 (const char *host)
{
  g_return_val_if_fail (host != NULL, FALSE);

  if (*host != '[' || host[strlen (host) - 1] != ']')
    return FALSE;

  return TRUE;
}

gchar *
gvfs_get_socket_dir (void)
{
  return g_build_filename (g_get_user_runtime_dir (), "gvfsd", NULL);
}
