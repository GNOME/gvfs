#ifndef __G_VFS_IMPL_DAEMON_H__
#define __G_VFS_IMPL_DAEMON_H__

#include <gio/gvfs.h>
#include <dbus/dbus.h>
#include "gmountspec.h"

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
  char *dbus_id;
  char *object_path;
  GMountSpec *spec;
} GMountInfo;

typedef void (*GMountInfoLookupCallback) (GMountInfo *mount_info,
					  gpointer data,
					  GError *error);

struct _GVfsImplDaemonClass
{
  GObjectClass parent_class;
};

GType   g_vfs_impl_daemon_get_type  (void) G_GNUC_CONST;

GVfsImplDaemon *g_vfs_impl_daemon_new (void);

void        _g_vfs_impl_daemon_get_mount_info_async (GMountSpec                *spec,
						     const char                *path,
						     GMountInfoLookupCallback   callback,
						     gpointer                   user_data);
GMountInfo *_g_vfs_impl_daemon_get_mount_info_sync  (GMountSpec                *spec,
						     const char                *path,
						     GError                   **error);
const char *_g_mount_info_resolve_path              (GMountInfo                *info,
						     const char                *path);
void        _g_mount_info_unref                     (GMountInfo                *info);

G_END_DECLS

#endif /* __G_VFS_IMPL_DAEMON_H__ */
