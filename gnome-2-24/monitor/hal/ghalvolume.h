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

#ifndef __G_HAL_VOLUME_H__
#define __G_HAL_VOLUME_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "hal-pool.h"
#include "ghalvolumemonitor.h"

G_BEGIN_DECLS

#define G_TYPE_HAL_VOLUME        (g_hal_volume_get_type ())
#define G_HAL_VOLUME(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_HAL_VOLUME, GHalVolume))
#define G_HAL_VOLUME_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_HAL_VOLUME, GHalVolumeClass))
#define G_IS_HAL_VOLUME(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_HAL_VOLUME))
#define G_IS_HAL_VOLUME_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_HAL_VOLUME))

typedef struct _GHalVolumeClass GHalVolumeClass;

struct _GHalVolumeClass {
   GObjectClass parent_class;
};

GType g_hal_volume_get_type (void) G_GNUC_CONST;

GHalVolume *g_hal_volume_new            (GVolumeMonitor   *volume_monitor,
					 HalDevice        *device,
					 HalPool          *pool,
                                         GFile            *foreign_mount_root,
                                         gboolean          is_mountable,
					 GHalDrive        *drive);

gboolean    g_hal_volume_has_mount_path (GHalVolume       *volume,
					 const char       *mount_path);
gboolean    g_hal_volume_has_device_path (GHalVolume       *volume,
                                          const char       *device_path);
gboolean    g_hal_volume_has_udi        (GHalVolume       *volume,
					 const char       *udi);
gboolean    g_hal_volume_has_uuid       (GHalVolume       *volume,
					 const char       *uuid);

gboolean    g_hal_volume_has_foreign_mount_root (GHalVolume       *volume,
                                                 GFile            *mount_root);

void        g_hal_volume_adopt_foreign_mount (GHalVolume *volume, 
                                              GMount *foreign_mount);

void        g_hal_volume_set_mount      (GHalVolume       *volume,
					 GHalMount        *mount);
void        g_hal_volume_unset_mount    (GHalVolume       *volume,
					 GHalMount        *mount);

void        g_hal_volume_set_drive      (GHalVolume       *volume,
					 GHalDrive        *drive);
void        g_hal_volume_unset_drive    (GHalVolume       *volume,
					 GHalDrive        *drive);

void        g_hal_volume_removed        (GHalVolume       *volume);

G_END_DECLS

#endif /* __G_HAL_VOLUME_H__ */
