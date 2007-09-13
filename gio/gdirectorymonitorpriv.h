#ifndef __G_DIRECTORY_MONITOR_PRIV_H__
#define __G_DIRECTORY_MONITOR_PRIV_H__

#include <gio/gdirectorymonitor.h>

G_BEGIN_DECLS

void g_directory_monitor_emit_event (GDirectoryMonitor      *monitor,
				     GFile                  *parent,
				     GFile                  *child,
				     GFile                  *other_file,
				     GDirectoryMonitorEvent  event_type);

G_END_DECLS

#endif /* __G_DIRECTORY_MONITOR_PRIV_H__ */
