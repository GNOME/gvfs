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

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gmodule.h>
#include <gio/gio.h>

#include <gvfsproxyvolumemonitordaemon.h>

#include "ggduvolumemonitor.h"

int
main (int argc, char *argv[])
{
  g_vfs_proxy_volume_monitor_daemon_init ();

  g_set_application_name (_("GVfs GDU Volume Monitor"));

  return g_vfs_proxy_volume_monitor_daemon_main (argc,
                                                 argv,
                                                 "org.gtk.Private.GduVolumeMonitor",
                                                 G_TYPE_GDU_VOLUME_MONITOR);
}
