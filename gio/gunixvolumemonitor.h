#ifndef __G_UNIX_VOLUME_MONITOR_H__
#define __G_UNIX_VOLUME_MONITOR_H__

#include <glib-object.h>
#include <gio/gvolumemonitor.h>

G_BEGIN_DECLS

#define G_TYPE_UNIX_VOLUME_MONITOR        (g_unix_volume_monitor_get_type ())
#define G_UNIX_VOLUME_MONITOR(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_UNIX_VOLUME_MONITOR, GUnixVolumeMonitor))
#define G_UNIX_VOLUME_MONITOR_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_UNIX_VOLUME_MONITOR, GUnixVolumeMonitorClass))
#define G_IS_UNIX_VOLUME_MONITOR(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_UNIX_VOLUME_MONITOR))
#define G_IS_UNIX_VOLUME_MONITOR_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_UNIX_VOLUME_MONITOR))

typedef struct _GUnixVolumeMonitor GUnixVolumeMonitor;
typedef struct _GUnixVolumeMonitorClass GUnixVolumeMonitorClass;

/* Forward definitions */
typedef struct _GUnixVolume GUnixVolume;
typedef struct _GUnixDrive GUnixDrive;

struct _GUnixVolumeMonitorClass {
  GVolumeMonitorClass parent_class;

};

GType g_unix_volume_monitor_get_type (void) G_GNUC_CONST;

GVolumeMonitor *g_unix_volume_monitor_new                         (void);
GUnixDrive *    g_unix_volume_monitor_lookup_drive_for_mountpoint (GUnixVolumeMonitor *monitor,
								   const char         *mountpoint);

G_END_DECLS

#endif /* __G_UNIX_VOLUME_MONITOR_H__ */
