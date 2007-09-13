#ifndef __G_VFS_JOB_SET_DISPLAY_NAME_H__
#define __G_VFS_JOB_SET_DISPLAY_NAME_H__

#include <gio/gfileinfo.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_SET_DISPLAY_NAME         (g_vfs_job_set_display_name_get_type ())
#define G_VFS_JOB_SET_DISPLAY_NAME(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_SET_DISPLAY_NAME, GVfsJobSetDisplayName))
#define G_VFS_JOB_SET_DISPLAY_NAME_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_SET_DISPLAY_NAME, GVfsJobSetDisplayNameClass))
#define G_VFS_IS_JOB_SET_DISPLAY_NAME(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_SET_DISPLAY_NAME))
#define G_VFS_IS_JOB_SET_DISPLAY_NAME_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_SET_DISPLAY_NAME))
#define G_VFS_JOB_SET_DISPLAY_NAME_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_SET_DISPLAY_NAME, GVfsJobSetDisplayNameClass))

typedef struct _GVfsJobSetDisplayNameClass   GVfsJobSetDisplayNameClass;

struct _GVfsJobSetDisplayName
{
  GVfsJobDBus parent_instance;

  GVfsBackend *backend;
  char *filename;
  char *display_name;

  char *new_path;
};

struct _GVfsJobSetDisplayNameClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_set_display_name_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_set_display_name_new          (DBusConnection        *connection,
						  DBusMessage           *message,
						  GVfsBackend           *backend);
void     g_vfs_job_set_display_name_set_new_path (GVfsJobSetDisplayName *job,
						  const char            *new_path);

G_END_DECLS

#endif /* __G_VFS_JOB_SET_DISPLAY_NAME_H__ */
