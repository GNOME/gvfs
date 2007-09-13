#ifndef __G_DAEMON_VFS_H__
#define __G_DAEMON_VFS_H__

#include <gio/gvfs.h>
#include <dbus/dbus.h>
#include "gmountspec.h"

G_BEGIN_DECLS

#define G_TYPE_DAEMON_VFS			(g_daemon_vfs_get_type ())
#define G_DAEMON_VFS(obj)			(G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_DAEMON_VFS, GDaemonVfs))
#define G_DAEMON_VFS_CLASS(klass)		(G_TYPE_CHECK_CLASS_CAST ((klass), G_TYPE_DAEMON_VFS, GDaemonVfsClass))
#define G_IS_DAEMON_VFS(obj)		(G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_DAEMON_VFS))
#define G_IS_DAEMON_VFS_CLASS(klass)	(G_TYPE_CHECK_CLASS_TYPE ((klass), G_TYPE_DAEMON_VFS))
#define G_DAEMON_VFS_GET_CLASS(obj)	(G_TYPE_INSTANCE_GET_CLASS ((obj), G_TYPE_DAEMON_VFS, GDaemonVfsClass))

typedef struct _GDaemonVfs       GDaemonVfs;
typedef struct _GDaemonVfsClass  GDaemonVfsClass;

typedef struct {
  volatile int ref_count;
  char *dbus_id;
  char *object_path;
  GMountSpec *spec;
} GMountRef;

typedef void (*GMountRefLookupCallback) (GMountRef *mount_ref,
					 gpointer data,
					 GError *error);

struct _GDaemonVfsClass
{
  GObjectClass parent_class;
};

GType   g_daemon_vfs_get_type  (void) G_GNUC_CONST;

GDaemonVfs *g_daemon_vfs_new (void);

GList      *_g_daemon_vfs_get_mount_list_sync (GError                  **error);
void        _g_daemon_vfs_get_mount_ref_async (GMountSpec               *spec,
					       const char               *path,
					       GMountRefLookupCallback   callback,
					       gpointer                  user_data);
GMountRef  *_g_daemon_vfs_get_mount_ref_sync  (GMountSpec               *spec,
					       const char               *path,
					       GError                  **error);
const char *_g_mount_ref_resolve_path         (GMountRef                *ref,
					       const char               *path);
void        _g_mount_ref_unref                (GMountRef                *ref);

G_END_DECLS

#endif /* __G_DAEMON_VFS_H__ */
