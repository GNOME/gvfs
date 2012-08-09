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

#ifndef _GVFSAFPSERVER_H_
#define _GVFSAFPSERVER_H_

#include <gio/gio.h>

#include "gmountsource.h"
#include "gvfsafpconnection.h"

#include "gvfsafptypes.h"

G_BEGIN_DECLS

typedef enum
{
  AFP_VERSION_INVALID,
  AFP_VERSION_3_0,
  AFP_VERSION_3_1,
  AFP_VERSION_3_2,
  AFP_VERSION_3_3
} AfpVersion;

typedef struct
{
  guint16             flags;
  char                *machine_type;
  char                *server_name;
  char                *utf8_server_name;
  GSList              *uams;
  AfpVersion          version;
} GVfsAfpServerInfo;

#define G_VFS_TYPE_AFP_SERVER             (g_vfs_afp_server_get_type ())
#define G_VFS_AFP_SERVER(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_VFS_TYPE_AFP_SERVER, GVfsAfpServer))
#define G_VFS_AFP_SERVER_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), G_VFS_TYPE_AFP_SERVER, GVfsAfpServerClass))
#define G_VFS_IS_AFP_SERVER(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_VFS_TYPE_AFP_SERVER))
#define G_VFS_IS_AFP_SERVER_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), G_VFS_TYPE_AFP_SERVER))
#define G_VFS_AFP_SERVER_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), G_VFS_TYPE_AFP_SERVER, GVfsAfpServerClass))

typedef struct _GVfsAfpServerClass GVfsAfpServerClass;
typedef struct _GvfsAfpServerPrivate GVfsAfpServerPrivate;

struct _GVfsAfpServerClass
{
  GObjectClass parent_class;
};

struct _GVfsAfpServer
{
  GObject parent_instance;

  GVfsAfpServerPrivate *priv;
};

GType              g_vfs_afp_server_get_type             (void) G_GNUC_CONST;

GVfsAfpServer*     g_vfs_afp_server_new                  (GNetworkAddress *addr);

gboolean           g_vfs_afp_server_login                (GVfsAfpServer *server,
                                                          const char     *initial_user,
                                                          GMountSource   *mount_source,
                                                          char           **logged_in_user,
                                                          GCancellable   *cancellable,
                                                          GError         **error);

gboolean           g_vfs_afp_server_logout_sync          (GVfsAfpServer *server,
                                                          GCancellable  *cancellable,
                                                          GError       **error);

gint64             g_vfs_afp_server_time_to_local_time   (GVfsAfpServer *server,
                                                          gint32         server_time);

const
GVfsAfpServerInfo* g_vfs_afp_server_get_info             (GVfsAfpServer *server);

guint32            g_vfs_afp_server_get_max_request_size (GVfsAfpServer *server);

typedef struct _GVfsAfpVolumeData GVfsAfpVolumeData;
struct _GVfsAfpVolumeData
{
  char *name;
  guint16 flags;
};

void               g_vfs_afp_server_get_volumes          (GVfsAfpServer       *server,
                                                          GCancellable        *cancellable,
                                                          GAsyncReadyCallback  callback,
                                                          gpointer             user_data);
GPtrArray *        g_vfs_afp_server_get_volumes_finish   (GVfsAfpServer  *server,
                                                          GAsyncResult   *result,
                                                          GError         **error);

GVfsAfpVolume *    g_vfs_afp_server_mount_volume_sync (GVfsAfpServer *server,
                                                       const char    *volume_name,
                                                       GCancellable  *cancellable,
                                                       GError        **error);

gboolean           g_vfs_afp_server_fill_info         (GVfsAfpServer *server,
                                                       GFileInfo     *info,
                                                       GVfsAfpReply  *reply,
                                                       gboolean       directory,
                                                       guint16        bitmap,
                                                       GError        **error);


typedef enum
{
  GVFS_AFP_MAP_ID_FUNCTION_USER_ID_TO_NAME         = 1,
  GVFS_AFP_MAP_ID_FUNCTION_GROUP_ID_TO_NAME        = 2,
  GVFS_AFP_MAP_ID_FUNCTION_USER_ID_TO_UTF8_NAME    = 3,
  GVFS_AFP_MAP_ID_FUNCTION_GROUP_ID_TO_UTF8_NAME   = 4,
  GVFS_AFP_MAP_ID_FUNCTION_USER_UUID_TO_UTF8_NAME  = 5,
  GVFS_AFP_MAP_ID_FUNCTION_GROUP_UUID_TO_UTF8_NAME = 6
} GVfsAfpMapIDFunction;

void          g_vfs_afp_server_map_id                 (GVfsAfpServer       *server,
                                                       GVfsAfpMapIDFunction map_function,
                                                       gint64               id,
                                                       GCancellable        *cancellable,
                                                       GAsyncReadyCallback  callback,
                                                       gpointer             user_data);

char *        g_vfs_afp_server_map_id_finish          (GVfsAfpServer        *server,
                                                       GAsyncResult         *res,
                                                       GVfsAfpMapIDFunction *map_function,
                                                       GError              **error);

G_END_DECLS

#endif /* _GVFSAFPSERVER_H_ */
