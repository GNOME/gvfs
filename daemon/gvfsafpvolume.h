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

#ifndef _GVFSAFPVOLUME_H_
#define _GVFSAFPVOLUME_H_

#include "gvfsafptypes.h"
#include "gvfsafpconnection.h"

G_BEGIN_DECLS

#define G_VFS_TYPE_AFP_VOLUME             (g_vfs_afp_volume_get_type ())
#define G_VFS_AFP_VOLUME(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_VFS_TYPE_AFP_VOLUME, GVfsAfpVolume))
#define G_VFS_AFP_VOLUME_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), G_VFS_TYPE_AFP_VOLUME, GVfsAfpVolumeClass))
#define G_VFS_IS_AFP_VOLUME(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_VFS_TYPE_AFP_VOLUME))
#define G_VFS_IS_AFP_VOLUME_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), G_VFS_TYPE_AFP_VOLUME))
#define G_VFS_AFP_VOLUME_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), G_VFS_TYPE_AFP_VOLUME, GVfsAfpVolumeClass))

typedef struct _GVfsAfpVolumeClass GVfsAfpVolumeClass;
typedef struct _GVfsAfpVolumePrivate GVfsAfpVolumePrivate;

struct _GVfsAfpVolumeClass
{
  GObjectClass parent_class;
};

struct _GVfsAfpVolume
{
  GObject parent_instance;

  GVfsAfpVolumePrivate *priv;
};

GType g_vfs_afp_volume_get_type (void) G_GNUC_CONST;

GVfsAfpVolume *g_vfs_afp_volume_new                 (GVfsAfpServer *server, GVfsAfpConnection *conn);

gboolean       g_vfs_afp_volume_mount_sync          (GVfsAfpVolume *volume,
                                                     const char    *volume_name,
                                                     GCancellable  *cancellable,
                                                     GError       **error);

guint16        g_vfs_afp_volume_get_attributes      (GVfsAfpVolume *volume);
guint16        g_vfs_afp_volume_get_id              (GVfsAfpVolume *volume);

void           g_vfs_afp_volume_get_parms           (GVfsAfpVolume        *volume,
                                                     guint16              vol_bitmap,
                                                     GCancellable        *cancellable,
                                                     GAsyncReadyCallback  callback,
                                                     gpointer             user_data);

GFileInfo *    g_vfs_afp_volume_get_parms_finish    (GVfsAfpVolume       *volume,
                                                     GAsyncResult        *result,
                                                     GError             **error);

void           g_vfs_afp_volume_open_fork           (GVfsAfpVolume      *volume,
                                                     const char         *filename,
                                                     guint16             access_mode,
                                                     guint16             bitmap,
                                                     GCancellable       *cancellable,
                                                     GAsyncReadyCallback callback,
                                                     gpointer            user_data);

gboolean       g_vfs_afp_volume_open_fork_finish    (GVfsAfpVolume  *volume,
                                                     GAsyncResult   *res,
                                                     gint16         *fork_refnum,
                                                     GFileInfo      **info,
                                                     GError         **error);

void           g_vfs_afp_volume_close_fork          (GVfsAfpVolume       *volume,
                                                     gint16               fork_refnum,
                                                     GCancellable        *cancellable,
                                                     GAsyncReadyCallback  callback,
                                                     gpointer             user_data);

gboolean      g_vfs_afp_volume_close_fork_finish   (GVfsAfpVolume  *volume,
                                                    GAsyncResult   *result,
                                                    GError         **error);

void          g_vfs_afp_volume_delete              (GVfsAfpVolume       *volume,
                                                    const char          *filename,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);

gboolean      g_vfs_afp_volume_delete_finish       (GVfsAfpVolume  *volume,
                                                    GAsyncResult   *result,
                                                    GError         **error);

void          g_vfs_afp_volume_create_file         (GVfsAfpVolume      *volume,
                                                    const char         *filename,
                                                    gboolean            hard_create,
                                                    GCancellable       *cancellable,
                                                    GAsyncReadyCallback callback,
                                                    gpointer            user_data);

gboolean     g_vfs_afp_volume_create_file_finish   (GVfsAfpVolume  *volume,
                                                    GAsyncResult   *result,
                                                    GError         **error);

void         g_vfs_afp_volume_create_directory     (GVfsAfpVolume      *volume,
                                                    const char         *directory,
                                                    GCancellable       *cancellable,
                                                    GAsyncReadyCallback callback,
                                                    gpointer            user_data);

gboolean     g_vfs_afp_volume_create_directory_finish (GVfsAfpVolume  *volume,
                                                       GAsyncResult   *result,
                                                       GError         **error);

void         g_vfs_afp_volume_copy_file            (GVfsAfpVolume      *volume,
                                                    const char         *source,
                                                    const char         *destination,
                                                    GCancellable       *cancellable,
                                                    GAsyncReadyCallback callback,
                                                    gpointer            user_data);

gboolean     g_vfs_afp_volume_copy_file_finish     (GVfsAfpVolume *volume,
                                                    GAsyncResult  *res,
                                                    GError       **error);

void         g_vfs_afp_volume_rename               (GVfsAfpVolume      *volume,
                                                    const char         *filename,
                                                    const char         *new_name,
                                                    GCancellable       *cancellable,
                                                    GAsyncReadyCallback callback,
                                                    gpointer            user_data);

gboolean     g_vfs_afp_volume_rename_finish        (GVfsAfpVolume  *volume,
                                                    GAsyncResult   *res,
                                                    GError        **error);

void         g_vfs_afp_volume_move_and_rename      (GVfsAfpVolume      *volume,
                                                    const char         *source,
                                                    const char         *destination,
                                                    GCancellable       *cancellable,
                                                    GAsyncReadyCallback callback,
                                                    gpointer            user_data);

gboolean     g_vfs_afp_volume_move_and_rename_finish (GVfsAfpVolume  *volume,
                                                      GAsyncResult   *res,
                                                      GError        **error);

void          g_vfs_afp_volume_get_filedir_parms   (GVfsAfpVolume       *volume,
                                                    const char          *filename,
                                                    guint16              file_bitmap,
                                                    guint16              dir_bitmap,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);

GFileInfo *   g_vfs_afp_volume_get_filedir_parms_finish (GVfsAfpVolume  *volume,
                                                         GAsyncResult   *result,
                                                         GError         **error);

void          g_vfs_afp_volume_get_fork_parms      (GVfsAfpVolume       *volume,
                                                    gint16               fork_refnum,
                                                    guint16              file_bitmap,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);

GFileInfo *   g_vfs_afp_volume_get_fork_parms_finish (GVfsAfpVolume  *volume,
                                                      GAsyncResult   *result,
                                                      GError         **error);

void          g_vfs_afp_volume_set_fork_size         (GVfsAfpVolume       *volume,
                                                      gint16               fork_refnum,
                                                      gint64               size,
                                                      GCancellable        *cancellable,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data);

gboolean      g_vfs_afp_volume_set_fork_size_finish  (GVfsAfpVolume  *volume,
                                                      GAsyncResult   *result,
                                                      GError         **error);

void          g_vfs_afp_volume_set_unix_privs      (GVfsAfpVolume       *volume,
                                                    const char          *filename,
                                                    guint32              uid,
                                                    guint32              gid,
                                                    guint32              permissions,
                                                    guint32              ua_permissions,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);

gboolean      g_vfs_afp_volume_set_unix_privs_finish (GVfsAfpVolume  *volume,
                                                      GAsyncResult   *res,
                                                      GError        **error);

void          g_vfs_afp_volume_enumerate             (GVfsAfpVolume       *volume,
                                                      const char          *directory,
                                                      gint64               start_index,
                                                      guint16              file_bitmap,
                                                      guint16              dir_bitmap,
                                                      GCancellable        *cancellable,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data);

gboolean      g_vfs_afp_volume_enumerate_finish      (GVfsAfpVolume  *volume,
                                                      GAsyncResult   *res,
                                                      GPtrArray      **infos,
                                                      GError        **error);

void          g_vfs_afp_volume_exchange_files        (GVfsAfpVolume       *volume,
                                                      const char          *source,
                                                      const char          *destination,
                                                      GCancellable        *cancellable,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data);

gboolean      g_vfs_afp_volume_exchange_files_finish (GVfsAfpVolume  *volume,
                                                      GAsyncResult   *res,
                                                      GError        **error);

void          g_vfs_afp_volume_write_to_fork         (GVfsAfpVolume       *volume,
                                                      guint16              fork_refnum,
                                                      char                *buffer,
                                                      gsize                buffer_size,
                                                      gint64               offset,
                                                      GCancellable        *cancellable,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data);

gboolean      g_vfs_afp_volume_write_to_fork_finish  (GVfsAfpVolume  *volume,
                                                      GAsyncResult   *res,
                                                      gint64         *last_written,
                                                      GError        **error);

void          g_vfs_afp_volume_read_from_fork        (GVfsAfpVolume       *volume,
                                                      guint16              fork_refnum,
                                                      char                *buffer,
                                                      gsize                bytes_requested,
                                                      gint64               offset,
                                                      GCancellable        *cancellable,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data);

gboolean      g_vfs_afp_volume_read_from_fork_finish (GVfsAfpVolume  *volume,
                                                      GAsyncResult   *res,
                                                      gsize          *bytes_read,
                                                      GError        **error);


G_END_DECLS

#endif /* _GVFSAFPVOLUME_H_ */
