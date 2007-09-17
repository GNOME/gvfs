#ifndef __G_VFS_JOB_SET_ATTRIBUTE_H__
#define __G_VFS_JOB_SET_ATTRIBUTE_H__

#include <gio/gfileinfo.h>
#include <gvfsjob.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_SET_ATTRIBUTE         (g_vfs_job_set_attribute_get_type ())
#define G_VFS_JOB_SET_ATTRIBUTE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_SET_ATTRIBUTE, GVfsJobSetAttribute))
#define G_VFS_JOB_SET_ATTRIBUTE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_SET_ATTRIBUTE, GVfsJobSetAttributeClass))
#define G_VFS_IS_JOB_SET_ATTRIBUTE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_SET_ATTRIBUTE))
#define G_VFS_IS_JOB_SET_ATTRIBUTE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_SET_ATTRIBUTE))
#define G_VFS_JOB_SET_ATTRIBUTE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_SET_ATTRIBUTE, GVfsJobSetAttributeClass))

typedef struct _GVfsJobSetAttributeClass   GVfsJobSetAttributeClass;

struct _GVfsJobSetAttribute
{
  GVfsJobDBus parent_instance;

  GVfsBackend *backend;

  char *filename;
  char *attribute;
  GFileAttributeType type;
  GFileAttributeValue value;
  GFileQueryInfoFlags flags;
};

struct _GVfsJobSetAttributeClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_set_attribute_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_set_attribute_new (DBusConnection *connection,
				      DBusMessage    *message,
				      GVfsBackend    *backend);

G_END_DECLS

#endif /* __G_VFS_JOB_SET_ATTRIBUTE_H__ */
