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

#ifndef __G_HAL_MOUNT_H__
#define __G_HAL_MOUNT_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "hal-pool.h"
#include "ghalvolumemonitor.h"

G_BEGIN_DECLS

#define G_TYPE_HAL_MOUNT        (g_hal_mount_get_type ())
#define G_HAL_MOUNT(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_HAL_MOUNT, GHalMount))
#define G_HAL_MOUNT_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_HAL_MOUNT, GHalMountClass))
#define G_IS_HAL_MOUNT(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_HAL_MOUNT))
#define G_IS_HAL_MOUNT_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_HAL_MOUNT))

typedef struct _GHalMountClass GHalMountClass;

struct _GHalMountClass {
   GObjectClass parent_class;
};

GType g_hal_mount_get_type (void) G_GNUC_CONST;

GHalMount *  g_hal_mount_new_for_hal_device    (GVolumeMonitor    *volume_monitor,
						HalDevice         *device,
						GFile             *override_root,
						const char        *override_name,
						GIcon             *override_icon,
						gboolean           cannot_unmount,
						HalPool           *pool,
						GHalVolume        *volume);
GHalMount *  g_hal_mount_new                   (GVolumeMonitor    *volume_monitor,
						GUnixMountEntry   *mount_entry,
						HalPool           *pool,
						GHalVolume        *volume);
gboolean     g_hal_mount_has_mount_path        (GHalMount         *mount,
						const char        *mount_path);
gboolean     g_hal_mount_has_udi               (GHalMount         *mount,
						const char        *udi);
gboolean     g_hal_mount_has_uuid              (GHalMount         *mount,
						const char        *uuid);
void         g_hal_mount_unset_volume          (GHalMount         *mount,
						GHalVolume        *volume);
void         g_hal_mount_unmounted             (GHalMount         *mount);
void         g_hal_mount_override_name         (GHalMount         *mount,
						const char        *name);
void         g_hal_mount_override_icon         (GHalMount         *mount,
						GIcon             *icon);

G_END_DECLS

#endif /* __G_HAL_MOUNT_H__ */
