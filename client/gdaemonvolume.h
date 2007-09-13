#ifndef __G_DAEMON_VOLUME_H__
#define __G_DAEMON_VOLUME_H__

#include <glib-object.h>
#include <gio/gvolume.h>
#if 0
# include <gio/gdaemonmounts.h>
#endif
#include "gdaemonvfs.h"
#include "gdaemonvolumemonitor.h"

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

GDaemonVolume *g_daemon_volume_new            (GVolumeMonitor *volume_monitor,
					       GMountRef *mount_info);

G_END_DECLS

#endif /* __G_DAEMON_VOLUME_H__ */
