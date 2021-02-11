/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2008 Red Hat, Inc.
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
 *
 * Author: Christian Kellner <gicmo@gnome.org>
 */

#ifndef __G_VFS_BACKEND_HTTP_H__
#define __G_VFS_BACKEND_HTTP_H__

#include <gvfsbackend.h>
#include <gmountspec.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_HTTP         (g_vfs_backend_http_get_type ())
#define G_VFS_BACKEND_HTTP(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND_HTTP, GVfsBackendHttp))
#define G_VFS_BACKEND_HTTP_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_HTTP, GVfsBackendHttpClass))
#define G_VFS_IS_BACKEND_HTTP(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND_HTTP))
#define G_VFS_IS_BACKEND_HTTP_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND_HTTP))
#define G_VFS_BACKEND_HTTP_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND_HTTP, GVfsBackendHttpClass))

typedef struct _GVfsBackendHttp        GVfsBackendHttp;
typedef struct _GVfsBackendHttpClass   GVfsBackendHttpClass;

struct _GVfsBackendHttpClass
{
  GVfsBackendClass parent_class;

};

struct _GVfsBackendHttp
{
  GVfsBackend parent_instance;

  GUri        *mount_base;
  SoupSession *session;
};

GType         g_vfs_backend_http_get_type    (void) G_GNUC_CONST;

char *        http_uri_get_basename          (const char *uri_str);

char *        http_path_get_basename         (const char *path_str);

int           http_error_code_from_status    (guint status);

SoupSession * http_try_init_session          (gint max_conns,
                                              gint max_conns_per_host);

GUri *        http_backend_get_mount_base    (GVfsBackend *backend);

void          http_backend_open_for_read     (GVfsBackend         *backend,
					      GVfsJob             *job,
					      GUri                *uri);

void          http_job_failed                (GVfsJob             *job,
					      SoupMessage         *msg);

G_END_DECLS

#endif /* __G_VFS_BACKEND_HTTP_H__ */
