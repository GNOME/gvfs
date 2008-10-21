/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2008 Red Hat, Inc.
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

#ifndef __G_VFS_ICON_H__
#define __G_VFS_ICON_H__

#include <gio/gio.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_ICON         (g_vfs_icon_get_type ())
#define G_VFS_ICON(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_ICON, GVfsIcon))
#define G_VFS_ICON_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_ICON, GVfsIconClass))
#define G_VFS_IS_ICON(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_ICON))
#define G_VFS_IS_ICON_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_ICON))
#define G_VFS_ICON_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_ICON, GVfsIconClass))

/**
 * GVfsIcon:
 *
 * An object for referencing icons originating from a #GVfsBackend
 */
typedef struct _GVfsIcon        GVfsIcon;
typedef struct _GVfsIconClass   GVfsIconClass;

struct _GVfsIcon
{
  GObject parent_instance;

  GMountSpec *mount_spec;
  char *icon_id;
};

struct _GVfsIconClass
{
  GObjectClass parent_class;
};

GType  g_vfs_icon_get_type        (void) G_GNUC_CONST;

GIcon *g_vfs_icon_new             (GMountSpec  *mount_spec,
                                   const gchar *icon_id);

GMountSpec  *g_vfs_icon_get_mount_spec (GVfsIcon *vfs_icon);
const gchar *g_vfs_icon_get_icon_id    (GVfsIcon *vfs_icon);

G_END_DECLS

#endif /* __G_VFS_ICON_H__ */
