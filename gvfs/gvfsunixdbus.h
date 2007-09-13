#ifndef __G_VFS_UNIX_DBUS_H__
#define __G_VFS_UNIX_DBUS_H__

#include <glib.h>
#include <dbus/dbus.h>

DBusConnection *_g_vfs_unix_get_connection_sync (const char *mountpoint);

#endif /* __G_VFS_UNIX_DBUS_H__ */
