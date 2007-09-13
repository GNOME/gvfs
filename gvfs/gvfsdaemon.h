#ifndef __G_VFS_DAEMON_H__
#define __G_VFS_DAEMON_H__

#include <gvfs.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_DAEMON			(g_vfs_daemon_get_type ())
#define G_VFS_DAEMON(obj)			(G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_VFS_DAEMON, GVfsDaemon))
#define G_VFS_DAEMON_CLASS(klass)		(G_TYPE_CHECK_CLASS_CAST ((klass), G_TYPE_VFS_DAEMON, GVfsDaemonClass))
#define G_IS_VFS_DAEMON(obj)			(G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_VFS_DAEMON))
#define G_IS_VFS_DAEMON_CLASS(klass)		(G_TYPE_CHECK_CLASS_TYPE ((klass), G_TYPE_VFS_DAEMON))
#define G_VFS_DAEMON_GET_CLASS(obj)		(G_TYPE_INSTANCE_GET_CLASS ((obj), G_TYPE_VFS_DAEMON, GVfsDaemonClass))


typedef struct _GVfsDaemon       GVfsDaemon;
typedef struct _GVfsDaemonClass  GVfsDaemonClass;

struct _GVfsDaemonClass
{
  GObjectClass parent_class;
  
};

GType   g_vfs_daemon_get_type  (void) G_GNUC_CONST;

GVfsDaemon *g_vfs_daemon_new (void);

G_END_DECLS

#endif /* __G_VFS_DAEMON_H__ */
