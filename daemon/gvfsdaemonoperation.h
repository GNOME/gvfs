#ifndef __G_VFS_DAEMON_OPERATION_H__
#define __G_VFS_DAEMON_OPERATION_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_DAEMON_OPERATION         (g_vfs_daemon_operation_get_type ())
#define G_VFS_DAEMON_OPERATION(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_DAEMON_OPERATION, GVfsDaemonOperation))
#define G_VFS_DAEMON_OPERATION_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_DAEMON_OPERATION, GVfsDaemonOperationClass))
#define G_IS_VFS_DAEMON_OPERATION(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_DAEMON_OPERATION))
#define G_IS_VFS_DAEMON_OPERATION_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_DAEMON_OPERATION))
#define G_VFS_DAEMON_OPERATION_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_DAEMON_OPERATION, GVfsDaemonOperationClass))

typedef struct _GVfsDaemonOperation        GVfsDaemonOperation;
typedef struct _GVfsDaemonOperationClass   GVfsDaemonOperationClass;

/* Temp typedefs */
typedef GVfsDaemonOperation GVfsDaemonOperationOpenForRead;
typedef GVfsDaemonOperation GVfsDaemonOperationRead;
typedef GVfsDaemonOperation GVfsDaemonOperationReadSeek;

struct _GVfsDaemonOperation
{
  GObject parent_instance;
  
  guint failed : 1;
  guint cancelled : 1;
  GError *error;
};

struct _GVfsDaemonOperationClass
{
  GObjectClass parent_class;

  /* signals */
  void (*cancel) (GVfsDaemonOperation *op);
  void (*finished) (GVfsDaemonOperation *op);

  /* vtable */

  gboolean (*start) (GVfsDaemonOperation *op);

};

GType g_vfs_daemon_operation_get_type (void) G_GNUC_CONST;

void g_vfs_daemon_operation_cancel (GVfsDaemonOperation *op);
void g_vfs_daemon_operation_set_failed (GVfsDaemonOperation *op,
					GError *error);

G_END_DECLS

#endif /* __G_VFS_DAEMON_OPERATION_H__ */
