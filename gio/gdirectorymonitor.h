#ifndef __G_DIRECTORY_MONITOR_H__
#define __G_DIRECTORY_MONITOR_H__

#include <glib-object.h>
#include <gio/giotypes.h>
#include <gio/gfile.h>

G_BEGIN_DECLS

#define G_TYPE_DIRECTORY_MONITOR		(g_directory_monitor_get_type())
#define G_DIRECTORY_MONITOR(obj)		(G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_DIRECTORY_MONITOR, GDirectoryMonitor))
#define G_IS_DIRECTORY_MONITOR(obj)		(G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_DIRECTORY_MONITOR))
#define G_DIRECTORY_MONITOR_GET_IFACE(obj)	(G_TYPE_INSTANCE_GET_INTERFACE ((obj), G_TYPE_DIRECTORY_MONITOR, GDirectoryMonitorIface))

typedef enum {
  G_DIRECTORY_MONITOR_EVENT_CHANGED           = (1<<0),
  G_DIRECTORY_MONITOR_EVENT_DELETED           = (1<<1),
  G_DIRECTORY_MONITOR_EVENT_CREATED           = (1<<2),
  G_DIRECTORY_MONITOR_EVENT_ATTRIBUTE_CHANGED = (1<<3),
  G_DIRECTORY_MONITOR_EVENT_UNMOUNTED         = (1<<4)
} GDirectoryMonitorEventFlags;

typedef struct _GDirectoryMonitorIface	GDirectoryMonitorIface;

struct _GDirectoryMonitorIface
{
  GTypeInterface g_iface;
  
  /* Signals */
  void (* changed) (GDirectoryMonitor* monitor,
		    GFile* parent,
		    GFile* child,
		    GDirectoryMonitorEventFlags change_flags);
  
  /* Virtual Table */
  gboolean	(*cancel)(GDirectoryMonitor* monitor);
};

GType g_directory_monitor_get_type (void) G_GNUC_CONST;

gboolean g_directory_monitor_cancel (GDirectoryMonitor *monitor);

G_END_DECLS

#endif /* __G_DIRECTORY_MONITOR_H__ */
