/* gvfswsdddevice.c
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

#include <config.h>

#include <glib.h>

#include "gvfswsdddevice.h"

struct _GVfsWsddDevice
{
  GObject parent_instance;

  gchar *uuid;
  gchar *name;
  gchar *addresses;
};

G_DEFINE_TYPE (GVfsWsddDevice, g_vfs_wsdd_device, G_TYPE_OBJECT)

static void
g_vfs_wsdd_device_finalize (GObject *object)
{
  GVfsWsddDevice *device = G_VFS_WSDD_DEVICE (object);

  g_clear_pointer (&device->uuid, g_free);
  g_clear_pointer (&device->name, g_free);
  g_clear_pointer (&device->addresses, g_free);

  G_OBJECT_CLASS (g_vfs_wsdd_device_parent_class)->finalize (object);
}

static void
g_vfs_wsdd_device_init (GVfsWsddDevice *device)
{
}

static void
g_vfs_wsdd_device_class_init (GVfsWsddDeviceClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = g_vfs_wsdd_device_finalize;
}

GVfsWsddDevice *
g_vfs_wsdd_device_new (const gchar *uuid,
                       const gchar *name,
                       const gchar *addresses)
{
  GVfsWsddDevice *device;

  device = g_object_new (G_VFS_TYPE_WSDD_DEVICE, NULL);
  device->uuid = g_strdup (uuid);
  device->name = g_strdup (name);
  device->addresses = g_strdup (addresses);

  return device;
}

gint
g_vfs_wsdd_device_compare (GVfsWsddDevice *a,
                           GVfsWsddDevice *b)
{
 return g_strcmp0 (a->uuid, b->uuid);
}

gboolean
g_vfs_wsdd_device_equal (GVfsWsddDevice *a,
                         GVfsWsddDevice *b)
{
  if (g_strcmp0 (a->uuid, b->uuid) == 0 &&
      g_strcmp0 (a->name, b->name) == 0 &&
      g_strcmp0 (a->addresses, b->addresses) == 0)
    {
      return TRUE;
    }

  return FALSE;
}

guint
g_vfs_wsdd_device_hash (GVfsWsddDevice *device)
{
  return g_str_hash (device->uuid);
}

const gchar *
g_vfs_wsdd_device_get_uuid (GVfsWsddDevice *device)
{
  return device->uuid;
}

const gchar *
g_vfs_wsdd_device_get_name (GVfsWsddDevice *device)
{
  return device->name;
}

const gchar *
g_vfs_wsdd_device_get_addresses (GVfsWsddDevice *device)
{
  return device->addresses;
}

gchar *
g_vfs_wsdd_device_get_first_address (GVfsWsddDevice *device)
{
  static GRegex* regex = NULL;
  g_autoptr(GMatchInfo) match_info = NULL;

  if (regex == NULL)
    {
      /* e.g. "wlp0s20f3, {'[fe80::df0:3c72:229f:faf1]', '192.168.1.131'}" */
      regex = g_regex_new ("^.+, {'(.+)'.*$",
                           G_REGEX_UNGREEDY,
                           0,
                           NULL);
    }

  if (!g_regex_match (regex, device->addresses, 0, &match_info))
    {
      g_warning ("Unexpected format of addresses: %s\n", device->addresses);

      return NULL;
    }

  return g_match_info_fetch (match_info, 1);
}
