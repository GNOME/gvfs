#ifndef __G_VFS_JOB_GET_INFO_H__
#define __G_VFS_JOB_GET_INFO_H__

#include <gio/gfileinfo.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_GET_INFO         (g_vfs_job_get_info_get_type ())
#define G_VFS_JOB_GET_INFO(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_GET_INFO, GVfsJobGetInfo))
#define G_VFS_JOB_GET_INFO_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_GET_INFO, GVfsJobGetInfoClass))
#define G_VFS_IS_JOB_GET_INFO(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_GET_INFO))
#define G_VFS_IS_JOB_GET_INFO_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_GET_INFO))
#define G_VFS_JOB_GET_INFO_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_GET_INFO, GVfsJobGetInfoClass))

typedef struct _GVfsJobGetInfoClass   GVfsJobGetInfoClass;

struct _GVfsJobGetInfo
{
  GVfsJobDBus parent_instance;

  GVfsBackend *backend;
  char *filename;
  GFileInfoRequestFlags requested;
  char *attributes;
  gboolean follow_symlinks;

  GFileInfoRequestFlags requested_result;
  GFileInfo *file_info;
};

struct _GVfsJobGetInfoClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_get_info_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_get_info_new      (DBusConnection        *connection,
				      DBusMessage           *message,
				      GVfsBackend           *backend);
void     g_vfs_job_get_info_set_info (GVfsJobGetInfo        *job,
				      GFileInfoRequestFlags  requested_result,
				      GFileInfo             *file_info);

G_END_DECLS

#endif /* __G_VFS_JOB_GET_INFO_H__ */
