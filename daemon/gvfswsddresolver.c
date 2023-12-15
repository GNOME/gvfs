/* gvfswsddresolver.c
 *
 * Copyright (C) 2023 Red Hat, Inc.
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
 * You should have received a copy of the GNU Library General Public
 * License along with the Gnome Library; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Authors: Ondrej Holy <oholy@redhat.com>
 */

#include <glib.h>
#include <gio/gio.h>
#include <glib/gi18n.h>

#include "gvfswsddresolver.h"

#define RESOLVER_TIMEOUT 60000

struct _GVfsWsddResolver
{
  GObject parent_instance;

  GResolver *dns_resolver;
  GHashTable *cache;
  GCancellable *cancellable;
};

enum
{
  DEVICE_RESOLVED_SIGNAL,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (GVfsWsddResolver, g_vfs_wsdd_resolver, G_TYPE_OBJECT)

static void
g_vfs_wsdd_resolver_dispose (GObject *object)
{
  GVfsWsddResolver *resolver = G_VFS_WSDD_RESOLVER (object);

  g_cancellable_cancel (resolver->cancellable);

  G_OBJECT_CLASS (g_vfs_wsdd_resolver_parent_class)->dispose (object);
}

static void
g_vfs_wsdd_resolver_finalize (GObject *object)
{
  GVfsWsddResolver *resolver = G_VFS_WSDD_RESOLVER (object);

  g_clear_object (&resolver->cancellable);
  g_clear_object (&resolver->dns_resolver);

  g_clear_pointer (&resolver->cache, g_hash_table_unref);

  G_OBJECT_CLASS (g_vfs_wsdd_resolver_parent_class)->finalize (object);
}

static void
g_vfs_wsdd_resolver_init (GVfsWsddResolver *resolver)
{
  resolver->dns_resolver = g_resolver_get_default ();
  g_resolver_set_timeout (resolver->dns_resolver, RESOLVER_TIMEOUT);

  resolver->cache = g_hash_table_new_full ((GHashFunc) g_vfs_wsdd_device_hash,
                                           (GEqualFunc) g_vfs_wsdd_device_equal,
                                           (GDestroyNotify) g_object_unref,
                                           g_free);

  resolver->cancellable = g_cancellable_new ();
}

static void
g_vfs_wsdd_resolver_class_init (GVfsWsddResolverClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = g_vfs_wsdd_resolver_dispose;
  gobject_class->finalize = g_vfs_wsdd_resolver_finalize;

  /**
   * GVfsWsddResolver::device-resolved:
   * @wsdd_resolver: the #GVfsWsddResolver
   * @uuid: an uuid
   *
   * Emitted when hostname for #GVfsWsddDevice with @uuid is resolved.
   **/
  signals[DEVICE_RESOLVED_SIGNAL] = g_signal_new ("device-resolved",
                                                  G_TYPE_FROM_CLASS (klass),
                                                  G_SIGNAL_RUN_LAST,
                                                  0,
                                                  NULL,
                                                  NULL,
                                                  g_cclosure_marshal_generic,
                                                  G_TYPE_NONE,
                                                  1,
                                                  G_TYPE_STRING);
}

GVfsWsddResolver *
g_vfs_wsdd_resolver_new (void)
{
  return g_object_new (G_VFS_TYPE_WSDD_RESOLVER, NULL);
}

typedef struct {
  GVfsWsddResolver *resolver;
  GVfsWsddDevice *device;
  gchar *address;
} ResolveData;

static void
resolve_data_free (ResolveData *data)
{
  g_object_unref (data->resolver);
  g_object_unref (data->device);
  g_free (data->address);
  g_free (data);
}

static void
lookup_by_llmnr_name_cb (GObject* source_object,
                         GAsyncResult* result,
                         gpointer user_data)
{
  ResolveData *data = user_data;
  GList *addresses;

  addresses = g_resolver_lookup_by_name_finish (G_RESOLVER (source_object),
                                                result,
                                                NULL);
  if (addresses == NULL)
    {
      g_debug ("Failed to resolve address for device: %s\n",
               g_vfs_wsdd_device_get_uuid (data->device));

      resolve_data_free (data);
      return;
    }

  g_hash_table_insert (data->resolver->cache,
                       g_object_ref (data->device),
                       g_steal_pointer (&data->address));
  g_signal_emit (data->resolver,
                 signals[DEVICE_RESOLVED_SIGNAL],
                 0,
                 g_vfs_wsdd_device_get_uuid (data->device));

  g_resolver_free_addresses (addresses);
  resolve_data_free (data);
}

static void
lookup_by_dnssd_name_cb (GObject* source_object,
                         GAsyncResult* result,
                         gpointer user_data)
{
  ResolveData *data = user_data;
  g_autoptr(GError) error = NULL;
  GList *addresses;

  addresses = g_resolver_lookup_by_name_finish (G_RESOLVER (source_object),
                                                result,
                                                &error);
  if (addresses == NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          resolve_data_free (data);
          return;
        }

      /* Try llmnr address instead. */
      g_free (data->address);
      data->address = g_strdup (g_vfs_wsdd_device_get_name (data->device));
      g_resolver_lookup_by_name_async (data->resolver->dns_resolver,
                                       data->address,
                                       data->resolver->cancellable,
                                       lookup_by_llmnr_name_cb,
                                       data);
      return;
    }

  g_hash_table_insert (data->resolver->cache,
                       g_object_ref (data->device),
                       g_steal_pointer (&data->address));
  g_signal_emit (data->resolver,
                 signals[DEVICE_RESOLVED_SIGNAL],
                 0,
                 g_vfs_wsdd_device_get_uuid (data->device));

  g_resolver_free_addresses (addresses);
  resolve_data_free (data);
}

static gboolean
is_valid_netbios_name (const gchar *name)
{
  static GRegex* regex = NULL;

  if (regex == NULL)
    {
      regex = g_regex_new ("^[a-zA-Z0-9-]{1,15}$", 0, 0, NULL);
    }

  return g_regex_match (regex, name, 0, NULL);
}

void
g_vfs_wsdd_resolver_resolve (GVfsWsddResolver *resolver,
                             GVfsWsddDevice *device)
{
  ResolveData *data;
  const gchar *name;

  if (g_hash_table_contains (resolver->cache, device))
    {
      return;
    }

  /* Add placeholder to avoid multiple queues for the same device. */
  g_hash_table_insert (resolver->cache,
                       g_object_ref (device),
                       g_vfs_wsdd_device_get_first_address (device));

  name = g_vfs_wsdd_device_get_name (device);
  if (!is_valid_netbios_name (name))
    {
      g_debug ("The device has invalid netbios name: %s\n",
               g_vfs_wsdd_device_get_uuid (device));
      return;
    }

  data = g_new (ResolveData, 1);
  data->resolver = g_object_ref (resolver);
  data->device = g_object_ref (device);

  /* Try dnssd address first. */
  data->address = g_strconcat (name, ".local", NULL);
  g_resolver_lookup_by_name_async (resolver->dns_resolver,
                                   data->address,
                                   resolver->cancellable,
                                   lookup_by_dnssd_name_cb,
                                   data);
}

gchar *
g_vfs_wsdd_resolver_get_address (GVfsWsddResolver *resolver,
                                 GVfsWsddDevice *device)
{
  return g_strdup (g_hash_table_lookup (resolver->cache, device));
}
