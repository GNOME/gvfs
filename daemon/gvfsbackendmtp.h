/* GIO - GLib Input, Output and Streaming Library
 *   MTP Backend
 * 
 * Copyright (C) 2012 Philip Langdale <philipl@overt.org>
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
 * Public License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef __G_VFS_BACKEND_MTP_H__
#define __G_VFS_BACKEND_MTP_H__

#include <gvfsbackend.h>
#include <gmountspec.h>
#include <gudev/gudev.h>
#include <libmtp.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_MTP         (g_vfs_backend_mtp_get_type ())
#define G_VFS_BACKEND_MTP(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND_MTP, GVfsBackendMtp))
#define G_VFS_BACKEND_MTP_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_MTP, GVfsBackendMtpClass))
#define G_VFS_IS_BACKEND_MTP(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND_MTP))
#define G_VFS_IS_BACKEND_MTP_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND_MTP))
#define G_VFS_BACKEND_MTP_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND_MTP, GVfsBackendMtpClass))

typedef struct _GVfsBackendMtp        GVfsBackendMtp;
typedef struct _GVfsBackendMtpClass   GVfsBackendMtpClass;

struct _GVfsBackendMtp
{
  GVfsBackend parent_instance;

  GUdevClient *gudev_client;

  GMutex mutex;
  LIBMTP_mtpdevice_t *device;
  char *dev_path;
  char *volume_name;
  char *volume_icon;
  char *volume_symbolic_icon;

  GHashTable *file_cache;

  GHashTable *monitors;
  guint hb_id;
  gint unmount_started;
  gboolean force_unmounted;

  gboolean android_extension;
  gboolean get_partial_object_capability;
  gboolean move_object_capability;
  gboolean copy_object_capability;

  GThreadPool *event_pool;
  GThread *event_thread;
  gboolean event_completed;
};

struct _GVfsBackendMtpClass
{
  GVfsBackendClass parent_class;
};

GType g_vfs_backend_mtp_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif /* __G_VFS_BACKEND_MTP_H__ */
