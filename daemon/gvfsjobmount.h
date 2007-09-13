#ifndef __G_VFS_JOB_MOUNT_H__
#define __G_VFS_JOB_MOUNT_H__

#include <gio/gfileinfo.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_MOUNT         (g_vfs_job_mount_get_type ())
#define G_VFS_JOB_MOUNT(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_MOUNT, GVfsJobMount))
#define G_VFS_JOB_MOUNT_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_MOUNT, GVfsJobMountClass))
#define G_VFS_IS_JOB_MOUNT(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_MOUNT))
#define G_VFS_IS_JOB_MOUNT_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_MOUNT))
#define G_VFS_JOB_MOUNT_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_MOUNT, GVfsJobMountClass))

typedef struct _GVfsJobMountClass   GVfsJobMountClass;

struct _GVfsJobMount
{
  GVfsJob parent_instance;

  GVfsBackend *backend;
  GMountSpec *mount_spec;
  GMountSource *mount_source;
};

struct _GVfsJobMountClass
{
  GVfsJobClass parent_class;
};

GType g_vfs_job_mount_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_mount_new (GMountSpec  *spec,
			      GMountSource *source,
			      GVfsBackend *backend);

G_END_DECLS

#endif /* __G_VFS_JOB_MOUNT_H__ */
