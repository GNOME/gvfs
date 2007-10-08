#ifndef __G_VFS_JOB_CREATE_MONITOR_H__
#define __G_VFS_JOB_CREATE_MONITOR_H__

#include <gio/gfileinfo.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_CREATE_MONITOR         (g_vfs_job_create_monitor_get_type ())
#define G_VFS_JOB_CREATE_MONITOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_CREATE_MONITOR, GVfsJobCreateMonitor))
#define G_VFS_JOB_CREATE_MONITOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_CREATE_MONITOR, GVfsJobCreateMonitorClass))
#define G_VFS_IS_JOB_CREATE_MONITOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_CREATE_MONITOR))
#define G_VFS_IS_JOB_CREATE_MONITOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_CREATE_MONITOR))
#define G_VFS_JOB_CREATE_MONITOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_CREATE_MONITOR, GVfsJobCreateMonitorClass))

typedef struct _GVfsJobCreateMonitorClass   GVfsJobCreateMonitorClass;

struct _GVfsJobCreateMonitor
{
  GVfsJobDBus parent_instance;

  gboolean is_directory;
  GVfsBackend *backend;
  char *filename;
  GFileMonitorFlags flags;
  
  char *object_path;
};

struct _GVfsJobCreateMonitorClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_create_monitor_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_create_monitor_new          (DBusConnection       *connection,
						DBusMessage          *message,
						GVfsBackend          *backend,
						gboolean              is_directory);
void     g_vfs_job_create_monitor_set_obj_path (GVfsJobCreateMonitor *job,
						const char           *object_path);

G_END_DECLS

#endif /* __G_VFS_JOB_CREATE_MONITOR_H__ */
