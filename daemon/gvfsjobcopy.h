#ifndef __G_VFS_JOB_COPY_H__
#define __G_VFS_JOB_COPY_H__

#include <gio/gfileinfo.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_COPY         (g_vfs_job_copy_get_type ())
#define G_VFS_JOB_COPY(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_COPY, GVfsJobCopy))
#define G_VFS_JOB_COPY_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_COPY, GVfsJobCopyClass))
#define G_VFS_IS_JOB_COPY(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_COPY))
#define G_VFS_IS_JOB_COPY_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_COPY))
#define G_VFS_JOB_COPY_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_COPY, GVfsJobCopyClass))

typedef struct _GVfsJobCopyClass   GVfsJobCopyClass;

struct _GVfsJobCopy
{
  GVfsJobDBus parent_instance;

  GVfsBackend *backend;
  char *source;
  char *destination;
  GFileCopyFlags flags;
  char *callback_obj_path;
  
};

struct _GVfsJobCopyClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_copy_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_copy_new (DBusConnection *connection,
			     DBusMessage    *message,
			     GVfsBackend    *backend);

G_END_DECLS

#endif /* __G_VFS_JOB_COPY_H__ */
