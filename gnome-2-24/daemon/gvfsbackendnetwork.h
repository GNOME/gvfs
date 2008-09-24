/* gvfs-backend-network.h - GIO - GLib Input, Output and Streaming Library
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
 * write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Andrew Walton <awalton@svn.gnome.org>
 */

#ifndef __G_VFS_BACKEND_NETWORK_H__
#define __G_VFS_BACKEND_NETWORK_H__

#include <gvfsbackend.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_NETWORK         (g_vfs_backend_network_get_type ())
#define G_VFS_BACKEND_NETWORK(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND_NETWORK, GVfsBackendNetwork))
#define G_VFS_BACKEND_NETWORK_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_NETWORK, GVfsBackendNetworkClass))
#define G_VFS_IS_BACKEND_NETWORK(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND_NETWORK))
#define G_VFS_IS_BACKEND_NETWORK_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND_NETWORK))
#define G_VFS_BACKEND_NETWORK_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND_NETWORK, GVfsBackendNetworkClass))

typedef struct _GVfsBackendNetworkClass   GVfsBackendNetworkClass;

struct _GVfsBackendNetworkClass
{
  GVfsBackendClass parent_class;
};

GType g_vfs_backend_network_get_type (void) G_GNUC_CONST;

#define BACKEND_SETUP_FUNC g_vfs_network_daemon_init 
void g_vfs_network_daemon_init (void);


G_END_DECLS

#endif /* __G_VFS_BACKEND_NETWORK_H__ */
