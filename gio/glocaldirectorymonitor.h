#ifndef __G_LOCAL_DIRECTORY_MONITOR_H__
#define __G_LOCAL_DIRECTORY_MONITOR_H__

#include <glib-object.h>
#include <gio/gdirectorymonitor.h>

G_BEGIN_DECLS

#define G_TYPE_LOCAL_DIRECTORY_MONITOR		(g_local_directory_monitor_get_type ())
#define G_LOCAL_DIRECTORY_MONITOR(o)		(G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_LOCAL_DIRECTORY_MONITOR, GLocalDirectoryMonitor))
#define G_LOCAL_DIRECTORY_MONITOR_CLASS(k)	(G_TYPE_CHECK_CLASS_CAST ((k), G_TYPE_LOCAL_DIRECTORY_MONITOR, GLocalDirectoryMonitorClass))
#define G_IS_LOCAL_DIRECTORY_MONITOR(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_LOCAL_DIRECTORY_MONITOR))
#define G_IS_LOCAL_DIRECTORY_MONITOR_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_LOCAL_DIRECTORY_MONITOR))

typedef struct _GLocalDirectoryMonitor      GLocalDirectoryMonitor;
typedef struct _GLocalDirectoryMonitorClass GLocalDirectoryMonitorClass;

struct _GLocalDirectoryMonitorClass {
  GDirectoryMonitorClass parent_class;
};

GType g_local_directory_monitor_get_type (void) G_GNUC_CONST;

GDirectoryMonitor* g_local_directory_monitor_start (const char* dirname);

G_END_DECLS

#endif /* __G_LOCAL_DIRECTORY_MONITOR_H__ */
