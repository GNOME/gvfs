#ifndef __G_DAEMON_VFS_H__
#define __G_DAEMON_VFS_H__

#include <gio/gvfs.h>
#include <dbus/dbus.h>
#include "gmountspec.h"
#include "gmounttracker.h"
#include "gvfsuriutils.h"

G_BEGIN_DECLS

typedef struct _GDaemonVfs       GDaemonVfs;
typedef struct _GDaemonVfsClass  GDaemonVfsClass;

typedef void (*GMountInfoLookupCallback) (GMountInfo *mount_info,
					  gpointer data,
					  GError *error);

GType   g_daemon_vfs_get_type  (void);

GDaemonVfs *g_daemon_vfs_new (void);

char *          _g_daemon_vfs_get_uri_for_mountspec    (GMountSpec               *spec,
							char                     *path,
							gboolean                  allow_utf8);
const char *    _g_daemon_vfs_mountspec_get_uri_scheme (GMountSpec               *spec);
void            _g_daemon_vfs_get_mount_info_async     (GMountSpec               *spec,
							const char               *path,
							GMountInfoLookupCallback  callback,
							gpointer                  user_data);
GMountInfo *    _g_daemon_vfs_get_mount_info_sync      (GMountSpec               *spec,
							const char               *path,
							GError                  **error);
DBusConnection *_g_daemon_vfs_get_async_bus            (void);



G_END_DECLS

#endif /* __G_DAEMON_VFS_H__ */
