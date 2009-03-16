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

#ifndef __G_DAEMON_FILE_OUTPUT_STREAM_H__
#define __G_DAEMON_FILE_OUTPUT_STREAM_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define G_TYPE_DAEMON_FILE_OUTPUT_STREAM         (g_daemon_file_output_stream_get_type ())
#define G_DAEMON_FILE_OUTPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DAEMON_FILE_OUTPUT_STREAM, GDaemonFileOutputStream))
#define G_DAEMON_FILE_OUTPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_DAEMON_FILE_OUTPUT_STREAM, GDaemonFileOutputStreamClass))
#define G_IS_DAEMON_FILE_OUTPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DAEMON_FILE_OUTPUT_STREAM))
#define G_IS_DAEMON_FILE_OUTPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DAEMON_FILE_OUTPUT_STREAM))
#define G_DAEMON_FILE_OUTPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_DAEMON_FILE_OUTPUT_STREAM, GDaemonFileOutputStreamClass))

typedef struct _GDaemonFileOutputStream         GDaemonFileOutputStream;
typedef struct _GDaemonFileOutputStreamClass    GDaemonFileOutputStreamClass;

struct _GDaemonFileOutputStreamClass
{
  GFileOutputStreamClass parent_class;
};

GType g_daemon_file_output_stream_get_type (void) G_GNUC_CONST;

GFileOutputStream *g_daemon_file_output_stream_new (int fd,
						    gboolean can_seek,
						    goffset initial_offset);

G_END_DECLS

#endif /* __G_DAEMON_FILE_OUTPUT_STREAM_H__ */
