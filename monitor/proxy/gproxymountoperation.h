/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2009 Red Hat, Inc.
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

#ifndef __G_PROXY_MOUNT_OPERATION_H__
#define __G_PROXY_MOUNT_OPERATION_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "gproxyvolumemonitor.h"

G_BEGIN_DECLS

const gchar *g_proxy_mount_operation_wrap                (GMountOperation     *op,
                                                          GProxyVolumeMonitor *monitor);

void  g_proxy_mount_operation_handle_ask_password   (const gchar        *wrapped_id,
                                                     const gchar        *message,
                                                     const gchar        *default_user,
                                                     const gchar        *default_domain,
                                                     guint              flags);

void  g_proxy_mount_operation_handle_ask_question   (const gchar        *wrapped_id,
                                                     const gchar        *message,
                                                     const gchar *const *choices);

void  g_proxy_mount_operation_handle_show_processes (const gchar        *wrapped_id,
                                                     const gchar        *message,
                                                     GVariant           *pids,
                                                     const gchar *const *choices);

void  g_proxy_mount_operation_handle_show_unmount_progress (const gchar *wrapped_id,
                                                            const gchar *message,
                                                            gint64       time_left,
                                                            gint64       bytes_left);

void  g_proxy_mount_operation_handle_aborted        (const gchar      *wrapped_id);

void  g_proxy_mount_operation_destroy               (const gchar      *wrapped_id);


G_END_DECLS

#endif /* __G_PROXY_MOUNT_OPERATION_H__ */
