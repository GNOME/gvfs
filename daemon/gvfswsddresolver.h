/* gvfswsddresolver.h
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
#include <gobject/gobject.h>

#include "gvfswsdddevice.h"

G_BEGIN_DECLS

#define G_VFS_TYPE_WSDD_RESOLVER (g_vfs_wsdd_resolver_get_type ())
G_DECLARE_FINAL_TYPE (GVfsWsddResolver, g_vfs_wsdd_resolver, G_VFS, WSDD_RESOLVER, GObject)

GVfsWsddResolver *
g_vfs_wsdd_resolver_new (void);

void
g_vfs_wsdd_resolver_resolve (GVfsWsddResolver *resolver,
                             GVfsWsddDevice *device);

gchar *
g_vfs_wsdd_resolver_get_address (GVfsWsddResolver *resolver,
                                 GVfsWsddDevice *device);

G_END_DECLS
