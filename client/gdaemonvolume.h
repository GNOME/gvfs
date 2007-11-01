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
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#ifndef __G_DAEMON_VOLUME_H__
#define __G_DAEMON_VOLUME_H__

#include <glib-object.h>
#include <gio/gvolume.h>
#include "gdaemonvfs.h"
#include "gdaemonvolumemonitor.h"
#include "gmounttracker.h"

G_BEGIN_DECLS

#define G_TYPE_DAEMON_VOLUME        (g_daemon_volume_get_type ())
#define G_DAEMON_VOLUME(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DAEMON_VOLUME, GDaemonVolume))
#define G_DAEMON_VOLUME_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_DAEMON_VOLUME, GDaemonVolumeClass))
#define G_IS_DAEMON_VOLUME(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DAEMON_VOLUME))
#define G_IS_DAEMON_VOLUME_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DAEMON_VOLUME))

typedef struct _GDaemonVolumeClass GDaemonVolumeClass;

struct _GDaemonVolumeClass {
   GObjectClass parent_class;
};

GType g_daemon_volume_get_type (void) G_GNUC_CONST;

GDaemonVolume *g_daemon_volume_new            (GMountInfo *mount_info);

GMountInfo    *g_daemon_volume_get_mount_info (GDaemonVolume *volume);

G_END_DECLS

#endif /* __G_DAEMON_VOLUME_H__ */
