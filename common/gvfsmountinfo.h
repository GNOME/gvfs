/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2009 Red Hat, Inc.
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

#ifndef __G_VFS_MOUNT_INFO_H__
#define __G_VFS_MOUNT_INFO_H__

#include <gio/gio.h>

G_BEGIN_DECLS

void g_vfs_mount_info_query_autorun_info (GFile               *directory,
                                          GCancellable        *cancellable,
                                          GAsyncReadyCallback  callback,
                                          gpointer             user_data);

GIcon *g_vfs_mount_info_query_autorun_info_finish (GFile          *directory,
                                                   GAsyncResult   *res,
                                                   GError        **error);

void g_vfs_mount_info_query_xdg_volume_info (GFile               *directory,
                                             GCancellable        *cancellable,
                                             GAsyncReadyCallback  callback,
                                             gpointer             user_data);

GIcon *g_vfs_mount_info_query_xdg_volume_info_finish (GFile          *directory,
                                                      GAsyncResult   *res,
                                                      gchar         **out_name,
                                                      GError        **error);

void g_vfs_mount_info_query_bdmv_volume_info (GFile               *directory,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data);

GIcon *g_vfs_mount_info_query_bdmv_volume_info_finish (GFile          *directory,
                                                       GAsyncResult   *res,
                                                       gchar         **out_name,
                                                       GError        **error);

G_END_DECLS

#endif /* __G_VFS_MOUNT_INFO_H__ */
