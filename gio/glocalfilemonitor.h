#ifndef __G_LOCAL_FILE_MONITOR_H__
#define __G_LOCAL_FILE_MONITOR_H__

#include <glib-object.h>
#include <gio/gfilemonitor.h>

G_BEGIN_DECLS

#define G_TYPE_LOCAL_FILE_MONITOR		(g_local_file_monitor_get_type ())
#define G_LOCAL_FILE_MONITOR(o)			(G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_LOCAL_FILE_MONITOR, GLocalFileMonitor))
#define G_LOCAL_FILE_MONITOR_CLASS(k)		(G_TYPE_CHECK_CLASS_CAST ((k), G_TYPE_LOCAL_FILE_MONITOR, GLocalFileMonitorClass))
#define G_IS_LOCAL_FILE_MONITOR(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_LOCAL_FILE_MONITOR))
#define G_IS_LOCAL_FILE_MONITOR_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_LOCAL_FILE_MONITOR))

typedef struct _GLocalFileMonitor      GLocalFileMonitor;
typedef struct _GLocalFileMonitorClass GLocalFileMonitorClass;

struct _GLocalFileMonitorClass {
  GObjectClass parent_class;
};

GType g_local_file_monitor_get_type (void) G_GNUC_CONST;

GFileMonitor* g_local_file_monitor_start (const char* dirname);

G_END_DECLS

#endif /* __G_LOCAL_FILE_MONITOR_H__ */
