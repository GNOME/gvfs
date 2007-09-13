#ifndef __G_VFS_JOB_TRASH_H__
#define __G_VFS_JOB_TRASH_H__

#include <gio/gfileinfo.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_TRASH         (g_vfs_job_trash_get_type ())
#define G_VFS_JOB_TRASH(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_TRASH, GVfsJobTrash))
#define G_VFS_JOB_TRASH_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_TRASH, GVfsJobTrashClass))
#define G_VFS_IS_JOB_TRASH(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_TRASH))
#define G_VFS_IS_JOB_TRASH_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_TRASH))
#define G_VFS_JOB_TRASH_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_TRASH, GVfsJobTrashClass))

typedef struct _GVfsJobTrashClass   GVfsJobTrashClass;

struct _GVfsJobTrash
{
  GVfsJobDBus parent_instance;

  GVfsBackend *backend;
  char *filename;
};

struct _GVfsJobTrashClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_trash_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_trash_new (DBusConnection *connection,
			       DBusMessage    *message,
			       GVfsBackend    *backend);

G_END_DECLS

#endif /* __G_VFS_JOB_TRASH_H__ */
