/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2008 Red Hat, Inc.
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

#ifndef __G_VFS_DNS_SD_UTILS_H__
#define __G_VFS_DNS_SD_UTILS_H__

#include <gio/gio.h>

G_BEGIN_DECLS

gchar *g_vfs_encode_dns_sd_triple (const gchar *service_name,
                                   const gchar *service_type,
                                   const gchar *domain);

gchar *g_vfs_normalize_encoded_dns_sd_triple (const gchar *encoded_triple);

gboolean
g_vfs_decode_dns_sd_triple (const gchar *encoded_triple,
                            gchar      **out_service_name,
                            gchar      **out_service_type,
                            gchar      **out_domain,
                            GError     **error);

gchar *
g_vfs_get_dns_sd_uri_for_triple (const gchar *service_name,
                                 const gchar *service_type,
                                 const gchar *domain);

G_END_DECLS

#endif /* __G_VFS_DNS_SD_UTILS_H__ */
