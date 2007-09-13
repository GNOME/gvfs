#ifndef __G_VFS_DAEMON_H__
#define __G_VFS_DAEMON_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_DAEMON         (g_vfs_daemon_get_type ())
#define G_VFS_DAEMON(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_DAEMON, GVfsDaemon))
#define G_VFS_DAEMON_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_DAEMON, GVfsDaemonClass))
#define G_IS_VFS_DAEMON(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_DAEMON))
#define G_IS_VFS_DAEMON_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_DAEMON))
#define G_VFS_DAEMON_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_DAEMON, GVfsDaemonClass))

typedef struct _GVfsDaemon        GVfsDaemon;
typedef struct _GVfsDaemonClass   GVfsDaemonClass;
typedef struct _GVfsDaemonPrivate GVfsDaemonPrivate;

/* Placed here to fix circular ref problems in headers */
typedef struct _GVfsDaemonBackend GVfsDaemonBackend;

struct _GVfsDaemon
{
  GObject parent_instance;

  GVfsDaemonBackend *backend;
  GVfsDaemonPrivate *priv;
};

struct _GVfsDaemonClass
{
  GObjectClass parent_class;

  /* vtable */
  
};

GType g_vfs_daemon_get_type (void) G_GNUC_CONST;

GVfsDaemon *g_vfs_daemon_new (const char *mountpoint,
			      GVfsDaemonBackend *backend);
gboolean g_vfs_daemon_is_active (GVfsDaemon *daemon);

G_END_DECLS

#endif /* __G_VFS_DAEMON_H__ */
