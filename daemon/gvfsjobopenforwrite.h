#ifndef __G_VFS_JOB_OPEN_FOR_WRITE_H__
#define __G_VFS_JOB_OPEN_FOR_WRITE_H__

#include <dbus/dbus.h>
#include <gvfsjobdbus.h>
#include <gvfsbackend.h>
#include <gvfswritechannel.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_OPEN_FOR_WRITE         (g_vfs_job_open_for_write_get_type ())
#define G_VFS_JOB_OPEN_FOR_WRITE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_OPEN_FOR_WRITE, GVfsJobOpenForWrite))
#define G_VFS_JOB_OPEN_FOR_WRITE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_OPEN_FOR_WRITE, GVfsJobOpenForWriteClass))
#define G_IS_VFS_JOB_OPEN_FOR_WRITE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_OPEN_FOR_WRITE))
#define G_IS_VFS_JOB_OPEN_FOR_WRITE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_OPEN_FOR_WRITE))
#define G_VFS_JOB_OPEN_FOR_WRITE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_OPEN_FOR_WRITE, GVfsJobOpenForWriteClass))

typedef struct _GVfsJobOpenForWriteClass   GVfsJobOpenForWriteClass;

typedef enum {
  OPEN_FOR_WRITE_CREATE = 0,
  OPEN_FOR_WRITE_APPEND = 1,
  OPEN_FOR_WRITE_REPLACE = 2,
} GVfsJobOpenForWriteMode;

struct _GVfsJobOpenForWrite
{
  GVfsJobDBus parent_instance;

  GVfsJobOpenForWriteMode mode;
  char *filename;
  time_t mtime;
  gboolean make_backup;
  
  GVfsBackend *backend;
  GVfsBackendHandle backend_handle;

  gboolean can_seek;
  goffset initial_offset;
  GVfsWriteChannel *write_channel;
};

struct _GVfsJobOpenForWriteClass
{
  GVfsJobDBusClass parent_class;
};

GType g_vfs_job_open_for_write_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_open_for_write_new                (DBusConnection      *connection,
						      DBusMessage         *message,
						      GVfsBackend         *backend);
void     g_vfs_job_open_for_write_set_handle         (GVfsJobOpenForWrite *job,
						      GVfsBackendHandle    handle);
void     g_vfs_job_open_for_write_set_can_seek       (GVfsJobOpenForWrite *job,
						      gboolean             can_seek);
void     g_vfs_job_open_for_write_set_initial_offset (GVfsJobOpenForWrite *job,
						      goffset              initial_offset);

G_END_DECLS

#endif /* __G_VFS_JOB_OPEN_FOR_WRITE_H__ */
