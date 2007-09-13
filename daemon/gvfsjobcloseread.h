#ifndef __G_VFS_JOB_CLOSE_READ_H__
#define __G_VFS_JOB_CLOSE_READ_H__

#include <gvfsjob.h>
#include <gvfsbackend.h>
#include <gvfsreadstream.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_JOB_CLOSE_READ         (g_vfs_job_close_read_get_type ())
#define G_VFS_JOB_CLOSE_READ(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_JOB_CLOSE_READ, GVfsJobCloseRead))
#define G_VFS_JOB_CLOSE_READ_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_JOB_CLOSE_READ, GVfsJobCloseReadClass))
#define G_IS_VFS_JOB_CLOSE_READ(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_JOB_CLOSE_READ))
#define G_IS_VFS_JOB_CLOSE_READ_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_JOB_CLOSE_READ))
#define G_VFS_JOB_CLOSE_READ_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_JOB_CLOSE_READ, GVfsJobCloseReadClass))

/* GVfsJobCloseRead declared in gvfsjob.h */
typedef struct _GVfsJobCloseReadClass   GVfsJobCloseReadClass;

struct _GVfsJobCloseRead
{
  GVfsJob parent_instance;

  GVfsReadStream *stream;
  GVfsBackendHandle handle;
};

struct _GVfsJobCloseReadClass
{
  GVfsJobClass parent_class;
};

GType g_vfs_job_close_read_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_close_read_new (GVfsReadStream    *stream,
				   GVfsBackendHandle  handle,
				   GVfsBackend       *backend);

G_END_DECLS

#endif /* __G_VFS_JOB_CLOSE_READ_H__ */
