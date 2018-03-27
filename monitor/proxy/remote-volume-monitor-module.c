/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2008 Red Hat, Inc.
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
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "gproxyvolumemonitor.h"
#include "gproxyvolume.h"
#include "gproxymount.h"
#include "gproxyshadowmount.h"
#include "gproxydrive.h"

void
g_io_module_load (GIOModule *module)
{
  /* see gvfsproxyvolumemonitor.c:g_vfs_proxy_volume_monitor_daemon_init() */
  if (g_getenv ("GVFS_REMOTE_VOLUME_MONITOR_IGNORE") != NULL)
    goto out;

  /* We make this module resident since we *may* hold on to an instance
   * of the union monitor in the static method get_mount_for_mount_path()
   * on GNativeVolumeMonitor. And it doesn't make much sense to unload
   * the module *anyway*.
   *
   * See the comment gproxyvolumemonitor.c:get_mount_for_mount_path().
   */
  g_type_module_use (G_TYPE_MODULE (module));

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");

  g_proxy_drive_register (module);
  g_proxy_mount_register (module);
  g_proxy_shadow_mount_register (module);
  g_proxy_volume_register (module);
  g_proxy_volume_monitor_register (module);
out:
  ;
 }

void
g_io_module_unload (GIOModule *module)
{
  if (g_getenv ("GVFS_REMOTE_VOLUME_MONITOR_IGNORE") != NULL)
    goto out;

  g_proxy_volume_monitor_unload_cleanup ();

out:
  ;
}

char **
g_io_module_query (void)
{
  char *eps[] = {
    G_NATIVE_VOLUME_MONITOR_EXTENSION_POINT_NAME,
    G_VOLUME_MONITOR_EXTENSION_POINT_NAME,
    NULL
  };
  return g_strdupv (eps);
}
