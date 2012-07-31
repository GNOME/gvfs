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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#ifndef __G_MOUNT_OPERATION_DBUS_H__
#define __G_MOUNT_OPERATION_DBUS_H__

#include <sys/stat.h>

#include <glib-object.h>
#include <gio/gio.h>
#include <gmountspec.h>
#include <gmountsource.h>

G_BEGIN_DECLS

GMountSource *g_mount_operation_dbus_wrap (GMountOperation *op,
                                           GDBusConnection *connection);

G_END_DECLS

#endif /* __G_MOUNT_OPERATION_DBUS_H__ */
