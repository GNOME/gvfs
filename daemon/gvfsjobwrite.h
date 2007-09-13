#ifndef __G_VFS_JOB_WRITE_H__
#define __G_VFS_JOB_WRITE_H__

#include <gvfsjob.h>
#include <gvfsbackend.h>
#include <gvfswritechannel.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_WRITE         (g_vfs_job_write_get_type ())
#define G_VFS_JOB_WRITE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_JOB_WRITE, GVfsJobWrite))
#define G_VFS_JOB_WRITE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_JOB_WRITE, GVfsJobWriteClass))
#define G_VFS_IS_JOB_WRITE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_JOB_WRITE))
#define G_VFS_IS_JOB_WRITE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_JOB_WRITE))
#define G_VFS_JOB_WRITE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_JOB_WRITE, GVfsJobWriteClass))

typedef struct _GVfsJobWriteClass   GVfsJobWriteClass;

struct _GVfsJobWrite
{
  GVfsJob parent_instance;

  GVfsWriteChannel *channel;
  GVfsBackend *backend;
  GVfsBackendHandle handle;
  char *data;
  gsize data_size;
  
  gsize written_size;
};

struct _GVfsJobWriteClass
{
  GVfsJobClass parent_class;
};

GType g_vfs_job_write_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_write_new              (GVfsWriteChannel  *channel,
					   GVfsBackendHandle  handle,
					   char              *data,
					   gsize              data_size,
					   GVfsBackend       *backend);
void     g_vfs_job_write_set_written_size (GVfsJobWrite      *job,
					   gsize              written_size);

G_END_DECLS

#endif /* __G_VFS_JOB_WRITE_H__ */
