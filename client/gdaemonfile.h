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

#ifndef __G_DAEMON_FILE_H__
#define __G_DAEMON_FILE_H__

#include <gio/gio.h>
#include "gdaemonvfs.h"
#include "gmountspec.h"

G_BEGIN_DECLS

#define G_TYPE_DAEMON_FILE         (g_daemon_file_get_type ())
#define G_DAEMON_FILE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DAEMON_FILE, GDaemonFile))
#define G_DAEMON_FILE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_DAEMON_FILE, GDaemonFileClass))
#define G_IS_DAEMON_FILE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DAEMON_FILE))
#define G_IS_DAEMON_FILE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DAEMON_FILE))
#define G_DAEMON_FILE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_DAEMON_FILE, GDaemonFileClass))

typedef struct _GDaemonFile        GDaemonFile;
typedef struct _GDaemonFileClass   GDaemonFileClass;

struct _GDaemonFileClass
{
  GObjectClass parent_class;
};

struct _GDaemonFile
{
  GObject parent_instance;

  GMountSpec *mount_spec;
  char *path;
};

GType g_daemon_file_get_type (void) G_GNUC_CONST;
  
GFile * g_daemon_file_new (GMountSpec *mount_spec,
			   const char *path);

G_END_DECLS

#endif /* __G_DAEMON_FILE_H__ */
