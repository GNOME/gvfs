/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2012 Red Hat, Inc.
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
 * Author: David Zeuthen <davidz@redhat.com>
 */

#ifndef __GVFS_UDISKS2_UTILS_H__
#define __GVFS_UDISKS2_UTILS_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "gvfsudisks2volumemonitor.h"

G_BEGIN_DECLS

void   gvfs_udisks2_utils_udisks_error_to_gio_error (GError *error);
GIcon *gvfs_udisks2_utils_icon_from_fs_type (const gchar *fs_type);
GIcon *gvfs_udisks2_utils_symbolic_icon_from_fs_type (const gchar *fs_type);

gchar *gvfs_udisks2_utils_lookup_fstab_options_value (const gchar *fstab_options,
                                                      const gchar *key);

void     gvfs_udisks2_utils_spawn (guint                timeout_seconds,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data,
                                   const gchar         *command_line_format,
                                   ...);

gboolean gvfs_udisks2_utils_spawn_finish (GAsyncResult   *res,
                                          gint           *out_exit_status,
                                          gchar         **out_standard_output,
                                          gchar         **out_standard_error,
                                          GError        **error);

gboolean gvfs_udisks2_utils_is_drive_on_our_seat (UDisksDrive *drive);

void     gvfs_udisks2_unmount_notify_start (GMountOperation *op,
                                            GMount          *mount,
                                            GDrive          *drive);
void     gvfs_udisks2_unmount_notify_stop  (GMountOperation *op,
                                            gboolean         unmount_failed);

G_END_DECLS

#endif /* __GVFS_UDISKS2_UTILS_H__ */
