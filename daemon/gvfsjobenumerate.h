#ifndef __G_VFS_JOB_ENUMERATE_H__
#define __G_VFS_JOB_ENUMERATE_H__

#include <gio/gfileinfo.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_ENUMERATE         (g_vfs_job_enumerate_get_type ())
#define G_VFS_JOB_ENUMERATE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_ENUMERATE, GVfsJobEnumerate))
#define G_VFS_JOB_ENUMERATE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_ENUMERATE, GVfsJobEnumerateClass))
#define G_VFS_IS_JOB_ENUMERATE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_ENUMERATE))
#define G_VFS_IS_JOB_ENUMERATE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_ENUMERATE))
#define G_VFS_JOB_ENUMERATE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_ENUMERATE, GVfsJobEnumerateClass))

typedef struct _GVfsJobEnumerateClass   GVfsJobEnumerateClass;

struct _GVfsJobEnumerate
{
  GVfsJobDBus parent_instance;

  GVfsBackend *backend;
  char *filename;
  char *object_path;
  GFileAttributeMatcher *attribute_matcher;
  GFileQueryInfoFlags flags;

  DBusMessage *building_infos;
  DBusMessageIter building_iter;
  DBusMessageIter building_array_iter;
  int n_building_infos;
};

struct _GVfsJobEnumerateClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_enumerate_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_enumerate_new        (DBusConnection        *connection,
					 DBusMessage           *message,
					 GVfsBackend           *backend);
void     g_vfs_job_enumerate_add_info   (GVfsJobEnumerate      *job,
					 GFileInfo             *info);
void     g_vfs_job_enumerate_add_infos  (GVfsJobEnumerate      *job,
					 GList                 *info);
void     g_vfs_job_enumerate_done       (GVfsJobEnumerate      *job);

G_END_DECLS

#endif /* __G_VFS_JOB_ENUMERATE_H__ */
