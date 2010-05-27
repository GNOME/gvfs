/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#ifndef __GVFS_DBUS_UTILS_H__
#define __GVFS_DBUS_UTILS_H__

#include <glib.h>
#include <dbus/dbus.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef gboolean (*GFDSourceFunc) (gpointer data,
				   GIOCondition condition,
				   int fd);
typedef void (*GAsyncDBusCallback) (DBusMessage *reply,
				    GError *error,
				    gpointer user_data);

/* Only used internally, never on wire */
#define G_DBUS_TYPE_CSTRING 1024

GList *      _g_dbus_bus_list_names_with_prefix     (DBusConnection   *connection,
						     const char       *prefix,
						     DBusError        *error);
void         _g_dbus_message_iter_append_cstring    (DBusMessageIter  *iter,
						     const char       *str);
void         _g_dbus_message_iter_append_args_valist (DBusMessageIter *iter,
						     int               first_arg_type,
						     va_list           var_args);
void         _g_dbus_message_iter_append_args       (DBusMessageIter  *iter,
						     int               first_arg_type,
						     ...);
void         _g_dbus_message_append_args_valist     (DBusMessage      *message,
						     int               first_arg_type,
						     va_list           var_args);
void         _g_dbus_message_append_args            (DBusMessage      *message,
						     int               first_arg_type,
						     ...);
dbus_bool_t  _g_dbus_message_iter_get_args_valist   (DBusMessageIter  *iter,
						     DBusError        *error,
						     int               first_arg_type,
						     va_list           var_args);
dbus_bool_t  _g_dbus_message_iter_get_args          (DBusMessageIter  *iter,
						     DBusError        *error,
						     int               first_arg_type,
						     ...);
void         _g_error_from_dbus                     (DBusError        *derror,
						     GError          **error);
gboolean     _g_error_from_message                  (DBusMessage      *message,
						     GError          **error);
DBusMessage *_dbus_message_new_from_gerror          (DBusMessage      *message,
						     GError           *error);
DBusMessage *_dbus_message_new_gerror               (DBusMessage      *message,
						     GQuark            domain,
						     gint              code,
						     const gchar      *format,
						     ...);
void         _g_dbus_connection_integrate_with_main (DBusConnection   *connection);
void         _g_dbus_connection_remove_from_main    (DBusConnection   *connection);
GSource *    __g_fd_source_new                      (int               fd,
						     gushort           events,
						     GCancellable     *cancellable);
void         _g_dbus_message_iter_copy              (DBusMessageIter  *dest,
						     DBusMessageIter  *source);
void         _g_dbus_oom                            (void) G_GNUC_NORETURN;
void        _g_dbus_connection_call_async           (DBusConnection *connection,
						     DBusMessage *message,
						     int timeout_msecs,
						     GAsyncDBusCallback callback,
						     gpointer user_data);

G_END_DECLS


#endif /* __GVFS_DBUS_UTILS_H__ */
