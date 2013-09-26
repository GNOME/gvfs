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

#ifndef __G_DAEMON_FILE_ENUMERATOR_H__
#define __G_DAEMON_FILE_ENUMERATOR_H__

#include <gio/gio.h>
#include <gvfsdbus.h>

G_BEGIN_DECLS

#define G_TYPE_DAEMON_FILE_ENUMERATOR         (g_daemon_file_enumerator_get_type ())
#define G_DAEMON_FILE_ENUMERATOR(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DAEMON_FILE_ENUMERATOR, GDaemonFileEnumerator))
#define G_DAEMON_FILE_ENUMERATOR_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_DAEMON_FILE_ENUMERATOR, GDaemonFileEnumeratorClass))
#define G_IS_DAEMON_FILE_ENUMERATOR(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DAEMON_FILE_ENUMERATOR))
#define G_IS_DAEMON_FILE_ENUMERATOR_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DAEMON_FILE_ENUMERATOR))
#define G_DAEMON_FILE_ENUMERATOR_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_DAEMON_FILE_ENUMERATOR, GDaemonFileEnumeratorClass))

typedef struct _GDaemonFileEnumerator         GDaemonFileEnumerator;
typedef struct _GDaemonFileEnumeratorClass    GDaemonFileEnumeratorClass;
typedef struct _GDaemonFileEnumeratorPrivate  GDaemonFileEnumeratorPrivate;

struct _GDaemonFileEnumeratorClass
{
  GFileEnumeratorClass parent_class;
};

GType g_daemon_file_enumerator_get_type (void) G_GNUC_CONST;

GDaemonFileEnumerator *g_daemon_file_enumerator_new                 (GFile *file,
                                                                     GVfsDBusMount *proxy,
								     const char *attributes,
								     gboolean sync);
char  *                g_daemon_file_enumerator_get_object_path     (GDaemonFileEnumerator *enumerator);

void                   g_daemon_file_enumerator_set_sync_connection (GDaemonFileEnumerator *enumerator,
                                                                     GDBusConnection       *connection);

G_END_DECLS

#endif /* __G_FILE_DAEMON_FILE_ENUMERATOR_H__ */
