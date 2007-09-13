#ifndef __G_VFS_DAEMON_PROTOCOL_H__
#define __G_VFS_DAEMON_PROTOCOL_H__

G_BEGIN_DECLS

#define G_VFS_DBUS_MOUNTPOINT_NAME "org.gtk.vfs.mount."
#define G_VFS_DBUS_ERROR_SOCKET_FAILED "org.gtk.vfs.Error.SocketFailed"

#define G_VFS_DBUS_DAEMON_PATH "/org/gtk/vfs/Daemon"
#define G_VFS_DBUS_DAEMON_INTERFACE "org.gtk.vfs.Daemon"
#define G_VFS_DBUS_OP_GET_CONNECTION "GetConnection"
#define G_VFS_DBUS_OP_OPEN_FOR_READ "OpenForRead"

G_END_DECLS

#endif /* __G_VFS_DAEMON_PROTOCOL_H__ */
