#ifndef __G_VFS_DAEMON_UTILS_H__
#define __G_VFS_DAEMON_UTILS_H__

#include <glib-object.h>
#include <dbus/dbus.h>
#include <gvfsreadrequest.h>

G_BEGIN_DECLS

DBusMessage *dbus_message_new_error_from_gerror (DBusMessage *message,
						 GError *error);

G_END_DECLS

#endif /* __G_VFS_DAEMON_UTILS_H__ */
