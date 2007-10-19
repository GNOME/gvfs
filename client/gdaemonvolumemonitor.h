#ifndef __G_DAEMON_VOLUME_MONITOR_H__
#define __G_DAEMON_VOLUME_MONITOR_H__

#include <glib-object.h>
#include <gio/gvolumemonitor.h>

G_BEGIN_DECLS

#define G_TYPE_DAEMON_VOLUME_MONITOR        (g_daemon_volume_monitor_get_type ())
#define G_DAEMON_VOLUME_MONITOR(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DAEMON_VOLUME_MONITOR, GDaemonVolumeMonitor))
#define G_DAEMON_VOLUME_MONITOR_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_DAEMON_VOLUME_MONITOR, GDaemonVolumeMonitorClass))
#define G_IS_DAEMON_VOLUME_MONITOR(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DAEMON_VOLUME_MONITOR))
#define G_IS_DAEMON_VOLUME_MONITOR_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DAEMON_VOLUME_MONITOR))

typedef struct _GDaemonVolumeMonitor GDaemonVolumeMonitor;
typedef struct _GDaemonVolumeMonitorClass GDaemonVolumeMonitorClass;

/* Forward definitions */
typedef struct _GDaemonVolume GDaemonVolume;
typedef struct _GDaemonDrive GDaemonDrive;

struct _GDaemonVolumeMonitorClass {
  GVolumeMonitorClass parent_class;

};

GType g_daemon_volume_monitor_get_type (void) G_GNUC_CONST;
void g_daemon_volume_monitor_register_types (GTypeModule *type_module);

GVolumeMonitor *g_daemon_volume_monitor_new (void);

G_END_DECLS

#endif /* __G_DAEMON_VOLUME_MONITOR_H__ */
