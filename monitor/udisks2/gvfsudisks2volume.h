/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2012 Red Hat, Inc.
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

#ifndef __GVFS_UDISKS2_VOLUME_H__
#define __GVFS_UDISKS2_VOLUME_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "gvfsudisks2volumemonitor.h"

G_BEGIN_DECLS

#define GVFS_TYPE_UDISKS2_VOLUME  (gvfs_udisks2_volume_get_type ())
#define GVFS_UDISKS2_VOLUME(o)    (G_TYPE_CHECK_INSTANCE_CAST ((o), GVFS_TYPE_UDISKS2_VOLUME, GVfsUDisks2Volume))
#define GVFS_IS_UDISKS2_VOLUME(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), GVFS_TYPE_UDISKS2_VOLUME))

GType              gvfs_udisks2_volume_get_type    (void) G_GNUC_CONST;

GVfsUDisks2Volume *gvfs_udisks2_volume_new         (GVfsUDisks2VolumeMonitor   *monitor,
                                                    UDisksBlock                *block,
                                                    GUnixMountPoint            *mount_point,
                                                    GVfsUDisks2Drive           *drive,
                                                    GFile                      *activation_root,
                                                    gboolean                    coldplug);
void               gvfs_udisks2_volume_removed     (GVfsUDisks2Volume          *volume);

UDisksBlock       *gvfs_udisks2_volume_get_block       (GVfsUDisks2Volume      *volume);
GUnixMountPoint   *gvfs_udisks2_volume_get_mount_point (GVfsUDisks2Volume      *volume);
dev_t              gvfs_udisks2_volume_get_dev         (GVfsUDisks2Volume      *volume);

void               gvfs_udisks2_volume_set_mount   (GVfsUDisks2Volume          *volume,
                                                    GVfsUDisks2Mount           *mount);
void               gvfs_udisks2_volume_unset_mount (GVfsUDisks2Volume          *volume,
                                                    GVfsUDisks2Mount           *mount);

void               gvfs_udisks2_volume_set_drive   (GVfsUDisks2Volume          *volume,
                                                    GVfsUDisks2Drive           *drive);
void               gvfs_udisks2_volume_unset_drive (GVfsUDisks2Volume          *volume,
                                                    GVfsUDisks2Drive           *drive);

gboolean           gvfs_udisks2_volume_has_uuid    (GVfsUDisks2Volume          *volume,
                                                    const gchar                *uuid);

G_END_DECLS

#endif /* __GVFS_UDISKS2_VOLUME_H__ */
