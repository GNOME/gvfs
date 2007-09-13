#ifndef __G_VOLUME_MONITOR_H__
#define __G_VOLUME_MONITOR_H__

#include <glib-object.h>
#include <gvolume.h>
#include <gdrive.h>

G_BEGIN_DECLS

#define G_TYPE_VOLUME_MONITOR        (g_volume_monitor_get_type ())
#define G_VOLUME_MONITOR(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VOLUME_MONITOR, GVolumeMonitor))
#define G_VOLUME_MONITOR_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VOLUME_MONITOR, GVolumeMonitorClass))
#define G_IS_VOLUME_MONITOR(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VOLUME_MONITOR))
#define G_IS_VOLUME_MONITOR_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VOLUME_MONITOR))

typedef struct _GVolumeMonitor GVolumeMonitor;
typedef struct _GVolumeMonitorClass GVolumeMonitorClass;

struct _GVolumeMonitorClass {
	GObjectClass parent_class;

	/*< public >*/
	/* signals */
	void (* volume_mounted)	  	(GVolumeMonitor *volume_monitor,
				   	 GVolume        *volume);
	void (* volume_pre_unmount)	(GVolumeMonitor *volume_monitor,
				   	 GVolume	*volume);
	void (* volume_unmounted)	(GVolumeMonitor *volume_monitor,
				   	 GVolume        *volume);
	void (* drive_connected) 	(GVolumeMonitor *volume_monitor,
				   	 GDrive	        *drive);
	void (* drive_disconnected)	(GVolumeMonitor *volume_monitor,
				   	 GDrive         *drive);
};

GType g_volume_monitor_get_type (void) G_GNUC_CONST;

GVolumeMonitor *g_get_volume_monitor                  (void);
GList *         g_volume_monitor_get_mounted_volumes  (GVolumeMonitor *volume_monitor);
GList *         g_volume_monitor_get_connected_drives (GVolumeMonitor *volume_monitor);

G_END_DECLS

#endif /* __G_VOLUME_MONITOR_H__ */
