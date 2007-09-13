#ifndef __G_FILE_MONITOR_H__
#define __G_FILE_MONITOR_H__

#include <glib-object.h>
#include <gio/giotypes.h>
#include <gio/gfile.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_MONITOR		(g_file_monitor_get_type())
#define G_FILE_MONITOR(obj)		(G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_FILE_MONITOR, GFileMonitor))
#define G_IS_FILE_MONITOR(obj)		(G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_FILE_MONITOR))
#define G_FILE_MONITOR_GET_IFACE(obj)	(G_TYPE_INSTANCE_GET_INTERFACE ((obj), G_TYPE_FILE_MONITOR, GFileMonitorIface))

typedef enum {
  G_FILE_MONITOR_EVENT_CHANGED           = (1<<0),
  G_FILE_MONITOR_EVENT_DELETED           = (1<<1),
  G_FILE_MONITOR_EVENT_CREATED           = (1<<2),
  G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED = (1<<3),
  G_FILE_MONITOR_EVENT_UNMOUNTED         = (1<<4)
} GFileMonitorEventFlags;

typedef struct _GFileMonitorIface	GFileMonitorIface;

struct _GFileMonitorIface
{
  GTypeInterface g_iface;
  
  /* Signals */
  void (* change) (GFileMonitor* monitor,
		   GFile* file,
		   GFileMonitorEventFlags change_flags);
  
  /* Virtual Table */
  gboolean	(*cancel)(GFileMonitor* monitor);
};

GType g_file_monitor_get_type (void) G_GNUC_CONST;

gboolean g_file_monitor_cancel (GFileMonitor *monitor);

G_END_DECLS

#endif /* __G_FILE_MONITOR_H__ */
