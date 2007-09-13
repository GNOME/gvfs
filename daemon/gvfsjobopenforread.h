#ifndef __G_VFS_JOB_OPEN_FOR_READ_H__
#define __G_VFS_JOB_OPEN_FOR_READ_H__

#include <dbus/dbus.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>
#include <gvfsreadchannel.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_OPEN_FOR_READ         (g_vfs_job_open_for_read_get_type ())
#define G_VFS_JOB_OPEN_FOR_READ(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_OPEN_FOR_READ, GVfsJobOpenForRead))
#define G_VFS_JOB_OPEN_FOR_READ_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_OPEN_FOR_READ, GVfsJobOpenForReadClass))
#define G_VFS_IS_JOB_OPEN_FOR_READ(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_OPEN_FOR_READ))
#define G_VFS_IS_JOB_OPEN_FOR_READ_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_OPEN_FOR_READ))
#define G_VFS_JOB_OPEN_FOR_READ_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_OPEN_FOR_READ, GVfsJobOpenForReadClass))

typedef struct _GVfsJobOpenForReadClass   GVfsJobOpenForReadClass;

struct _GVfsJobOpenForRead
{
  GVfsJobDBus parent_instance;

  char *filename;
  GVfsBackend *backend;
  GVfsBackendHandle backend_handle;
  gboolean can_seek;
  GVfsReadChannel *read_channel;
};

struct _GVfsJobOpenForReadClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_open_for_read_get_type (void) G_GNUC_CONST;

GVfsJob *        g_vfs_job_open_for_read_new           (DBusConnection     *connection,
							DBusMessage        *message,
							GVfsBackend        *backend);
void             g_vfs_job_open_for_read_set_handle    (GVfsJobOpenForRead *job,
							GVfsBackendHandle   handle);
void             g_vfs_job_open_for_read_set_can_seek  (GVfsJobOpenForRead *job,
							gboolean            can_seek);

G_END_DECLS

#endif /* __G_VFS_JOB_OPEN_FOR_READ_H__ */
