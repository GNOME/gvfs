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
typedef struct _GVfsJobPrivate GVfsJobPrivate;
typedef struct _GVfsJobClass   GVfsJobClass;

/* Defined here to avoid circular includes */
typedef struct _GVfsJobSource GVfsJobSource;

struct _GVfsJob
{
  GObject parent_instance;

  /* TODO: Move stuff to private */
  gpointer backend_data;
  guint failed : 1;
  guint cancelled : 1;
  guint sending_reply : 1;
  guint finished : 1;
  GError *error;
  
  GVfsJobPrivate *priv;
};

struct _GVfsJobClass
{
  GObjectClass parent_class;

  /* signals */
  void (*cancelled)  (GVfsJob *job);
  void (*send_reply) (GVfsJob *job);
  void (*new_source) (GVfsJob *job,
		      GVfsJobSource *job_source);
  void (*finished)   (GVfsJob *job);

  /* vtable */

  void     (*run)    (GVfsJob *job);
  gboolean (*try)    (GVfsJob *job);
};

GType g_vfs_job_get_type (void) G_GNUC_CONST;

gboolean g_vfs_job_is_finished       (GVfsJob     *job);
gboolean g_vfs_job_is_cancelled      (GVfsJob     *job);
void     g_vfs_job_cancel            (GVfsJob     *job);
void     g_vfs_job_run               (GVfsJob     *job);
gboolean g_vfs_job_try               (GVfsJob     *job);
void     g_vfs_job_emit_finished     (GVfsJob     *job);
void     g_vfs_job_failed            (GVfsJob     *job,
				      GQuark       domain,
				      gint         code,
				      const gchar *format,
				      ...) G_GNUC_PRINTF (4, 5);
void     g_vfs_job_failed_from_error (GVfsJob     *job,
				      GError      *error);
void     g_vfs_job_succeeded         (GVfsJob     *job);

G_END_DECLS

#endif /* __G_VFS_JOB_H__ */
