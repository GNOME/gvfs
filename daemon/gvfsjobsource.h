#ifndef __G_VFS_JOB_SOURCE_H__
#define __G_VFS_JOB_SOURCE_H__

#include <glib-object.h>
#include <gvfsjob.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_JOB_SOURCE            (g_vfs_job_source_get_type ())
#define G_VFS_JOB_SOURCE(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_VFS_TYPE_JOB_SOURCE, GVfsJobSource))
#define G_IS_VFS_JOB_SOURCE(obj)	 (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_VFS_TYPE_JOB_SOURCE))
#define G_VFS_JOB_SOURCE_GET_IFACE(obj)  (G_TYPE_INSTANCE_GET_INTERFACE ((obj), G_VFS_TYPE_JOB_SOURCE, GVfsJobSourceIface))

/* GVfsJobSource defined in gvfsjob.h */
typedef struct _GVfsJobSourceIface    GVfsJobSourceIface;

struct _GVfsJobSourceIface
{
  GTypeInterface g_iface;

  /* Virtual Table: */

  /* Signals: */

  void (*new_job) (GVfsJobSource *source,
		   GVfsJob *job);
  void (*closed)  (GVfsJobSource *source);

};

GType g_vfs_job_source_get_type (void) G_GNUC_CONST;

void g_vfs_job_source_new_job (GVfsJobSource *job_source,
			       GVfsJob       *job);
void g_vfs_job_source_closed  (GVfsJobSource *job_source);


G_END_DECLS

#endif /* __G_VFS_JOB_SOURCE_H__ */
