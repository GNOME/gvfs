/* gvfswsddservice.h
 *
 * Copyright (C) 2023 Red Hat, Inc.
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
 * You should have received a copy of the GNU Library General Public
 * License along with the Gnome Library; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Authors: Ondrej Holy <oholy@redhat.com>
 */

#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_WSDD_SERVICE (g_vfs_wsdd_service_get_type ())
G_DECLARE_FINAL_TYPE (GVfsWsddService, g_vfs_wsdd_service, G_VFS, WSDD_SERVICE, GObject)

void
g_vfs_wsdd_service_new (GCancellable *cancellable,
                        GAsyncReadyCallback callback,
                        gpointer user_data);

GVfsWsddService *
g_vfs_wsdd_service_new_finish (GAsyncResult *result,
                               GError **error);

GList *
g_vfs_wsdd_service_get_devices (GVfsWsddService *service,
                                GError **error);

G_END_DECLS
