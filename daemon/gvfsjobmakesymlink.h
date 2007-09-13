#ifndef __G_VFS_JOB_MAKE_SYMLINK_H__
#define __G_VFS_JOB_MAKE_SYMLINK_H__

#include <gio/gfileinfo.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_MAKE_SYMLINK         (g_vfs_job_make_symlink_get_type ())
#define G_VFS_JOB_MAKE_SYMLINK(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_MAKE_SYMLINK, GVfsJobMakeSymlink))
#define G_VFS_JOB_MAKE_SYMLINK_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_MAKE_SYMLINK, GVfsJobMakeSymlinkClass))
#define G_VFS_IS_JOB_MAKE_SYMLINK(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_MAKE_SYMLINK))
#define G_VFS_IS_JOB_MAKE_SYMLINK_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_MAKE_SYMLINK))
#define G_VFS_JOB_MAKE_SYMLINK_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_MAKE_SYMLINK, GVfsJobMakeSymlinkClass))

typedef struct _GVfsJobMakeSymlinkClass   GVfsJobMakeSymlinkClass;

struct _GVfsJobMakeSymlink
{
  GVfsJobDBus parent_instance;

  GVfsBackend *backend;
  char *filename;
  char *symlink_value;
};

struct _GVfsJobMakeSymlinkClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_make_symlink_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_make_symlink_new (DBusConnection *connection,
				     DBusMessage    *message,
				     GVfsBackend    *backend);

G_END_DECLS

#endif /* __G_VFS_JOB_MAKE_SYMLINK_H__ */
