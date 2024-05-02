/* gvfsbackendwsdd.c
 *
 * Copyright (C) 2023 Red Hat, Inc.
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
 * You should have received a copy of the GNU Library General Public
 * License along with the Gnome Library; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Authors: Ondrej Holy <oholy@redhat.com>
 */

#include <config.h>

#include <gio/gio.h>
#include <glib.h>
#include <glib/gi18n.h>

#include "gvfsbackendwsdd.h"
#include "gvfswsdddevice.h"
#include "gvfswsddservice.h"
#include "gvfswsddresolver.h"

#include "gvfsdaemonprotocol.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobmount.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsutils.h"

struct _GVfsBackendWsdd
{
  GVfsBackend parent_instance;

  GVfsWsddService *wsdd_service;
  GVfsWsddResolver *wsdd_resolver;

  GVfsMonitor *root_monitor;
};

G_DEFINE_TYPE (GVfsBackendWsdd, g_vfs_backend_wsdd, G_VFS_TYPE_BACKEND)

static inline gboolean
is_root (const gchar *filename)
{
  g_return_val_if_fail (filename != NULL, FALSE);

  return g_str_equal (filename, "/");
}

static void
file_info_from_wsdd_device (GVfsBackendWsdd *wsdd_backend,
                            GVfsWsddDevice *device,
                            GFileInfo *info)
{
  g_autoptr(GIcon) icon = NULL;
  g_autoptr(GIcon) symbolic_icon = NULL;
  g_autofree gchar *uri = NULL;
  g_autofree gchar *address = NULL;

  g_file_info_set_name (info, g_vfs_wsdd_device_get_uuid (device));
  g_file_info_set_display_name (info, g_vfs_wsdd_device_get_name (device));

  icon = g_themed_icon_new ("network-server");
  g_file_info_set_icon (info, icon);

  symbolic_icon = g_themed_icon_new ("network-server-symbolic");
  g_file_info_set_symbolic_icon (info, symbolic_icon);

  g_file_info_set_file_type (info, G_FILE_TYPE_SHORTCUT);
  g_file_info_set_content_type (info, "inode/directory");
  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_STANDARD_IS_VIRTUAL,
                                     TRUE);

  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME,
                                     FALSE);
  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE,
                                     FALSE);
  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE,
                                     FALSE);
  g_file_info_set_attribute_boolean (info,
                                     G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH,
                                     FALSE);

  address = g_vfs_wsdd_resolver_get_address (wsdd_backend->wsdd_resolver,
                                             device);
  if (address == NULL)
    {
      address = g_vfs_wsdd_device_get_first_address (device);
      g_vfs_wsdd_resolver_resolve (wsdd_backend->wsdd_resolver, device);
    }

  uri = g_strconcat ("smb://", address, "/", NULL);
  g_file_info_set_attribute_string (info,
                                    G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,
                                    uri);
}

static GVfsWsddDevice *
lookup_wsdd_device (GList *devices,
                    const char *filename)
{
  GList *l;

  g_return_val_if_fail (filename != NULL && *filename != '\0', NULL);

  for (l = devices; l != NULL; l = l->next)
    {
      if (g_str_equal (g_vfs_wsdd_device_get_uuid (l->data), filename + 1))
        {
          return l->data;
        }
    }

  return NULL;
}

static void
device_changed_cb (GVfsWsddService *wsdd_service,
                   const gchar *uuid,
                   GFileMonitorEvent event,
                   gpointer user_data)
{
  GVfsBackendWsdd *wsdd_backend = G_VFS_BACKEND_WSDD (user_data);
  g_autofree gchar *path = NULL;

  path = g_strconcat ("/", uuid, NULL);
  g_vfs_monitor_emit_event (wsdd_backend->root_monitor, event, path, NULL);
}

static void
device_resolved_cb (GVfsWsddResolver *wsdd_resolver,
                    const gchar *uuid,
                    gpointer user_data)
{
  GVfsBackendWsdd *wsdd_backend = G_VFS_BACKEND_WSDD (user_data);
  g_autofree gchar *path = NULL;

  path = g_strconcat ("/", uuid, NULL);
  g_vfs_monitor_emit_event (wsdd_backend->root_monitor,
                            G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED,
                            path,
                            NULL);
}

static void
try_mount_service_cb (GObject* source_object,
                      GAsyncResult* result,
                      gpointer user_data)
{
  GVfsJobMount *job = G_VFS_JOB_MOUNT (user_data);
  GVfsBackendWsdd *wsdd_backend = G_VFS_BACKEND_WSDD (job->backend);
  g_autoptr (GError) error = NULL;

  wsdd_backend->wsdd_service = g_vfs_wsdd_service_new_finish (result, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);

      return;
    }

  g_signal_connect (wsdd_backend->wsdd_service,
                    "device-changed",
                    G_CALLBACK (device_changed_cb),
                    wsdd_backend);

  wsdd_backend->wsdd_resolver = g_vfs_wsdd_resolver_new ();
  g_signal_connect (wsdd_backend->wsdd_resolver,
                    "device-resolved",
                    G_CALLBACK (device_resolved_cb),
                    wsdd_backend);

  wsdd_backend->root_monitor = g_vfs_monitor_new (job->backend);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GMountSpec *real_mount_spec;

  real_mount_spec = g_mount_spec_new ("wsdd");
  g_vfs_backend_set_mount_spec (backend, real_mount_spec);
  g_mount_spec_unref (real_mount_spec);

  g_vfs_wsdd_service_new (G_VFS_JOB (job)->cancellable,
                          try_mount_service_cb,
                          job);

  return TRUE;
}

static gboolean
try_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  GVfsBackendWsdd *wsdd_backend = G_VFS_BACKEND_WSDD (backend);
  g_autoptr (GError) error = NULL;
  GList *devices;
  GVfsWsddDevice *device;

  if (is_root (filename))
    {
      g_file_info_set_name (info, "/");
      g_file_info_set_display_name (info, g_vfs_backend_get_display_name (backend));

      g_file_info_set_icon (info, g_vfs_backend_get_icon (backend));
      g_file_info_set_symbolic_icon (info, g_vfs_backend_get_symbolic_icon (backend));

      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_content_type (info, "inode/directory");

      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE, TRUE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);

      g_vfs_job_succeeded (G_VFS_JOB (job));

      return TRUE;
    }

  devices = g_vfs_wsdd_service_get_devices (wsdd_backend->wsdd_service, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);

      return TRUE;
    }

  device = lookup_wsdd_device (devices, filename);
  if (device == NULL)
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        _("File not found"));

      return TRUE;
    }

  file_info_from_wsdd_device (wsdd_backend, device, info);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static gboolean
try_query_fs_info (GVfsBackend *backend,
                   GVfsJobQueryFsInfo *job,
                   const char *filename,
                   GFileInfo *info,
                   GFileAttributeMatcher *matcher)
{
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "wsdd");
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, TRUE);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_NEVER);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *matcher,
               GFileQueryInfoFlags flags)
{
  GVfsBackendWsdd *wsdd_backend = G_VFS_BACKEND_WSDD (backend);
  g_autoptr (GError) error = NULL;
  GList *devices;
  GList *l;

  devices = g_vfs_wsdd_service_get_devices (wsdd_backend->wsdd_service, &error);
  if (error != NULL)
    {
      g_vfs_job_failed_from_error (G_VFS_JOB (job), error);

      return TRUE;
    }

  if (!is_root (filename))
    {
      GVfsWsddDevice *device;

      device = lookup_wsdd_device (devices, filename);
      if (device == NULL)
        {
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            _("File not found"));
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job),
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_DIRECTORY,
                            _("File is not a directory"));
        }

      return TRUE;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));

  for (l = devices; l != NULL; l = l->next)
    {
      g_autoptr(GFileInfo) info = NULL;

      info = g_file_info_new ();
      file_info_from_wsdd_device (wsdd_backend, l->data, info);
      g_vfs_job_enumerate_add_info (job, info);
    }

  g_vfs_job_enumerate_done (job);

  return TRUE;
}

static gboolean
try_create_monitor (GVfsBackend *backend,
                    GVfsJobCreateMonitor *job,
                    const char *filename,
                    GFileMonitorFlags flags)
{
  GVfsBackendWsdd *wsdd_backend = G_VFS_BACKEND_WSDD (backend);
  g_autoptr (GError) error = NULL;

  if (!is_root (filename))
    {
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Operation not supported"));

      return TRUE;
    }

  g_vfs_job_create_monitor_set_monitor (job, wsdd_backend->root_monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static void
g_vfs_backend_wsdd_init (GVfsBackendWsdd *wsdd_backend)
{
  GVfsBackend *backend = G_VFS_BACKEND (wsdd_backend);

  g_vfs_backend_set_display_name (backend, _("WS-Discovery Network"));
  g_vfs_backend_set_icon_name (backend, "network-workgroup");
  g_vfs_backend_set_symbolic_icon_name (backend, "network-workgroup-symbolic");
  g_vfs_backend_set_user_visible (backend, FALSE);
}

static void
g_vfs_backend_wsdd_finalize (GObject *object)
{
  GVfsBackendWsdd *wsdd_backend = G_VFS_BACKEND_WSDD (object);

  if (wsdd_backend->wsdd_service != NULL)
    {
      g_signal_handlers_disconnect_by_data (wsdd_backend->wsdd_service,
                                            wsdd_backend);
      g_clear_object (&wsdd_backend->wsdd_service);
    }

  if (wsdd_backend->wsdd_resolver != NULL)
    {
      g_signal_handlers_disconnect_by_data (wsdd_backend->wsdd_resolver,
                                            wsdd_backend);
      g_clear_object (&wsdd_backend->wsdd_resolver);
    }

  g_clear_object (&wsdd_backend->root_monitor);

  G_OBJECT_CLASS (g_vfs_backend_wsdd_parent_class)->finalize (object);
}

static void
g_vfs_backend_wsdd_class_init (GVfsBackendWsddClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

  gobject_class->finalize = g_vfs_backend_wsdd_finalize;

  backend_class->try_mount = try_mount;
  backend_class->try_query_info = try_query_info;
  backend_class->try_query_fs_info = try_query_fs_info;
  backend_class->try_enumerate = try_enumerate;
  backend_class->try_create_dir_monitor = try_create_monitor;
  backend_class->try_create_file_monitor = try_create_monitor;
}

