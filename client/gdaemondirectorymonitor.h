#ifndef __G_DAEMON_DIRECTORY_MONITOR_H__
#define __G_DAEMON_DIRECTORY_MONITOR_H__

#include <glib-object.h>
#include <gio/gdirectorymonitor.h>

G_BEGIN_DECLS

#define G_TYPE_DAEMON_DIRECTORY_MONITOR		(g_daemon_directory_monitor_get_type ())
#define G_DAEMON_DIRECTORY_MONITOR(o)		(G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DAEMON_DIRECTORY_MONITOR, GDaemonDirectoryMonitor))
#define G_DAEMON_DIRECTORY_MONITOR_CLASS(k)	(G_TYPE_CHECK_CLASS_CAST ((k), G_TYPE_DAEMON_DIRECTORY_MONITOR, GDaemonDirectoryMonitorClass))
#define G_IS_DAEMON_DIRECTORY_MONITOR(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DAEMON_DIRECTORY_MONITOR))
#define G_IS_DAEMON_DIRECTORY_MONITOR_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DAEMON_DIRECTORY_MONITOR))

typedef struct _GDaemonDirectoryMonitor      GDaemonDirectoryMonitor;
typedef struct _GDaemonDirectoryMonitorClass GDaemonDirectoryMonitorClass;

struct _GDaemonDirectoryMonitorClass {
  GDirectoryMonitorClass parent_class;
};

GType g_daemon_directory_monitor_get_type (void) G_GNUC_CONST;

GDirectoryMonitor* g_daemon_directory_monitor_new (const char *remote_id,
						   const char *remote_obj_path);


G_END_DECLS

#endif /* __G_DAEMON_DIRECTORY_MONITOR_H__ */
