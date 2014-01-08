/* GIO - GLib Input, Output and Streaming Library
 *   Volume Monitor for MTP Backend
 *
 * Copyright (C) 2012 Philip Langdale <philipl@overt.org>
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
 * Public License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef __G_MTP_VOLUME_H__
#define __G_MTP_VOLUME_H__

#include <glib-object.h>
#include <gio/gio.h>

#include <gudev/gudev.h>
#include "gmtpvolumemonitor.h"

G_BEGIN_DECLS

#define G_TYPE_MTP_VOLUME        (g_mtp_volume_get_type ())
#define G_MTP_VOLUME(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_MTP_VOLUME, GMtpVolume))
#define G_MTP_VOLUME_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_MTP_VOLUME, GMtpVolumeClass))
#define G_IS_MTP_VOLUME(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_MTP_VOLUME))
#define G_IS_MTP_VOLUME_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_MTP_VOLUME))

typedef struct _GMtpVolumeClass GMtpVolumeClass;

struct _GMtpVolumeClass {
   GObjectClass parent_class;
};

GType g_mtp_volume_get_type (void) G_GNUC_CONST;

GMtpVolume *g_mtp_volume_new      (GVolumeMonitor *volume_monitor,
                                   GUdevDevice    *device,
                                   GUdevClient    *gudev_client,
                                   GFile          *activation_root);

gboolean    g_mtp_volume_has_path (GMtpVolume     *volume,
                                   const char     *path);

void        g_mtp_volume_removed  (GMtpVolume     *volume);

G_END_DECLS

#endif /* __G_MTP_VOLUME_H__ */
