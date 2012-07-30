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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
 */

#ifndef _GVFSBACKENDAFP_H_
#define _GVFSBACKENDAFP_H_

#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_AFP             (g_vfs_backend_afp_get_type ())
#define G_VFS_BACKEND_AFP(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_VFS_TYPE_BACKEND_AFP, GVfsBackendAfp))
#define G_VFS_BACKEND_AFP_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), G_VFS_TYPE_BACKEND_AFP, GVfsBackendAfpClass))
#define G_IS_VFS_BACKEND_AFP(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_VFS_TYPE_BACKEND_AFP))
#define G_IS_VFS_BACKEND_AFP_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), G_VFS_TYPE_BACKEND_AFP))
#define G_VFS_BACKEND_AFP_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), G_VFS_TYPE_BACKEND_AFP, GVfsBackendAfpClass))

typedef struct _GVfsBackendAfpClass GVfsBackendAfpClass;
typedef struct _GVfsBackendAfp GVfsBackendAfp;

GType g_vfs_backend_afp_get_type (void) G_GNUC_CONST;

#define BACKEND_SETUP_FUNC g_vfs_afp_daemon_init
void g_vfs_afp_daemon_init (void);

G_END_DECLS

#endif /* _GVFSBACKENDAFP_H_ */
