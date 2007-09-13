#ifndef __G_VFS_DAEMON_DBUS_H__
#define __G_VFS_DAEMON_DBUS_H__

#include <glib.h>
#include <dbus/dbus.h>
#include <gvfs/gcancellable.h>
#include <gvfs/gfileinfo.h>

G_BEGIN_DECLS

/* Only used internally */
#define G_DBUS_TYPE_CSTRING 1024

typedef void (*GVfsAsyncDBusCallback) (DBusMessage *reply,
				       DBusConnection *conntection,
				       GError *io_error,
				       GCancellable *cancellable,
				       gpointer op_callback,
				       gpointer op_callback_data,
				       gpointer callback_data);
typedef void (*GetFdAsyncCallback)    (int fd,
				       gpointer callback_data);

void         _g_dbus_register_vfs_filter             (const char                 *obj_path,
						      DBusHandleMessageFunction   callback,
						      GObject                    *data);
void         _g_dbus_unregister_vfs_filter           (const char                 *obj_path);
GList *      _g_dbus_bus_list_names_with_prefix_sync (DBusConnection             *connection,
						      const char                 *prefix,
						      DBusError                  *error);
int          _g_dbus_connection_get_fd_sync          (DBusConnection             *conn,
						      int                         fd_id);
gboolean     _g_dbus_message_iter_append_cstring     (DBusMessageIter            *iter,
						      const char                 *filename);
void         _g_dbus_message_append_args_valist      (DBusMessage                *message,
						      int                         first_arg_type,
						      va_list                     var_args);
void         _g_dbus_message_append_args             (DBusMessage                *message,
						      int                         first_arg_type,
						      ...);
void         _g_error_from_dbus                      (DBusError                  *derror,
						      GError                    **error);
void         _g_dbus_connection_get_fd_async         (DBusConnection             *connection,
						      int                         fd_id,
						      GetFdAsyncCallback          callback,
						      gpointer                    callback_data);
void         _g_vfs_daemon_call_async                (DBusMessage                *message,
						      gpointer                    op_callback,
						      gpointer                    op_callback_data,
						      GVfsAsyncDBusCallback       callback,
						      gpointer                    callback_data,
						      GCancellable               *cancellable);
DBusMessage *_g_vfs_daemon_call_sync                 (DBusMessage                *message,
						      DBusConnection            **connection_out,
						      GCancellable               *cancellable,
						      GError                    **error);
void         _g_dbus_connection_setup_with_main      (DBusConnection             *connection);
char *       _g_dbus_unescape_bus_name               (const char                 *escaped,
						      const char                 *end);
void         _g_dbus_append_escaped_bus_name         (GString                    *string,
						      gboolean                    at_start,
						      const char                 *unescaped);
GFileInfo *  _g_dbus_get_file_info                   (DBusMessageIter            *iter,
						      GFileInfoRequestFlags       requested,
						      GError                    **error);


G_END_DECLS

#endif /* __G_VFS_DAEMON_DBUS_H__ */
