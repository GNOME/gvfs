#ifndef __G_DAEMON_FILE_MONITOR_H__
#define __G_DAEMON_FILE_MONITOR_H__

#include <glib-object.h>
#include <gio/gfilemonitor.h>

G_BEGIN_DECLS

#define G_TYPE_DAEMON_FILE_MONITOR		(g_daemon_file_monitor_get_type ())
#define G_DAEMON_FILE_MONITOR(o)		(G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DAEMON_FILE_MONITOR, GDaemonFileMonitor))
#define G_DAEMON_FILE_MONITOR_CLASS(k)		(G_TYPE_CHECK_CLASS_CAST ((k), G_TYPE_DAEMON_FILE_MONITOR, GDaemonFileMonitorClass))
#define G_IS_DAEMON_FILE_MONITOR(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DAEMON_FILE_MONITOR))
#define G_IS_DAEMON_FILE_MONITOR_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DAEMON_FILE_MONITOR))

typedef struct _GDaemonFileMonitor      GDaemonFileMonitor;
typedef struct _GDaemonFileMonitorClass GDaemonFileMonitorClass;

struct _GDaemonFileMonitorClass {
  GFileMonitorClass parent_class;
};

GType g_daemon_file_monitor_get_type (void) G_GNUC_CONST;

GFileMonitor* g_daemon_file_monitor_new                  (void);
char  *       g_daemon_directory_monitor_get_object_path (GDaemonDirectoryMonitor *monitor);

G_END_DECLS

#endif /* __G_DAEMON_FILE_MONITOR_H__ */
