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
  const char *p = host;

  g_return_val_if_fail (host != NULL, FALSE);

  if (*p != '[')
    return FALSE;

  while (++p)
    if (!g_ascii_isxdigit (*p) && *p != ':')
      break;

  if (*p != ']' || *(p + 1) != '\0')
    return FALSE;

  return TRUE;
}

gchar *
gvfs_lookup_fstab_options_value (const gchar *fstab_options,
                                 const gchar *key)
{
  gchar *ret = NULL;

  if (fstab_options != NULL)
    {
      const gchar *start;
      guint n;

      /* The code doesn't care about prefix, which may cause problems for
       * options like "auto" and "noauto". However, this function is only used
       * with our "x-gvfs-*" options, where mentioned problems are unlikely.
       * Be careful, that some people rely on this bug and use "comment=x-gvfs-*"
       * as workaround, see: https://gitlab.gnome.org/GNOME/gvfs/issues/348
       */
      start = strstr (fstab_options, key);
      if (start != NULL)
        {
          start += strlen (key);
          for (n = 0; start[n] != ',' && start[n] != '\0'; n++)
            ;
          if (n == 0)
            ret = g_strdup ("");
          else if (n >= 1)
            ret = g_uri_unescape_segment (start, start + n, NULL);
        }
    }
  return ret;
}

GUnixMountPoint *
gvfs_get_mount_point_for_mount (GUnixMountEntry *mount_entry)
{
  GUnixMountPoint *ret = NULL;
  GList *mount_points, *l;

  mount_points = g_unix_mount_points_get (NULL);
  for (l = mount_points; l != NULL; l = l->next)
    {
      GUnixMountPoint *mount_point = l->data;
      if (g_strcmp0 (g_unix_mount_get_mount_path (mount_entry),
                     g_unix_mount_point_get_mount_path (mount_point)) == 0)
        {
          ret = mount_point;
          goto out;
        }
    }

 out:
  for (l = mount_points; l != NULL; l = l->next)
    {
      GUnixMountPoint *mount_point = l->data;
      if (G_LIKELY (mount_point != ret))
        g_unix_mount_point_free (mount_point);
    }
  g_list_free (mount_points);
  return ret;
}
