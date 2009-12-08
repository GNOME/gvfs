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

#ifndef __G_GDU_DRIVE_H__
#define __G_GDU_DRIVE_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "ggduvolumemonitor.h"

G_BEGIN_DECLS

#define G_TYPE_GDU_DRIVE        (g_gdu_drive_get_type ())
#define G_GDU_DRIVE(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_GDU_DRIVE, GGduDrive))
#define G_GDU_DRIVE_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_GDU_DRIVE, GGduDriveClass))
#define G_IS_GDU_DRIVE(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_GDU_DRIVE))
#define G_IS_GDU_DRIVE_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_GDU_DRIVE))

typedef struct _GGduDriveClass GGduDriveClass;

struct _GGduDriveClass {
   GObjectClass parent_class;
};

GType g_gdu_drive_get_type (void) G_GNUC_CONST;

GGduDrive *g_gdu_drive_new             (GVolumeMonitor *volume_monitor,
                                        GduPresentable *presentable);
void       g_gdu_drive_set_volume      (GGduDrive      *drive,
                                        GGduVolume     *volume);
void       g_gdu_drive_unset_volume    (GGduDrive      *drive,
                                        GGduVolume     *volume);
void       g_gdu_drive_disconnected    (GGduDrive      *drive);
gboolean   g_gdu_drive_has_dev         (GGduDrive      *drive,
                                        dev_t           dev);
time_t     g_gdu_drive_get_time_of_last_media_insertion (GGduDrive      *drive);

gboolean   g_gdu_drive_has_presentable (GGduDrive       *drive,
                                        GduPresentable  *presentable);

GduPresentable *g_gdu_drive_get_presentable (GGduDrive       *drive);


char *     _drive_get_icon          (GduDevice      *d);

G_END_DECLS

#endif /* __G_GDU_DRIVE_H__ */
