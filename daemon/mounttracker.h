#ifndef __G_MOUNT_TRACKER_H__
#define __G_MOUNT_TRACKER_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define G_TYPE_MOUNT_TRACKER         (g_mount_tracker_get_type ())
#define G_MOUNT_TRACKER(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_MOUNT_TRACKER, GMountTracker))
#define G_MOUNT_TRACKER_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_MOUNT_TRACKER, GMountTrackerClass))
#define G_IS_MOUNT_TRACKER(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_MOUNT_TRACKER))
#define G_IS_MOUNT_TRACKER_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_MOUNT_TRACKER))
#define G_MOUNT_TRACKER_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_MOUNT_TRACKER, GMountTrackerClass))

typedef struct _GMountTracker        GMountTracker;
typedef struct _GMountTrackerClass   GMountTrackerClass;

struct _GMountTrackerClass
{
  GObjectClass parent_class;
};

GType g_mount_tracker_get_type (void) G_GNUC_CONST;

GMountTracker *g_mount_tracker_new (void);

G_END_DECLS

#endif /* __G_MOUNT_TRACKER_H__ */
