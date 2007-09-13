#ifndef __G_VFS_DAEMON_UTILS_H__
#define __G_VFS_DAEMON_UTILS_H__

#include <glib-object.h>
#include <dbus/dbus.h>

G_BEGIN_DECLS

void         dbus_connection_add_fd_send_fd       (DBusConnection   *connection,
						   int               extra_fd);
gboolean     dbus_connection_send_fd              (DBusConnection   *connection,
						   int               fd,
						   int              *fd_id,
						   GError          **error);
char *       g_error_to_daemon_reply              (GError           *error,
						   guint32           seq_nr,
						   gsize            *len_out);

G_END_DECLS

#endif /* __G_VFS_DAEMON_UTILS_H__ */
