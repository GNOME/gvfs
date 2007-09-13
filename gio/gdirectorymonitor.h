#ifndef __G_DIRECTORY_MONITOR_H__
#define __G_DIRECTORY_MONITOR_H__

#include <glib-object.h>
#include <gio/giotypes.h>
#include <gio/gfile.h>

G_BEGIN_DECLS

#define G_TYPE_DIRECTORY_MONITOR         (g_directory_monitor_get_type ())
#define G_DIRECTORY_MONITOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DIRECTORY_MONITOR, GDirectoryMonitor))
#define G_DIRECTORY_MONITOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_DIRECTORY_MONITOR, GDirectoryMonitorClass))
#define G_IS_DIRECTORY_MONITOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DIRECTORY_MONITOR))
#define G_IS_DIRECTORY_MONITOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DIRECTORY_MONITOR))
#define G_DIRECTORY_MONITOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_DIRECTORY_MONITOR, GDirectoryMonitorClass))

typedef enum {
  G_DIRECTORY_MONITOR_EVENT_CHANGED,
  G_DIRECTORY_MONITOR_EVENT_DELETED,
  G_DIRECTORY_MONITOR_EVENT_CREATED,
  G_DIRECTORY_MONITOR_EVENT_ATTRIBUTE_CHANGED,
  G_DIRECTORY_MONITOR_EVENT_UNMOUNTED
} GDirectoryMonitorEvent;

typedef struct _GDirectoryMonitorClass	 GDirectoryMonitorClass;
typedef struct _GDirectoryMonitorPrivate GDirectoryMonitorPrivate;

struct _GDirectoryMonitor
{
  GObject parent;

  /*< private >*/
  GDirectoryMonitorPrivate *priv;
};

struct _GDirectoryMonitorClass
{
  GObjectClass parent_class;
  
  /* Signals */
  void (* changed) (GDirectoryMonitor* monitor,
		    GFile* child,
		    GFile* other_file,
		    GDirectoryMonitorEvent event_type);
  
  /* Virtual Table */
  gboolean	(*cancel)(GDirectoryMonitor* monitor);
};

GType g_directory_monitor_get_type (void) G_GNUC_CONST;

gboolean g_directory_monitor_cancel         (GDirectoryMonitor *monitor);
void     g_directory_monitor_set_rate_limit (GDirectoryMonitor *monitor,
					     int                limit_msecs);

G_END_DECLS

#endif /* __G_DIRECTORY_MONITOR_H__ */
