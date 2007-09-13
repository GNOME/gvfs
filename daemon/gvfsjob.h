#ifndef __G_VFS_JOB_H__
#define __G_VFS_JOB_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_JOB         (g_vfs_job_get_type ())
#define G_VFS_JOB(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_JOB, GVfsJob))
#define G_VFS_JOB_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_JOB, GVfsJobClass))
#define G_IS_VFS_JOB(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_JOB))
#define G_IS_VFS_JOB_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_JOB))
#define G_VFS_JOB_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_JOB, GVfsJobClass))

typedef struct _GVfsJob        GVfsJob;
typedef struct _GVfsJobClass   GVfsJobClass;

/* Temp typedefs */
typedef GVfsJob GVfsJobOpenForRead;
typedef GVfsJob GVfsJobRead;
typedef GVfsJob GVfsJobReadSeek;

struct _GVfsJob
{
  GObject parent_instance;
  
  guint failed : 1;
  guint cancelled : 1;
  GError *error;
};

struct _GVfsJobClass
{
  GObjectClass parent_class;

  /* signals */
  void (*cancel) (GVfsJob *job);
  void (*finished) (GVfsJob *job);

  /* vtable */

  gboolean (*start) (GVfsJob *job);

};

GType g_vfs_job_get_type (void) G_GNUC_CONST;

void g_vfs_job_cancel (GVfsJob *job);
void g_vfs_job_set_failed (GVfsJob *job,
			   GError *error);

G_END_DECLS

#endif /* __G_VFS_JOB_H__ */
