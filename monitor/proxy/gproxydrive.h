/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2008 Red Hat, Inc.
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

#ifndef __G_PROXY_DRIVE_H__
#define __G_PROXY_DRIVE_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "gproxyvolumemonitor.h"

G_BEGIN_DECLS

#define G_TYPE_PROXY_DRIVE        (g_proxy_drive_get_type ())
#define G_PROXY_DRIVE(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_PROXY_DRIVE, GProxyDrive))
#define G_PROXY_DRIVE_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_PROXY_DRIVE, GProxyDriveClass))
#define G_IS_PROXY_DRIVE(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_PROXY_DRIVE))
#define G_IS_PROXY_DRIVE_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_PROXY_DRIVE))

typedef struct _GProxyDriveClass GProxyDriveClass;

struct _GProxyDriveClass {
   GObjectClass parent_class;
};

GType         g_proxy_drive_get_type     (void) G_GNUC_CONST;
void          g_proxy_drive_register     (GIOModule           *module);
GProxyDrive  *g_proxy_drive_new          (GProxyVolumeMonitor *volume_monitor);
void          g_proxy_drive_update       (GProxyDrive         *drive,
                                          GVariant            *iter);
const char   *g_proxy_drive_get_id       (GProxyDrive         *drive);

void          g_proxy_drive_handle_start_op_ask_password (GProxyDrive  *drive,
                                                          GVariant     *iter);

void          g_proxy_drive_handle_start_op_ask_question (GProxyDrive  *drive,
                                                          GVariant     *iter);

void          g_proxy_drive_handle_start_op_aborted      (GProxyDrive  *drive,
                                                          GVariant     *iter);

G_END_DECLS

#endif /* __G_PROXY_DRIVE_H__ */
