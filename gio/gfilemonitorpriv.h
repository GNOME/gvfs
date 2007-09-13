#ifndef __G_FILE_MONITOR_PRIV_H__
#define __G_FILE_MONITOR_PRIV_H__

#include <gio/gfilemonitor.h>

G_BEGIN_DECLS

void g_file_monitor_emit_event (GFileMonitor      *monitor,
				GFile             *file,
				GFile             *other_file,
				GFileMonitorEvent  event_type);

G_END_DECLS

#endif /* __G_FILE_MONITOR_PRIV_H__ */
