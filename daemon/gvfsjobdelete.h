#ifndef __G_VFS_JOB_DELETE_H__
#define __G_VFS_JOB_DELETE_H__

#include <gio/gfileinfo.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_DELETE         (g_vfs_job_delete_get_type ())
#define G_VFS_JOB_DELETE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_DELETE, GVfsJobDelete))
#define G_VFS_JOB_DELETE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_DELETE, GVfsJobDeleteClass))
#define G_VFS_IS_JOB_DELETE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_DELETE))
#define G_VFS_IS_JOB_DELETE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_DELETE))
#define G_VFS_JOB_DELETE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_DELETE, GVfsJobDeleteClass))

typedef struct _GVfsJobDeleteClass   GVfsJobDeleteClass;

struct _GVfsJobDelete
{
  GVfsJobDBus parent_instance;

  GVfsBackend *backend;
  char *filename;
};

struct _GVfsJobDeleteClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_delete_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_delete_new (DBusConnection *connection,
			       DBusMessage    *message,
			       GVfsBackend    *backend);

G_END_DECLS

#endif /* __G_VFS_JOB_DELETE_H__ */
