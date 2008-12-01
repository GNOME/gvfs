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

#ifndef __G_VFS_DNS_SD_RESOLVER_H__
#define __G_VFS_DNS_SD_RESOLVER_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_DNS_SD_RESOLVER         (g_vfs_dns_sd_resolver_get_type ())
#define G_VFS_DNS_SD_RESOLVER(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_DNS_SD_RESOLVER, GVfsDnsSdResolver))
#define G_VFS_DNS_SD_RESOLVER_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_DNS_SD_RESOLVER, GVfsDnsSdResolverClass))
#define G_VFS_IS_DNS_SD_RESOLVER(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_DNS_SD_RESOLVER))
#define G_VFS_IS_DNS_SD_RESOLVER_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_DNS_SD_RESOLVER))
#define G_VFS_DNS_SD_RESOLVER_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_DNS_SD_RESOLVER, GVfsDnsSdResolverClass))

/**
 * GVfsDnsSdResolver:
 *
 * Resolves DNS-SD triples.
 */
typedef struct _GVfsDnsSdResolver        GVfsDnsSdResolver;
typedef struct _GVfsDnsSdResolverClass   GVfsDnsSdResolverClass;

GType                g_vfs_dns_sd_resolver_get_type               (void) G_GNUC_CONST;
GVfsDnsSdResolver   *g_vfs_dns_sd_resolver_new_for_encoded_triple (const gchar        *encoded_triple,
                                                                   const gchar        *required_txt_keys);
GVfsDnsSdResolver   *g_vfs_dns_sd_resolver_new_for_service        (const gchar        *service_name,
                                                                   const gchar        *service_type,
                                                                   const gchar        *domain,
                                                                   const gchar        *required_txt_keys);
const gchar         *g_vfs_dns_sd_resolver_get_encoded_triple     (GVfsDnsSdResolver  *resolver);
const gchar         *g_vfs_dns_sd_resolver_get_service_name       (GVfsDnsSdResolver  *resolver);
const gchar         *g_vfs_dns_sd_resolver_get_service_type       (GVfsDnsSdResolver  *resolver);
const gchar         *g_vfs_dns_sd_resolver_get_domain             (GVfsDnsSdResolver  *resolver);
const gchar         *g_vfs_dns_sd_resolver_get_required_txt_keys  (GVfsDnsSdResolver  *resolver);

void                 g_vfs_dns_sd_resolver_resolve                (GVfsDnsSdResolver  *resolver,
                                                                   GCancellable       *cancellable,
                                                                   GAsyncReadyCallback callback,
                                                                   gpointer            user_data);

gboolean             g_vfs_dns_sd_resolver_resolve_finish         (GVfsDnsSdResolver  *resolver,
                                                                   GAsyncResult       *res,
                                                                   GError            **error);

gboolean             g_vfs_dns_sd_resolver_resolve_sync           (GVfsDnsSdResolver  *resolver,
                                                                   GCancellable       *cancellable,
                                                                   GError            **error);

gboolean             g_vfs_dns_sd_resolver_is_resolved            (GVfsDnsSdResolver  *resolver);
gchar               *g_vfs_dns_sd_resolver_get_address            (GVfsDnsSdResolver  *resolver);
guint                g_vfs_dns_sd_resolver_get_port               (GVfsDnsSdResolver  *resolver);
gchar              **g_vfs_dns_sd_resolver_get_txt_records        (GVfsDnsSdResolver  *resolver);
gchar               *g_vfs_dns_sd_resolver_lookup_txt_record      (GVfsDnsSdResolver  *resolver,
                                                                   const gchar        *key);

G_END_DECLS

#endif /* __G_VFS_DNS_SD_RESOLVER_H__ */
