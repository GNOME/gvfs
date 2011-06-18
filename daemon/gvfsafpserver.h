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

#ifndef _GVFSAFPSERVER_H_
#define _GVFSAFPSERVER_H_

#include <gio/gio.h>

#include "gmountsource.h"
#include "gvfsafpconnection.h"

G_BEGIN_DECLS

typedef enum
{
  AFP_VERSION_INVALID,
  AFP_VERSION_3_0,
  AFP_VERSIOM_3_1,
  AFP_VERSION_3_2,
  AFP_VERSION_3_3
} AfpVersion;

#define G_VFS_TYPE_AFP_SERVER             (g_vfs_afp_server_get_type ())
#define G_VFS_AFP_SERVER(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_VFS_TYPE_AFP_SERVER, GVfsAfpServer))
#define G_VFS_AFP_SERVER_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), G_VFS_TYPE_AFP_SERVER, GVfsAfpServerClass))
#define G_VFS_IS_AFP_SERVER(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_VFS_TYPE_AFP_SERVER))
#define G_VFS_IS_AFP_SERVER_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), G_VFS_TYPE_AFP_SERVER))
#define G_VFS_AFP_SERVER_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), G_VFS_TYPE_AFP_SERVER, GVfsAfpServerClass))

typedef struct _GVfsAfpServerClass GVfsAfpServerClass;
typedef struct _GVfsAfpServer GVfsAfpServer;

struct _GVfsAfpServerClass
{
  GObjectClass parent_class;
};

struct _GVfsAfpServer
{
  GObject parent_instance;

  GNetworkAddress     *addr;
  GVfsAfpConnection   *conn;

  guint16             flags;
  char                *machine_type;
  char                *server_name;
  char                *utf8_server_name;
  GSList              *uams;
  AfpVersion          version;
};

gboolean           g_vfs_afp_server_login           (GVfsAfpServer *afp_serv,
                                                     const char     *initial_user,
                                                     GMountSource   *mount_source,
                                                     GCancellable   *cancellable,
                                                     GError         **error);

GVfsAfpServer*     g_vfs_afp_server_new             (GNetworkAddress *addr);

GType g_vfs_afp_server_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif /* _GVFSAFPSERVER_H_ */
