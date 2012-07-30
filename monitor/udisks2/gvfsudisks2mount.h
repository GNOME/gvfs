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

#ifndef __GVFS_UDISKS2_MOUNT_H__
#define __GVFS_UDISKS2_MOUNT_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "gvfsudisks2volumemonitor.h"

G_BEGIN_DECLS

#define GVFS_TYPE_UDISKS2_MOUNT  (gvfs_udisks2_mount_get_type ())
#define GVFS_UDISKS2_MOUNT(o)    (G_TYPE_CHECK_INSTANCE_CAST ((o), GVFS_TYPE_UDISKS2_MOUNT, GVfsUDisks2Mount))
#define GVFS_IS_UDISKS2_MOUNT(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), GVFS_TYPE_UDISKS2_MOUNT))


GType             gvfs_udisks2_mount_get_type       (void) G_GNUC_CONST;
GVfsUDisks2Mount *gvfs_udisks2_mount_new            (GVfsUDisks2VolumeMonitor *monitor,
                                                     GUnixMountEntry          *mount_entry,
                                                     GVfsUDisks2Volume        *volume);
void              gvfs_udisks2_mount_unmounted      (GVfsUDisks2Mount         *mount);

gboolean          gvfs_udisks2_mount_has_uuid       (GVfsUDisks2Mount         *mount,
                                                     const gchar              *uuid);

void              gvfs_udisks2_mount_set_volume     (GVfsUDisks2Mount         *mount,
                                                     GVfsUDisks2Volume        *volume);
void              gvfs_udisks2_mount_unset_volume   (GVfsUDisks2Mount         *mount,
                                                     GVfsUDisks2Volume        *volume);
gboolean          gvfs_udisks2_mount_has_volume     (GVfsUDisks2Mount         *mount,
                                                     GVfsUDisks2Volume        *volume);
GVfsUDisks2Volume *gvfs_udisks2_mount_get_volume    (GVfsUDisks2Mount         *mount);

const gchar      *gvfs_udisks2_mount_get_mount_path  (GVfsUDisks2Mount        *mount);
GUnixMountEntry  *gvfs_udisks2_mount_get_mount_entry (GVfsUDisks2Mount        *mount);

G_END_DECLS

#endif /* __GVFS_UDISKS2_MOUNT_H__ */
