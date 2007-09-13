#ifndef __G_UNION_DRIVE_H__
#define __G_UNION_DRIVE_H__

#include <glib-object.h>
#include <gio/gdrive.h>
#include <gio/gvolumemonitor.h>

G_BEGIN_DECLS

#define G_TYPE_UNION_DRIVE        (g_union_drive_get_type ())
#define G_UNION_DRIVE(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_UNION_DRIVE, GUnionDrive))
#define G_UNION_DRIVE_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_UNION_DRIVE, GUnionDriveClass))
#define G_IS_UNION_DRIVE(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_UNION_DRIVE))
#define G_IS_UNION_DRIVE_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_UNION_DRIVE))

typedef struct _GUnionDrive GUnionDrive;
typedef struct _GUnionDriveClass GUnionDriveClass;

struct _GUnionDriveClass {
   GObjectClass parent_class;
};

GType g_union_drive_get_type (void) G_GNUC_CONST;

GUnionDrive *g_union_drive_new                   (GVolumeMonitor *union_monitor,
						  GDrive         *child_drive,
						  GVolumeMonitor *child_monitor);
gboolean     g_union_drive_has_child_drive       (GUnionDrive    *union_drive,
						  GDrive         *child_drive);
GDrive *     g_union_drive_get_child_for_monitor (GUnionDrive    *union_drive,
						  GVolumeMonitor *child_monitor);

G_END_DECLS

#endif /* __G_UNION_DRIVE_H__ */
