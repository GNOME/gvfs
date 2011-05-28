 /* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) Carl-Anton Ingmarsson 2011 <ca.ingmarsson@gmail.com>
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
 * Author: Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
 */

#ifndef _GVFSAFPCONNECTION_H_
#define _GVFSAFPCONNECTION_H_

#include <gio/gio.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_AFP_CONNECTION             (g_vfs_afp_connection_get_type ())
#define G_VFS_AFP_CONNECTION(obj)             (G_VFS_TYPE_CHECK_INSTANCE_CAST ((obj), G_VFS_TYPE_VFS_AFP_CONNECTION, GVfsAfpConnection))
#define G_VFS_AFP_CONNECTION_CLASS(klass)     (G_VFS_TYPE_CHECK_CLASS_CAST ((klass), G_VFS_TYPE_VFS_AFP_CONNECTION, GVfsAfpConnectionClass))
#define G_IS_VFS_AFP_CONNECTION(obj)          (G_VFS_TYPE_CHECK_INSTANCE_TYPE ((obj), G_VFS_TYPE_VFS_AFP_CONNECTION))
#define G_IS_VFS_AFP_CONNECTION_CLASS(klass)  (G_VFS_TYPE_CHECK_CLASS_TYPE ((klass), G_VFS_TYPE_VFS_AFP_CONNECTION))
#define G_VFS_AFP_CONNECTION_GET_CLASS(obj)   (G_VFS_TYPE_INSTANCE_GET_CLASS ((obj), G_VFS_TYPE_VFS_AFP_CONNECTION, GVfsAfpConnectionClass))

typedef struct _GVfsAfpConnectionClass GVfsAfpConnectionClass;
typedef struct _GVfsAfpConnection GVfsAfpConnection;
typedef struct _GVfsAfpConnectionPrivate GVfsAfpConnectionPrivate;

struct _GVfsAfpConnectionClass
{
	GObjectClass parent_class;
};

struct _GVfsAfpConnection
{
	GObject parent_instance;

  GVfsAfpConnectionPrivate *priv;
};

GType g_vfs_afp_connection_get_type (void) G_GNUC_CONST;

GVfsAfpConnection *
g_vfs_afp_connection_new (GSocketConnectable *addr,
                          GCancellable       *cancellable,
                          GError             **error);
G_END_DECLS

#endif /* _GVFSAFPCONNECTION_H_ */
