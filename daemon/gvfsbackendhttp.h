/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2008 Red Hat, Inc.
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

  SoupURI     *mount_base;
  SoupSession *session;

  SoupSession *session_async;
};

GType         g_vfs_backend_http_get_type    (void) G_GNUC_CONST;

char *        http_uri_get_basename          (const char *uri_str);

char *        http_path_get_basename         (const char *path_str);

guint         http_error_code_from_status    (guint status);

SoupURI *     http_backend_get_mount_base    (GVfsBackend *backend);

guint         http_backend_send_message      (GVfsBackend *backend,
                                              SoupMessage *msg);

void          http_backend_queue_message     (GVfsBackend         *backend,
                                              SoupMessage         *msg,
                                              SoupSessionCallback  callback,
                                              gpointer             user_data);

void          http_backend_open_for_read     (GVfsBackend         *backend,
					      GVfsJob             *job,
					      SoupURI             *uri);

void          http_job_failed                (GVfsJob             *job,
					      SoupMessage         *msg);

G_END_DECLS

#endif /* __G_VFS_BACKEND_HTTP_H__ */
