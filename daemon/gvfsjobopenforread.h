#ifndef __G_VFS_JOB_OPEN_FOR_READ_H__
#define __G_VFS_JOB_OPEN_FOR_READ_H__

#include <dbus/dbus.h>
#include <gvfsjob.h>
#include <gvfsdaemonbackend.h>
#include <gvfsreadstream.h>


G_BEGIN_DECLS

#define G_TYPE_VFS_JOB_OPEN_FOR_READ         (g_vfs_job_open_for_read_get_type ())
#define G_VFS_JOB_OPEN_FOR_READ(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_JOB_OPEN_FOR_READ, GVfsJobOpenForRead))
#define G_VFS_JOB_OPEN_FOR_READ_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_JOB_OPEN_FOR_READ, GVfsJobOpenForReadClass))
#define G_IS_VFS_JOB_OPEN_FOR_READ(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_JOB_OPEN_FOR_READ))
#define G_IS_VFS_JOB_OPEN_FOR_READ_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_JOB_OPEN_FOR_READ))
#define G_VFS_JOB_OPEN_FOR_READ_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_JOB_OPEN_FOR_READ, GVfsJobOpenForReadClass))

/* GVfsJobOpenForRead declared in gvfsjob.h */
typedef struct _GVfsJobOpenForReadClass   GVfsJobOpenForReadClass;

struct _GVfsJobOpenForRead
{
  GVfsJob parent_instance;

  DBusConnection *connection;
  DBusMessage *message;
  char *filename;
  GVfsHandle *backend_handle;
  GVfsReadStream *read_stream;
};

struct _GVfsJobOpenForReadClass
{
  GVfsJobClass parent_class;
};

GType g_vfs_job_open_for_read_get_type (void) G_GNUC_CONST;

GVfsJob *       g_vfs_job_open_for_read_new          (GVfsDaemonBackend  *backend,
						      DBusConnection     *connection,
						      DBusMessage        *message);
void            g_vfs_job_open_for_read_set_handle   (GVfsJobOpenForRead *job,
						      GVfsHandle         *handle);
GVfsReadStream *g_vfs_job_open_for_read_steal_stream (GVfsJobOpenForRead *job);

G_END_DECLS

#endif /* __G_VFS_JOB_OPEN_FOR_READ_H__ */
