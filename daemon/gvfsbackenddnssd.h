/* gvfs-backend-dns_sd.h - GIO - GLib Input, Output and Streaming Library
 * Original work, Copyright (C) 2003 Red Hat, Inc
 * GVFS port, Copyright (c) 2008 Andrew Walton.
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
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#ifndef __G_VFS_BACKEND_DNS_SD_H__
#define __G_VFS_BACKEND_DNS_SD_H__

#include <gvfsbackend.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_DNS_SD         (g_vfs_backend_dns_sd_get_type ())
#define G_VFS_BACKEND_DNS_SD(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND_DNS_SD, GVfsBackendDnsSd))
#define G_VFS_BACKEND_DNS_SD_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_DNS_SD, GVfsBackendDnsSdClass))
#define G_VFS_IS_BACKEND_DNS_SD(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND_DNS_SD))
#define G_VFS_IS_BACKEND_DNS_SD_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND_DNS_SD))
#define G_VFS_BACKEND_DNS_SD_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND_DNS_SD, GVfsBackendDnsSdClass))

typedef struct _GVfsBackendDnsSdClass   GVfsBackendDnsSdClass;

struct _GVfsBackendDnsSdClass
{
  GVfsBackendClass parent_class;
};

GType g_vfs_backend_dns_sd_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif /* __G_VFS_BACKEND_DNS_SD_H__ */
