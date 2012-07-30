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

#ifndef __G_VFS_READ_CHANNEL_H__
#define __G_VFS_READ_CHANNEL_H__

#include <glib-object.h>
#include <gvfsjob.h>
#include <gvfschannel.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_READ_CHANNEL         (g_vfs_read_channel_get_type ())
#define G_VFS_READ_CHANNEL(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_READ_CHANNEL, GVfsReadChannel))
#define G_VFS_READ_CHANNEL_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_READ_CHANNEL, GVfsReadChannelClass))
#define G_VFS_IS_READ_CHANNEL(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_READ_CHANNEL))
#define G_VFS_IS_READ_CHANNEL_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_READ_CHANNEL))
#define G_VFS_READ_CHANNEL_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_READ_CHANNEL, GVfsReadChannelClass))

typedef struct _GVfsReadChannel        GVfsReadChannel;
typedef struct _GVfsReadChannelClass   GVfsReadChannelClass;
typedef struct _GVfsReadChannelPrivate GVfsReadChannelPrivate;

struct _GVfsReadChannelClass
{
  GVfsChannelClass parent_class;
};

GType g_vfs_read_channel_get_type (void) G_GNUC_CONST;

GVfsReadChannel *g_vfs_read_channel_new                (GVfsBackend        *backend,
                                                        GPid                actual_consumer);
void            g_vfs_read_channel_send_data          (GVfsReadChannel     *read_channel,
						       char               *buffer,
						       gsize               count);
void            g_vfs_read_channel_send_closed        (GVfsReadChannel     *read_channel);
void            g_vfs_read_channel_send_seek_offset   (GVfsReadChannel     *read_channel,
						      goffset             offset);

G_END_DECLS

#endif /* __G_VFS_READ_CHANNEL_H__ */
