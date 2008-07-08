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

#ifndef __G_HAL_DRIVE_H__
#define __G_HAL_DRIVE_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "hal-pool.h"
#include "ghalvolumemonitor.h"

G_BEGIN_DECLS

#define G_TYPE_HAL_DRIVE        (g_hal_drive_get_type ())
#define G_HAL_DRIVE(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_HAL_DRIVE, GHalDrive))
#define G_HAL_DRIVE_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_HAL_DRIVE, GHalDriveClass))
#define G_IS_HAL_DRIVE(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_HAL_DRIVE))
#define G_IS_HAL_DRIVE_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_HAL_DRIVE))

typedef struct _GHalDriveClass GHalDriveClass;

struct _GHalDriveClass {
   GObjectClass parent_class;
};

GType g_hal_drive_get_type (void) G_GNUC_CONST;

GHalDrive *g_hal_drive_new          (GVolumeMonitor *volume_monitor,
				     HalDevice      *device,
				     HalPool        *pool);
gboolean   g_hal_drive_has_udi      (GHalDrive      *drive,
				     const char     *udi);
void       g_hal_drive_set_volume   (GHalDrive      *drive,
				     GHalVolume     *volume);
void       g_hal_drive_unset_volume (GHalDrive      *drive,
				     GHalVolume     *volume);
void       g_hal_drive_disconnected (GHalDrive      *drive);
char *     _drive_get_icon          (HalDevice      *d);

G_END_DECLS

#endif /* __G_HAL_DRIVE_H__ */
