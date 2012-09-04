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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#ifndef __G_MOUNT_SOURCE_H__
#define __G_MOUNT_SOURCE_H__

#include <glib-object.h>
#include <gmountspec.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define G_TYPE_MOUNT_SOURCE         (g_mount_source_get_type ())
#define G_MOUNT_SOURCE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_MOUNT_SOURCE, GMountSource))
#define G_MOUNT_SOURCE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_MOUNT_SOURCE, GMountSourceClass))
#define G_IS_MOUNT_SOURCE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_MOUNT_SOURCE))
#define G_IS_MOUNT_SOURCE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_MOUNT_SOURCE))
#define G_MOUNT_SOURCE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_MOUNT_SOURCE, GMountSourceClass))

typedef struct _GMountSource        GMountSource;
typedef struct _GMountSourceClass   GMountSourceClass;

struct _GMountSourceClass
{
  GObjectClass parent_class;
};

typedef void   (*RequestMountSpecCallback)   (GMountSource *source,
					      GMountSpec *mount_spec,
					      GError *error,
					      gpointer data);

GType g_mount_source_get_type (void) G_GNUC_CONST;

GMountSource *g_mount_source_new                      (const char                *dbus_id,
						       const char                *obj_path);
GMountSource *g_mount_source_new_dummy                (void);
GVariant     *g_mount_source_to_dbus                  (GMountSource              *source);
GMountSource *g_mount_source_from_dbus                (GVariant                  *value);
gboolean      g_mount_source_ask_password             (GMountSource              *mount_source,
						       const char                *message,
						       const char                *initial_user,
						       const char                *initial_domain,
						       GAskPasswordFlags          flags,
						       gboolean                  *aborted,
						       char                     **password_out,
						       char                     **user_out,
						       char                     **domain_out,
						       gboolean			 *anonymous_out,
						       GPasswordSave             *password_save_out);

void          g_mount_source_ask_password_async       (GMountSource              *mount_source,
						       const char                *message,
						       const char                *initial_user,
						       const char                *initial_domain,
						       GAskPasswordFlags          flags,
                                                       GAsyncReadyCallback        callback,
                                                       gpointer                   user_data);

gboolean     g_mount_source_ask_password_finish       (GMountSource              *source,
                                                       GAsyncResult              *result,
                                                       gboolean                  *aborted,
                                                       char                     **password_out,
                                                       char                     **user_out,
                                                       char                     **domain_out,
						       gboolean			 *anonymous_out,
						       GPasswordSave             *password_save_out);

gboolean      g_mount_source_ask_question             (GMountSource              *mount_source,
						       const char                *message,
						       const char               **choices,
						       gboolean                  *aborted,
						       gint                      *choice_out);

void          g_mount_source_ask_question_async       (GMountSource              *mount_source,
						       const char                *message,
						       const char               **choices,
                                                       GAsyncReadyCallback        callback,
                                                       gpointer                   user_data);

gboolean     g_mount_source_ask_question_finish       (GMountSource              *source,
                                                       GAsyncResult              *result,
                                                       gboolean                  *aborted,
						       gint                      *choice_out);

gboolean      g_mount_source_show_processes           (GMountSource              *mount_source,
						       const char                *message,
                                                       GArray                    *processes,
						       const char               **choices,
						       gboolean                  *aborted,
						       gint                      *choice_out);

void          g_mount_source_show_processes_async     (GMountSource              *mount_source,
						       const char                *message,
                                                       GArray                    *processes,
						       const char               **choices,
                                                       GAsyncReadyCallback        callback,
                                                       gpointer                   user_data);

gboolean     g_mount_source_show_processes_finish     (GMountSource              *source,
                                                       GAsyncResult              *result,
                                                       gboolean                  *aborted,
                                                       gint                      *choice_out);

void         g_mount_source_show_unmount_progress     (GMountSource              *mount_source,
						       const char                *message,
                                                       gint64                     time_left,
                                                       gint64                     bytes_left);

gboolean     g_mount_source_abort                     (GMountSource              *source);

gboolean     g_mount_source_is_dummy                  (GMountSource              *source);


const char *  g_mount_source_get_dbus_id              (GMountSource              *mount_source);
const char *  g_mount_source_get_obj_path             (GMountSource              *mount_source);

GMountOperation *g_mount_source_get_operation         (GMountSource              *mount_source);

G_END_DECLS

#endif /* __G_MOUNT_SOURCE_H__ */
