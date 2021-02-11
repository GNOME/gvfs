/* Copyright (C) 2006, 2007, 2012 Red Hat, Inc.
 * Copyright (C) 2021 Igalia S.L.
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
 */

#ifndef __G_VFS_HTTP_INPUT_STREAM_H__
#define __G_VFS_HTTP_INPUT_STREAM_H__

#include <gio/gio.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_HTTP_INPUT_STREAM         (g_vfs_http_input_stream_get_type ())
#define G_VFS_HTTP_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_HTTP_INPUT_STREAM, GVfsHttpInputStream))
#define G_VFS_HTTP_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_HTTP_INPUT_STREAM, GVfsHttpInputStreamClass))
#define G_VFS_IS_HTTP_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_HTTP_INPUT_STREAM))
#define G_VFS_IS_HTTP_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_HTTP_INPUT_STREAM))
#define G_VFS_HTTP_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_HTTP_INPUT_STREAM, GVfsHttpInputStreamClass))

typedef struct GVfsHttpInputStream         GVfsHttpInputStream;
typedef struct GVfsHttpInputStreamPrivate  GVfsHttpInputStreamPrivate;
typedef struct GVfsHttpInputStreamClass    GVfsHttpInputStreamClass;

struct GVfsHttpInputStream
{
  GInputStream parent;

  GVfsHttpInputStreamPrivate *priv;
};

struct GVfsHttpInputStreamClass
{
  GInputStreamClass parent_class;

  /* Padding for future expansion */
  void (*_g_reserved1) (void);
  void (*_g_reserved2) (void);
  void (*_g_reserved3) (void);
  void (*_g_reserved4) (void);
  void (*_g_reserved5) (void);
};

GType g_vfs_http_input_stream_get_type (void) G_GNUC_CONST;

GInputStream *g_vfs_http_input_stream_new         (SoupSession          *session,
                                                   GUri                 *uri);

void          g_vfs_http_input_stream_send_async  (GInputStream         *stream,
						   int                   io_priority,
						   GCancellable         *cancellable,
						   GAsyncReadyCallback   callback,
						   gpointer              user_data);
gboolean      g_vfs_http_input_stream_send_finish (GInputStream         *stream,
						   GAsyncResult         *result,
						   GError              **error);

SoupMessage  *g_vfs_http_input_stream_get_message (GInputStream         *stream);

G_END_DECLS

#endif /* __G_VFS_HTTP_INPUT_STREAM_H__ */
