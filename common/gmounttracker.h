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

#ifndef __G_MOUNT_TRACKER_H__
#define __G_MOUNT_TRACKER_H__

#include <glib-object.h>
#include <gio/gio.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_TYPE_MOUNT_TRACKER         (g_mount_tracker_get_type ())
#define G_MOUNT_TRACKER(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_MOUNT_TRACKER, GMountTracker))
#define G_MOUNT_TRACKER_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_MOUNT_TRACKER, GMountTrackerClass))
#define G_IS_MOUNT_TRACKER(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_MOUNT_TRACKER))
#define G_IS_MOUNT_TRACKER_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_MOUNT_TRACKER))
#define G_MOUNT_TRACKER_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_MOUNT_TRACKER, GMountTrackerClass))

typedef struct _GMountTracker        GMountTracker;
typedef struct _GMountTrackerClass   GMountTrackerClass;

typedef struct {
  volatile int ref_count;
  char *display_name;
  char *stable_name;
  char *x_content_types;
  GIcon *icon;
  char *dbus_id;
  char *object_path;
  gboolean user_visible;
  char *prefered_filename_encoding; /* NULL -> UTF8 */
  char *fuse_mountpoint;
  char *default_location;
  GMountSpec *mount_spec;
} GMountInfo;

struct _GMountTrackerClass
{
  GObjectClass parent_class;

  void (*mounted)   (GMountTracker *tracker,
		     GMountInfo *info);
  void (*unmounted) (GMountTracker *tracker,
		     GMountInfo *info);
};

GType g_mount_tracker_get_type (void) G_GNUC_CONST;

gboolean    g_mount_info_equal        (GMountInfo *info1,
				       GMountInfo *info2);
GMountInfo *g_mount_info_ref          (GMountInfo *info);
GMountInfo *g_mount_info_dup          (GMountInfo *info);
void        g_mount_info_unref        (GMountInfo *info);
const char *g_mount_info_resolve_path (GMountInfo *info,
				       const char *path);

GMountInfo * g_mount_info_from_dbus (DBusMessageIter *iter);

GMountTracker *g_mount_tracker_new                (DBusConnection *connection);
GList *        g_mount_tracker_list_mounts        (GMountTracker *tracker);
GMountInfo *   g_mount_tracker_find_by_mount_spec (GMountTracker *tracker,
						   GMountSpec    *mount_spec);
gboolean       g_mount_tracker_has_mount_spec     (GMountTracker *tracker,
						   GMountSpec    *mount_spec);

G_END_DECLS

#endif /* __G_MOUNT_TRACKER_H__ */
