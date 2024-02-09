/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2019 Vilém Hořínek
 * Copyright (C) 2022-2023 Jan-Michael Brummer <jan-michael.brummer1@volkswagen.de>
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
 * Author: Jan-Michael Brummer <jan-michael.brummer1@volkswagen.de>
 */

#ifndef __G_VFS_BACKEND_ONEDRIVE_H__
#define __G_VFS_BACKEND_ONEDRIVE_H__

#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_ONEDRIVE (g_vfs_backend_onedrive_get_type ())
G_DECLARE_FINAL_TYPE (GVfsBackendOnedrive, g_vfs_backend_onedrive, G_VFS, BACKEND_ONEDRIVE, GVfsBackend)

G_END_DECLS

#endif // __G_VFS_BACKEND_ONEDRIVE_H__
