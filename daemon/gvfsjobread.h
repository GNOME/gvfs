#ifndef __G_VFS_JOB_READ_H__
#define __G_VFS_JOB_READ_H__

#include <gvfsjob.h>
#include <gvfsdaemonbackend.h>
#include <gvfsreadhandle.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_JOB_READ         (g_vfs_job_read_get_type ())
#define G_VFS_JOB_READ(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_JOB_READ, GVfsJobRead))
#define G_VFS_JOB_READ_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_JOB_READ, GVfsJobReadClass))
#define G_IS_VFS_JOB_READ(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_JOB_READ))
#define G_IS_VFS_JOB_READ_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_JOB_READ))
#define G_VFS_JOB_READ_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_JOB_READ, GVfsJobReadClass))

/* GVfsJobRead declared in gvfsjob.h */
typedef struct _GVfsJobReadClass   GVfsJobReadClass;

struct _GVfsJobRead
{
  GVfsJob parent_instance;

  GVfsReadHandle *handle;
  gsize bytes_requested;
  char *data;
  gsize data_count;
};

struct _GVfsJobReadClass
{
  GVfsJobClass parent_class;
};

GType g_vfs_job_read_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_read_new        (GVfsDaemonBackend *backend,
				    GVfsReadHandle    *handle,
				    gsize              bytes_requested);
void     g_vfs_job_read_set_result (GVfsJobRead       *job,
				    char              *data,
				    gsize              data_size);

G_END_DECLS

#endif /* __G_VFS_JOB_READ_H__ */
