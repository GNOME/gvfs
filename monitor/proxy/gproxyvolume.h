/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2008 Red Hat, Inc.
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

#ifndef __G_PROXY_VOLUME_H__
#define __G_PROXY_VOLUME_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "gproxyvolumemonitor.h"
#include "gproxyshadowmount.h"

G_BEGIN_DECLS

#define G_TYPE_PROXY_VOLUME        (g_proxy_volume_get_type ())
#define G_PROXY_VOLUME(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_PROXY_VOLUME, GProxyVolume))
#define G_PROXY_VOLUME_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_PROXY_VOLUME, GProxyVolumeClass))
#define G_IS_PROXY_VOLUME(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_PROXY_VOLUME))
#define G_IS_PROXY_VOLUME_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_PROXY_VOLUME))

typedef struct _GProxyVolumeClass GProxyVolumeClass;

struct _GProxyVolumeClass {
   GObjectClass parent_class;
};

GType         g_proxy_volume_get_type            (void) G_GNUC_CONST;
GProxyVolume *g_proxy_volume_new                 (GProxyVolumeMonitor *volume_monitor);
void          g_proxy_volume_update              (GProxyVolume        *volume,
                                                  GVariant            *iter);
const char   *g_proxy_volume_get_id              (GProxyVolume        *volume);
void          g_proxy_volume_register            (GIOModule           *module);

GProxyShadowMount *g_proxy_volume_get_shadow_mount (GProxyVolume        *volume);

G_END_DECLS

#endif /* __G_PROXY_VOLUME_H__ */
