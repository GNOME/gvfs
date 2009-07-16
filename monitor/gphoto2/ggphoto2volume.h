/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
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

#ifndef __G_GPHOTO2_VOLUME_H__
#define __G_GPHOTO2_VOLUME_H__

#include <glib-object.h>
#include <gio/gio.h>

#ifdef HAVE_GUDEV
 #include <gudev/gudev.h>
#elif defined(HAVE_HAL)
 #include "hal-pool.h"
#else
 #error Needs gudev or hal
#endif
#include "ggphoto2volumemonitor.h"

G_BEGIN_DECLS

#define G_TYPE_GPHOTO2_VOLUME        (g_gphoto2_volume_get_type ())
#define G_GPHOTO2_VOLUME(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_GPHOTO2_VOLUME, GGPhoto2Volume))
#define G_GPHOTO2_VOLUME_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_GPHOTO2_VOLUME, GGPhoto2VolumeClass))
#define G_IS_GPHOTO2_VOLUME(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_GPHOTO2_VOLUME))
#define G_IS_GPHOTO2_VOLUME_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_GPHOTO2_VOLUME))

typedef struct _GGPhoto2VolumeClass GGPhoto2VolumeClass;

struct _GGPhoto2VolumeClass {
   GObjectClass parent_class;
};

GType g_gphoto2_volume_get_type (void) G_GNUC_CONST;

GGPhoto2Volume *g_gphoto2_volume_new            (GVolumeMonitor   *volume_monitor,
#ifdef HAVE_GUDEV
                                                 GUdevDevice      *device,
                                                 GUdevClient      *gudev_client,
#else
                                                 HalDevice        *device,
                                                 HalPool          *pool,
#endif
                                                 GFile            *activation_root);

#ifdef HAVE_GUDEV
gboolean    g_gphoto2_volume_has_path       (GGPhoto2Volume       *volume,
                                             const char       *path);
#else
gboolean    g_gphoto2_volume_has_udi        (GGPhoto2Volume       *volume,
                                             const char       *udi);
#endif

void        g_gphoto2_volume_removed        (GGPhoto2Volume       *volume);

G_END_DECLS

#endif /* __G_GPHOTO2_VOLUME_H__ */
