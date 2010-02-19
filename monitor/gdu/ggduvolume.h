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

#ifndef __G_GDU_VOLUME_H__
#define __G_GDU_VOLUME_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "ggduvolumemonitor.h"

G_BEGIN_DECLS

#define G_TYPE_GDU_VOLUME        (g_gdu_volume_get_type ())
#define G_GDU_VOLUME(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_GDU_VOLUME, GGduVolume))
#define G_GDU_VOLUME_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_GDU_VOLUME, GGduVolumeClass))
#define G_IS_GDU_VOLUME(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_GDU_VOLUME))
#define G_IS_GDU_VOLUME_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_GDU_VOLUME))

typedef struct _GGduVolumeClass GGduVolumeClass;

struct _GGduVolumeClass {
   GObjectClass parent_class;
};

GType g_gdu_volume_get_type (void) G_GNUC_CONST;

GGduVolume *g_gdu_volume_new            (GVolumeMonitor   *volume_monitor,
                                         GduVolume        *gdu_volume,
                                         GGduDrive        *drive,
                                         GFile            *activation_root);

GGduVolume *g_gdu_volume_new_for_unix_mount_point (GVolumeMonitor   *volume_monitor,
                                                   GUnixMountPoint  *unix_mount_point);

void        g_gdu_volume_set_mount      (GGduVolume       *volume,
                                         GGduMount        *mount);
void        g_gdu_volume_unset_mount    (GGduVolume       *volume,
                                         GGduMount        *mount);

void        g_gdu_volume_set_drive      (GGduVolume       *volume,
                                         GGduDrive        *drive);
void        g_gdu_volume_unset_drive    (GGduVolume       *volume,
                                         GGduDrive        *drive);

void        g_gdu_volume_removed        (GGduVolume       *volume);

gboolean    g_gdu_volume_has_mount_path (GGduVolume       *volume,
                                         const char       *mount_path);
gboolean    g_gdu_volume_has_uuid       (GGduVolume       *volume,
                                         const char       *uuid);
gboolean   g_gdu_volume_has_device_file (GGduVolume      *volume,
                                         const gchar     *device_file);
gboolean   g_gdu_volume_has_dev         (GGduVolume       *volume,
                                         dev_t             dev);
gboolean   g_gdu_volume_has_presentable (GGduVolume       *volume,
                                         GduPresentable   *presentable);

GduPresentable *g_gdu_volume_get_presentable (GGduVolume *volume);

GduPresentable *g_gdu_volume_get_presentable_with_cleartext (GGduVolume *volume);

GUnixMountPoint *g_gdu_volume_get_unix_mount_point (GGduVolume *volume);

G_END_DECLS

#endif /* __G_GDU_VOLUME_H__ */
