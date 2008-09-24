/* Copyright (C) 2006-2008 Red Hat, Inc.
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

#ifndef __SOUP_OUTPUT_STREAM_H__
#define __SOUP_OUTPUT_STREAM_H__

#include <gio/gio.h>
#include <libsoup/soup-types.h>

G_BEGIN_DECLS

#define SOUP_TYPE_OUTPUT_STREAM         (soup_output_stream_get_type ())
#define SOUP_OUTPUT_STREAM(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), SOUP_TYPE_OUTPUT_STREAM, SoupOutputStream))
#define SOUP_OUTPUT_STREAM_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), SOUP_TYPE_OUTPUT_STREAM, SoupOutputStreamClass))
#define SOUP_IS_OUTPUT_STREAM(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), SOUP_TYPE_OUTPUT_STREAM))
#define SOUP_IS_OUTPUT_STREAM_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), SOUP_TYPE_OUTPUT_STREAM))
#define SOUP_OUTPUT_STREAM_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), SOUP_TYPE_OUTPUT_STREAM, SoupOutputStreamClass))

typedef struct SoupOutputStream         SoupOutputStream;
typedef struct SoupOutputStreamClass    SoupOutputStreamClass;

struct SoupOutputStream
{
  GOutputStream parent;

};

struct SoupOutputStreamClass
{
  GOutputStreamClass parent_class;

  /* Padding for future expansion */
  void (*_g_reserved1) (void);
  void (*_g_reserved2) (void);
  void (*_g_reserved3) (void);
  void (*_g_reserved4) (void);
  void (*_g_reserved5) (void);
};

GType soup_output_stream_get_type (void) G_GNUC_CONST;

GOutputStream *soup_output_stream_new         (SoupSession         *session,
					       SoupMessage         *msg,
					       goffset              size);

G_END_DECLS

#endif /* __SOUP_OUTPUT_STREAM_H__ */
