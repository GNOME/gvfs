#ifndef __G_MOUNT_TRACKER_H__
#define __G_MOUNT_TRACKER_H__

#include <glib-object.h>
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
  char *display_name;
  char *icon;
  char *dbus_id;
  char *object_path;
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

gboolean    g_mount_info_equal (GMountInfo *info1,
				GMountInfo *info2);
GMountInfo *g_mount_info_dup   (GMountInfo *info);
void        g_mount_info_free  (GMountInfo *info);

GMountTracker *g_mount_tracker_new                (void);
GList *        g_mount_tracker_list_mounts        (GMountTracker *tracker);
GMountInfo *   g_mount_tracker_find_by_mount_spec (GMountTracker *tracker,
						   GMountSpec    *mount_spec);

G_END_DECLS

#endif /* __G_MOUNT_TRACKER_H__ */
