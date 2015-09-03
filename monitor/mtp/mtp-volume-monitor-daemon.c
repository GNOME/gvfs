/* GIO - GLib Input, Output and Streaming Library
 *   Volume Monitor for MTP Backend
 *
 * Copyright (C) 2012 Philip Langdale <philipl@overt.org>
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

#include <gvfsproxyvolumemonitordaemon.h>

#include "gmtpvolumemonitor.h"

int
main (int argc, char *argv[])
{
  g_vfs_proxy_volume_monitor_daemon_init ();
  return g_vfs_proxy_volume_monitor_daemon_main (argc,
                                                 argv,
                                                 "org.gtk.vfs.MTPVolumeMonitor",
                                                 G_TYPE_MTP_VOLUME_MONITOR);
}
