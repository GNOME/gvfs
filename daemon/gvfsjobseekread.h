#ifndef __G_VFS_JOB_SEEK_READ_H__
#define __G_VFS_JOB_SEEK_READ_H__

#include <gvfsjob.h>
#include <gvfsbackend.h>
#include <gvfsreadchannel.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_JOB_SEEK_READ         (g_vfs_job_seek_read_get_type ())
#define G_VFS_JOB_SEEK_READ(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_JOB_SEEK_READ, GVfsJobSeekRead))
#define G_VFS_JOB_SEEK_READ_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_JOB_SEEK_READ, GVfsJobSeekReadClass))
#define G_IS_VFS_JOB_SEEK_READ(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_JOB_SEEK_READ))
#define G_IS_VFS_JOB_SEEK_READ_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_JOB_SEEK_READ))
#define G_VFS_JOB_SEEK_READ_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_JOB_SEEK_READ, GVfsJobSeekReadClass))

typedef struct _GVfsJobSeekReadClass   GVfsJobSeekReadClass;

struct _GVfsJobSeekRead
{
  GVfsJob parent_instance;

  GVfsReadChannel *channel;
  GVfsBackend *backend;
  GVfsBackendHandle handle;
  GSeekType seek_type;
  goffset requested_offset;
  goffset final_offset;
};

struct _GVfsJobSeekReadClass
{
  GVfsJobClass parent_class;
};

GType g_vfs_job_seek_read_get_type (void) G_GNUC_CONST;

GVfsJob *g_vfs_job_seek_read_new (GVfsReadChannel   *channel,
				  GVfsBackendHandle  handle,
				  GSeekType          seek_type,
				  goffset            offset,
				  GVfsBackend       *backend);

void g_vfs_job_seek_read_set_offset (GVfsJobSeekRead *job,
				     goffset offset);


G_END_DECLS

#endif /* __G_VFS_JOB_SEEK_READ_H__ */
