#ifndef __G_VFS_DAEMON_DBUS_H__
#define __G_VFS_DAEMON_DBUS_H__

#include <glib.h>
#include <dbus/dbus.h>

G_BEGIN_DECLS

DBusConnection *_g_vfs_daemon_get_connection_sync    (const char       *mountpoint,
						      GError          **error);
int             _g_dbus_connection_get_fd_sync       (DBusConnection   *conn,
						      int               fd_id);
gboolean        _g_dbus_message_iter_append_filename (DBusMessageIter  *iter,
						      const char       *filename);
gboolean        _g_error_from_dbus_message           (DBusMessage      *message,
						      GError          **error);
void            _g_error_from_dbus                   (DBusError        *derror,
						      GError          **error);

G_END_DECLS

#endif /* __G_VFS_DAEMON_DBUS_H__ */
