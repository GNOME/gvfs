/*
 * gvfs/monitor/afc/afc-volume-monitor-daemon.c
 *
 * Copyright (c) 2008-2009 Patrick Walton <pcwalton@ucla.edu>
 * Copyright (c) 2009 Martin Szulecki <opensuse@sukimashita.com>
 */

#include <config.h>

#include <gvfsproxyvolumemonitordaemon.h>

#include "afcvolumemonitor.h"

int
main (int argc, char *argv[])
{
  g_vfs_proxy_volume_monitor_daemon_init ();
  return g_vfs_proxy_volume_monitor_daemon_main (argc,
                                                 argv,
                                                 "org.gtk.vfs.AfcVolumeMonitor",
                                                 G_VFS_TYPE_AFC_VOLUME_MONITOR);
}

/*
 * vim: sw=2 ts=8 cindent expandtab cinoptions=f0,>4,n2,{2,(0,^-2,t0 ai
 */
