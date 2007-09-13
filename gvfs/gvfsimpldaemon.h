#ifndef __G_VFS_IMPL_DAEMON_H__
#define __G_VFS_IMPL_DAEMON_H__

#include <gvfs.h>
#include <dbus/dbus.h>

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
  const char *bus_name;
  const char *object_path;
} GDaemonMountInfo;

struct _GVfsImplDaemonClass
{
  GObjectClass parent_class;
};

GType   g_vfs_impl_daemon_get_type  (void) G_GNUC_CONST;

GVfsImplDaemon *g_vfs_impl_daemon_new       (void);
DBusMessage *_g_vfs_impl_daemon_new_path_call_valist (GQuark      match_bus_name,
						      const char *path,
						      const char *op,
						      int         first_arg_type,
						      va_list     var_args);
DBusMessage *_g_vfs_impl_daemon_new_path_call        (GQuark      match_bus_name,
						      const char *path,
						      const char *op,
						      int         first_arg_type,
						      ...);

G_END_DECLS

#endif /* __G_VFS_IMPL_DAEMON_H__ */
