/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2009 Red Hat, Inc.
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
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "gvfsudisks2utils.h"

void
gvfs_udisks2_utils_udisks_error_to_gio_error (GError *error)
{
  g_return_if_fail (error != NULL);

  if (error->domain == UDISKS_ERROR)
    {
      switch (error->code)
        {
        case UDISKS_ERROR_DEVICE_BUSY:
          error->code = G_IO_ERROR_BUSY;
          break;
        case UDISKS_ERROR_NOT_AUTHORIZED_DISMISSED:
          error->code = G_IO_ERROR_FAILED_HANDLED;
          break;
        default:
          error->code = G_IO_ERROR_FAILED;
          break;
        }
    }
  else
    {
      error->code = G_IO_ERROR_FAILED;
    }

  error->domain = G_IO_ERROR;
  g_dbus_error_strip_remote_error (error);
}


GIcon *
gvfs_udisks2_utils_icon_from_fs_type (const gchar *fs_type)
{
  const gchar *icon_name;
  if (g_strcmp0 (fs_type, "nfs") == 0 ||
      g_strcmp0 (fs_type, "nfs4") == 0 ||
      g_strcmp0 (fs_type, "cifs") == 0)
    {
      icon_name = "folder-remote";
    }
  else
    {
      icon_name = "drive-removable-media";
    }
  return g_themed_icon_new_with_default_fallbacks (icon_name);
}

gchar *
gvfs_udisks2_utils_lookup_fstab_options_value (const gchar *fstab_options,
                                               const gchar *key)
{
  gchar *ret = NULL;

  if (fstab_options != NULL)
    {
      const gchar *start;
      guint n;

      start = strstr (fstab_options, key);
      if (start != NULL)
        {
          start += strlen (key);
          for (n = 0; start[n] != ',' && start[n] != '\0'; n++)
            ;
          if (n > 1)
            ret = g_uri_unescape_segment (start, start + n, NULL);
        }
    }
  return ret;
}
