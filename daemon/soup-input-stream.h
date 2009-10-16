/* Copyright (C) 2006-2007 Red Hat, Inc.
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
 */

#ifndef __SOUP_INPUT_STREAM_H__
#define __SOUP_INPUT_STREAM_H__

#include <gio/gio.h>
#include <libsoup/soup-types.h>

G_BEGIN_DECLS

#define SOUP_TYPE_INPUT_STREAM         (soup_input_stream_get_type ())
#define SOUP_INPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), SOUP_TYPE_INPUT_STREAM, SoupInputStream))
#define SOUP_INPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), SOUP_TYPE_INPUT_STREAM, SoupInputStreamClass))
#define SOUP_IS_INPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), SOUP_TYPE_INPUT_STREAM))
#define SOUP_IS_INPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), SOUP_TYPE_INPUT_STREAM))
#define SOUP_INPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), SOUP_TYPE_INPUT_STREAM, SoupInputStreamClass))

typedef struct SoupInputStream         SoupInputStream;
typedef struct SoupInputStreamClass    SoupInputStreamClass;

struct SoupInputStream
{
  GInputStream parent;

};

struct SoupInputStreamClass
{
  GInputStreamClass parent_class;

  /* Padding for future expansion */
  void (*_g_reserved1) (void);
  void (*_g_reserved2) (void);
  void (*_g_reserved3) (void);
  void (*_g_reserved4) (void);
  void (*_g_reserved5) (void);
};

GType soup_input_stream_get_type (void) G_GNUC_CONST;

GInputStream *soup_input_stream_new         (SoupSession         *session,
					     SoupMessage         *msg);

gboolean      soup_input_stream_send        (GInputStream        *stream,
					     GCancellable        *cancellable,
					     GError             **error);

void          soup_input_stream_send_async  (GInputStream        *stream,
					     int                  io_priority,
					     GCancellable        *cancellable,
					     GAsyncReadyCallback  callback,
					     gpointer             user_data);
gboolean      soup_input_stream_send_finish (GInputStream        *stream,
					     GAsyncResult        *result,
					     GError             **error);

SoupMessage  *soup_input_stream_get_message (GInputStream         *stream);

#define SOUP_HTTP_ERROR soup_http_error_quark()
GQuark soup_http_error_quark (void);

G_END_DECLS

#endif /* __SOUP_INPUT_STREAM_H__ */
