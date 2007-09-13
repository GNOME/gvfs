#ifndef __G_UNIX_DRIVE_H__
#define __G_UNIX_DRIVE_H__

#include <glib-object.h>
#include <gio/gdrive.h>
#include <gio/gunixmounts.h>
#include <gio/gvolumemonitor.h>

G_BEGIN_DECLS

#define G_TYPE_UNIX_DRIVE        (g_unix_drive_get_type ())
#define G_UNIX_DRIVE(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_UNIX_DRIVE, GUnixDrive))
#define G_UNIX_DRIVE_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_UNIX_DRIVE, GUnixDriveClass))
#define G_IS_UNIX_DRIVE(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_UNIX_DRIVE))
#define G_IS_UNIX_DRIVE_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_UNIX_DRIVE))

typedef struct _GUnixDrive GUnixDrive;
typedef struct _GUnixDriveClass GUnixDriveClass;

struct _GUnixDriveClass {
   GObjectClass parent_class;
};

GType g_unix_drive_get_type (void) G_GNUC_CONST;

GUnixDrive *g_unix_drive_new            (GVolumeMonitor *volume_monitor,
					 GUnixMountPoint *mountpoint);
gboolean    g_unix_drive_has_mountpoint (GUnixDrive     *drive,
					 const char     *mountpoint);

G_END_DECLS

#endif /* __G_UNIX_DRIVE_H__ */
