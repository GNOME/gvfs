#ifndef __G_VFS_JOB_MOUNT_MOUNTABLE_H__
#define __G_VFS_JOB_MOUNT_MOUNTABLE_H__

#include <gio/gfileinfo.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_MOUNT_MOUNTABLE         (g_vfs_job_mount_mountable_get_type ())
#define G_VFS_JOB_MOUNT_MOUNTABLE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_MOUNT_MOUNTABLE, GVfsJobMountMountable))
#define G_VFS_JOB_MOUNT_MOUNTABLE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_MOUNT_MOUNTABLE, GVfsJobMountMountableClass))
#define G_VFS_IS_JOB_MOUNT_MOUNTABLE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_MOUNT_MOUNTABLE))
#define G_VFS_IS_JOB_MOUNT_MOUNTABLE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_MOUNT_MOUNTABLE))
#define G_VFS_JOB_MOUNT_MOUNTABLE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_MOUNT_MOUNTABLE, GVfsJobMountMountableClass))

typedef struct _GVfsJobMountMountableClass   GVfsJobMountMountableClass;

struct _GVfsJobMountMountable
{
  GVfsJobDBus parent_instance;

  GVfsBackend *backend;
  char *filename;
  GMountSource *mount_source;
};

struct _GVfsJobMountMountableClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_mount_mountable_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_mount_mountable_new (DBusConnection        *connection,
					DBusMessage           *message,
					GVfsBackend           *backend);

G_END_DECLS

#endif /* __G_VFS_JOB_MOUNT_MOUNTABLE_H__ */
