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

#ifndef __G_GDU_MOUNT_H__
#define __G_GDU_MOUNT_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "ggduvolumemonitor.h"

G_BEGIN_DECLS

#define G_TYPE_GDU_MOUNT        (g_gdu_mount_get_type ())
#define G_GDU_MOUNT(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_GDU_MOUNT, GGduMount))
#define G_GDU_MOUNT_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_GDU_MOUNT, GGduMountClass))
#define G_IS_GDU_MOUNT(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_GDU_MOUNT))
#define G_IS_GDU_MOUNT_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_GDU_MOUNT))

typedef struct _GGduMountClass GGduMountClass;

struct _GGduMountClass {
   GObjectClass parent_class;
};

GType g_gdu_mount_get_type (void) G_GNUC_CONST;

GGduMount *  g_gdu_mount_new                   (GVolumeMonitor    *volume_monitor,
                                                GUnixMountEntry   *mount_entry,
                                                GGduVolume        *volume);
gboolean     g_gdu_mount_has_mount_path        (GGduMount         *mount,
                                                const gchar       *mount_path);
gboolean     g_gdu_mount_has_uuid              (GGduMount         *mount,
                                                const gchar       *uuid);
void         g_gdu_mount_unset_volume          (GGduMount         *mount,
                                                GGduVolume        *volume);
void         g_gdu_mount_unmounted             (GGduMount         *mount);

gboolean     g_gdu_mount_has_volume            (GGduMount         *mount,
                                                GGduVolume        *volume);

G_END_DECLS

#endif /* __G_GDU_MOUNT_H__ */
