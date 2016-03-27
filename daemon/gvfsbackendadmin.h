/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2015, 2016 Cosimo Cecchi <cosimoc@gnome.org>
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
 */

#ifndef __G_VFS_BACKEND_ADMIN_H__
#define __G_VFS_BACKEND_ADMIN_H__

#include <gvfsbackend.h>

G_BEGIN_DECLS

#define BACKEND_PRE_SETUP_FUNC g_vfs_backend_admin_pre_setup
#define G_VFS_TYPE_BACKEND_ADMIN (g_vfs_backend_admin_get_type())

#define G_VFS_BACKEND_ADMIN(o) \
  (G_TYPE_CHECK_INSTANCE_CAST((o), \
   G_VFS_TYPE_BACKEND_ADMIN, GVfsBackendAdmin))

#define G_VFS_BACKEND_ADMIN_CLASS(k) \
  (G_TYPE_CHECK_CLASS_CAST((k), \
   G_VFS_TYPE_BACKEND_ADMIN, GVfsBackendAdminClass))

#define G_VFS_IS_BACKEND_ADMIN(o) \
  (G_TYPE_CHECK_INSTANCE_TYPE((o), \
   G_VFS_TYPE_BACKEND_ADMIN))

#define G_VFS_IS_BACKEND_ADMIN_CLASS(k) \
  (G_TYPE_CHECK_CLASS_TYPE((k), \
   G_VFS_TYPE_BACKEND_ADMIN))

#define G_VFS_BACKEND_ADMIN_GET_CLASS(o) \
  (G_TYPE_INSTANCE_GET_CLASS((o), \
   G_VFS_TYPE_BACKEND_ADMIN, GVfsBackendAdminClass))

typedef struct _GVfsBackendAdmin GVfsBackendAdmin;
typedef struct _GVfsBackendAdminClass GVfsBackendAdminClass;

GType g_vfs_backend_admin_get_type (void) G_GNUC_CONST;

void g_vfs_backend_admin_pre_setup (int *argc, char **argv[]);

G_END_DECLS

#endif /* __G_VFS_BACKEND_ADMIN_H__ */
