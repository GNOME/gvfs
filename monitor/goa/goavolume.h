/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2012 Red Hat, Inc.
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

#ifndef __GOA_VOLUME_H__
#define __GOA_VOLUME_H__

#include <glib-object.h>
#include <gio/gio.h>

#define GOA_API_IS_SUBJECT_TO_CHANGE
#include <goa/goa.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_GOA_VOLUME (g_vfs_goa_volume_get_type())

#define G_VFS_GOA_VOLUME(o) \
  (G_TYPE_CHECK_INSTANCE_CAST((o), \
   G_VFS_TYPE_GOA_VOLUME, GVfsGoaVolume))

#define G_VFS_GOA_VOLUME_CLASS(k) \
  (G_TYPE_CHECK_CLASS_CAST((k), \
   G_VFS_TYPE_GOA_VOLUME, GVfsGoaVolumeClass))

#define G_VFS_IS_GOA_VOLUME(o) \
  (G_TYPE_CHECK_INSTANCE_TYPE((o), \
   G_VFS_TYPE_GOA_VOLUME))

#define G_VFS_IS_GOA_VOLUME_CLASS(k) \
  (G_TYPE_CHECK_CLASS_TYPE((k), \
   G_VFS_TYPE_GOA_VOLUME))

#define G_VFS_GOA_VOLUME_GET_CLASS(o) \
  (G_TYPE_INSTANCE_GET_CLASS((o), \
   G_VFS_TYPE_GOA_VOLUME, GVfsGoaVolumeClass))

typedef struct _GVfsGoaVolume GVfsGoaVolume;
typedef struct _GVfsGoaVolumeClass GVfsGoaVolumeClass;

GType g_vfs_goa_volume_get_type (void) G_GNUC_CONST;

GVolume *g_vfs_goa_volume_new (GoaObject *object, const gchar *uuid);

G_END_DECLS

#endif /* __GOA_VOLUME_H__ */
