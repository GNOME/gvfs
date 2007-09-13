#ifndef __G_VFS_DAEMON_BACKEND_H__
#define __G_VFS_DAEMON_BACKEND_H__

#include <gvfsdaemonoperation.h>
#include <gvfs/gvfstypes.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_DAEMON_BACKEND         (g_vfs_daemon_backend_get_type ())
#define G_VFS_DAEMON_BACKEND(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_DAEMON_BACKEND, GVfsDaemonBackend))
#define G_VFS_DAEMON_BACKEND_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_DAEMON_BACKEND, GVfsDaemonBackendClass))
#define G_IS_VFS_DAEMON_BACKEND(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_DAEMON_BACKEND))
#define G_IS_VFS_DAEMON_BACKEND_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_DAEMON_BACKEND))
#define G_VFS_DAEMON_BACKEND_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_DAEMON_BACKEND, GVfsDaemonBackendClass))

typedef struct _GVfsDaemonBackend        GVfsDaemonBackend;
typedef struct _GVfsDaemonBackendClass   GVfsDaemonBackendClass;

typedef gpointer GVfsHandle;

struct _GVfsDaemonBackend
{
  GObject parent_instance;
};

struct _GVfsDaemonBackendClass
{
  GObjectClass parent_class;

  /* vtable */

  /* These should all be fast and non-blocking, scheduling the i/o
   * operations async (or on a thread).
   * Returning FALSE means "Can't do this right now, try later" 
   * Returning TRUE means you started the job and will set the
   * result (or error) on the opernation object when done.
   * A NULL here means operation not supported 
   */

  gboolean (*open_for_read) (GVfsDaemonBackend *backend,
			     GVfsDaemonOperationOpenForRead *op,
			     char *filename);
  gboolean (*read)          (GVfsDaemonBackend *backend,
			     GVfsDaemonOperationRead *op,
			     GVfsHandle *handle,
			     gsize count);
  gboolean (*seek_on_read)  (GVfsDaemonBackend *backend,
			     GVfsDaemonOperationReadSeek *op,
			     GVfsHandle *handle,
			     goffset    offset,
			     GSeekType  type);
};

GType g_vfs_daemon_backend_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif /* __G_VFS_DAEMON_BACKEND_H__ */
