#ifndef __G_VFS_IMPL_DAEMON_H__
#define __G_VFS_IMPL_DAEMON_H__

#include <gvfs.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_IMPL_DAEMON			(g_vfs_impl_daemon_get_type ())
#define G_VFS_IMPL_DAEMON(obj)			(G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_VFS_IMPL_DAEMON, GVfsImplDaemon))
#define G_VFS_IMPL_DAEMON_CLASS(klass)		(G_TYPE_CHECK_CLASS_CAST ((klass), G_TYPE_VFS_IMPL_DAEMON, GVfsImplDaemonClass))
#define G_IS_VFS_IMPL_DAEMON(obj)		(G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_VFS_IMPL_DAEMON))
#define G_IS_VFS_IMPL_DAEMON_CLASS(klass)	(G_TYPE_CHECK_CLASS_TYPE ((klass), G_TYPE_VFS_IMPL_DAEMON))
#define G_VFS_IMPL_DAEMON_GET_CLASS(obj)	(G_TYPE_INSTANCE_GET_CLASS ((obj), G_TYPE_VFS_IMPL_DAEMON, GVfsImplDaemonClass))

typedef struct _GVfsImplDaemon       GVfsImplDaemon;
typedef struct _GVfsImplDaemonClass  GVfsImplDaemonClass;

typedef struct {
  volatile int ref_count;
  gboolean is_mounted;
  char *dbus_owner;
  char *dbus_path;
  char *method;
  char *user;
  char *host;
  int port;
  char *path;
} GVfsMountpointInfo;

struct _GVfsImplDaemonClass
{
  GObjectClass parent_class;
};

GType   g_vfs_impl_daemon_get_type  (void) G_GNUC_CONST;

GVfsImplDaemon *g_vfs_impl_daemon_new (void);

GVfsMountpointInfo *g_vfs_mountpoint_info_ref   (GVfsMountpointInfo *info);
void                g_vfs_mountpoint_info_unref (GVfsMountpointInfo *info);

G_END_DECLS

#endif /* __G_VFS_IMPL_DAEMON_H__ */
