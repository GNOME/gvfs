/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2014 Red Hat, Inc.
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
 * Author: Debarshi Ray <debarshir@gnome.org>
 */

#ifndef __G_VFS_BACKEND_GOOGLE_H__
#define __G_VFS_BACKEND_GOOGLE_H__

#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_GOOGLE (g_vfs_backend_google_get_type())

#define G_VFS_BACKEND_GOOGLE(o) \
  (G_TYPE_CHECK_INSTANCE_CAST((o), \
   G_VFS_TYPE_BACKEND_GOOGLE, GVfsBackendGoogle))

#define G_VFS_BACKEND_GOOGLE_CLASS(k) \
  (G_TYPE_CHECK_CLASS_CAST((k), \
   G_VFS_TYPE_BACKEND_GOOGLE, GVfsBackendGoogleClass))

#define G_VFS_IS_BACKEND_GOOGLE(o) \
  (G_TYPE_CHECK_INSTANCE_TYPE((o), \
   G_VFS_TYPE_BACKEND_GOOGLE))

#define G_VFS_IS_BACKEND_GOOGLE_CLASS(k) \
  (G_TYPE_CHECK_CLASS_TYPE((k), \
   G_VFS_TYPE_BACKEND_GOOGLE))

#define G_VFS_BACKEND_GOOGLE_GET_CLASS(o) \
  (G_TYPE_INSTANCE_GET_CLASS((o), \
   G_VFS_TYPE_BACKEND_GOOGLE, GVfsBackendGoogleClass))

typedef struct _GVfsBackendGoogle GVfsBackendGoogle;
typedef struct _GVfsBackendGoogleClass GVfsBackendGoogleClass;

GType g_vfs_backend_google_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif /* __G_VFS_BACKEND_GOOGLE_H__ */
