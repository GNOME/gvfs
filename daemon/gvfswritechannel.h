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

#ifndef __G_VFS_WRITE_CHANNEL_H__
#define __G_VFS_WRITE_CHANNEL_H__

#include <glib-object.h>
#include <gvfsjob.h>
#include <gvfschannel.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_WRITE_CHANNEL         (g_vfs_write_channel_get_type ())
#define G_VFS_WRITE_CHANNEL(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_WRITE_CHANNEL, GVfsWriteChannel))
#define G_VFS_WRITE_CHANNEL_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_WRITE_CHANNEL, GVfsWriteChannelClass))
#define G_VFS_IS_WRITE_CHANNEL(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_WRITE_CHANNEL))
#define G_VFS_IS_WRITE_CHANNEL_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_WRITE_CHANNEL))
#define G_VFS_WRITE_CHANNEL_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_WRITE_CHANNEL, GVfsWriteChannelClass))

typedef struct _GVfsWriteChannel        GVfsWriteChannel;
typedef struct _GVfsWriteChannelClass   GVfsWriteChannelClass;
typedef struct _GVfsWriteChannelPrivate GVfsWriteChannelPrivate;

struct _GVfsWriteChannelClass
{
  GVfsChannelClass parent_class;
};

GType g_vfs_write_channel_get_type (void) G_GNUC_CONST;

GVfsWriteChannel *g_vfs_write_channel_new              (GVfsBackend      *backend,
                                                        GPid              actual_consumer);
void              g_vfs_write_channel_send_written     (GVfsWriteChannel *write_channel,
							gsize             bytes_written);
void              g_vfs_write_channel_send_closed      (GVfsWriteChannel *write_channel,
							const char       *etag);
void              g_vfs_write_channel_send_seek_offset (GVfsWriteChannel *write_channel,
							goffset           offset);

G_END_DECLS

#endif /* __G_VFS_WRITE_CHANNEL_H__ */
