/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

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
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <glib.h>
#include <gmodule.h>
#include <gio/gio.h>

#include "ghalvolumemonitor.h"
#include "ghalvolume.h"
#include "ghalmount.h"
#include "ghaldrive.h"
#include "hal-pool.h"
#include "hal-device.h"

void
g_io_module_load (GIOModule *module)
{
  hal_device_register (module);
  hal_pool_register (module);
  g_hal_drive_register (module);
  g_hal_mount_register (module);
  g_hal_volume_register (module);
  g_hal_volume_monitor_register (module);
 }

void
g_io_module_unload (GIOModule *module)
{
}
