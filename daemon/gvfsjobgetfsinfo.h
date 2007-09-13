#ifndef __G_VFS_JOB_GET_FS_INFO_H__
#define __G_VFS_JOB_GET_FS_INFO_H__

#include <gio/gfileinfo.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_GET_FS_INFO         (g_vfs_job_get_fs_info_get_type ())
#define G_VFS_JOB_GET_FS_INFO(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_GET_FS_INFO, GVfsJobGetFsInfo))
#define G_VFS_JOB_GET_FS_INFO_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_GET_FS_INFO, GVfsJobGetFsInfoClass))
#define G_VFS_IS_JOB_GET_FS_INFO(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_GET_FS_INFO))
#define G_VFS_IS_JOB_GET_FS_INFO_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_GET_FS_INFO))
#define G_VFS_JOB_GET_FS_INFO_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_GET_FS_INFO, GVfsJobGetFsInfoClass))

typedef struct _GVfsJobGetFsInfoClass   GVfsJobGetFsInfoClass;

struct _GVfsJobGetFsInfo
{
  GVfsJobDBus parent_instance;

  GVfsBackend *backend;
  char *filename;
  GFileAttributeMatcher *attribute_matcher;

  GFileInfo *file_info;
};

struct _GVfsJobGetFsInfoClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_get_fs_info_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_get_fs_info_new      (DBusConnection   *connection,
					 DBusMessage      *message,
					 GVfsBackend      *backend);

G_END_DECLS

#endif /* __G_VFS_JOB_GET_FS_INFO_H__ */
