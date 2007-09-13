#ifndef __G_FILE_MONITOR_H__
#define __G_FILE_MONITOR_H__

#include <glib-object.h>
#include <gio/giotypes.h>
#include <gio/gfile.h>

G_BEGIN_DECLS

#define G_TYPE_FILE_MONITOR         (g_file_monitor_get_type ())
#define G_FILE_MONITOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_FILE_MONITOR, GFileMonitor))
#define G_FILE_MONITOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_FILE_MONITOR, GFileMonitorClass))
#define G_IS_FILE_MONITOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_FILE_MONITOR))
#define G_IS_FILE_MONITOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_FILE_MONITOR))
#define G_FILE_MONITOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_FILE_MONITOR, GFileMonitorClass))

typedef enum {
  G_FILE_MONITOR_EVENT_CHANGED,
  G_FILE_MONITOR_EVENT_DELETED,
  G_FILE_MONITOR_EVENT_CREATED,
  G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED,
  G_FILE_MONITOR_EVENT_UNMOUNTED
} GFileMonitorEvent;

typedef struct _GFileMonitorClass	GFileMonitorClass;
typedef struct _GFileMonitorPrivate	GFileMonitorPrivate;

struct _GFileMonitor
{
  GObject parent;

  /*< private >*/
  GFileMonitorPrivate *priv;
};

struct _GFileMonitorClass
{
  GObjectClass parent_class;
  
  /* Signals */
  void (* changed) (GFileMonitor* monitor,
		    GFile* file,
		    GFile* other_file,
		    GFileMonitorEvent event_type);
  
  /* Virtual Table */
  gboolean	(*cancel)(GFileMonitor* monitor);
};

GType g_file_monitor_get_type (void) G_GNUC_CONST;

gboolean g_file_monitor_cancel         (GFileMonitor *monitor);
void     g_file_monitor_set_rate_limit (GFileMonitor *monitor,
					int           limit_msecs);

G_END_DECLS

#endif /* __G_FILE_MONITOR_H__ */
