 /* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) Carl-Anton Ingmarsson 2011 <ca.ingmarsson@gmail.com>
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
 * Author: Carl-Anton Ingmarsson <ca.ingmarsson@gmail.com>
 */

#include <config.h>

#include <stdlib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>

#ifdef HAVE_GCRYPT
#include <gcrypt.h>
#endif

#include "gvfsjobmount.h"
#include "gvfsjobunmount.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobmountmountable.h"
#include "gmounttracker.h"

#include "gvfsafpserver.h"
#include "gvfsafpconnection.h"

#include "gvfsbackendafpbrowse.h"

struct _GVfsBackendAfpBrowseClass
{
  GVfsBackendClass parent_class;
};

struct _GVfsBackendAfpBrowse
{
  GVfsBackend parent_instance;

  GNetworkAddress    *addr;
  char               *user;

  GMountTracker      *mount_tracker;
  GVfsAfpServer      *server;

  char               *logged_in_user;
  GPtrArray          *volumes;
};


G_DEFINE_TYPE (GVfsBackendAfpBrowse, g_vfs_backend_afp_browse, G_VFS_TYPE_BACKEND);


static void
get_volumes_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpServer *server = G_VFS_AFP_SERVER (source_object);
  GTask *task = G_TASK (user_data);
  GVfsBackendAfpBrowse *afp_backend;
  GPtrArray *volumes;
  GError *err = NULL;

  afp_backend = G_VFS_BACKEND_AFP_BROWSE (g_task_get_source_object (task));

  volumes = g_vfs_afp_server_get_volumes_finish (server, res, &err);
  if (!volumes)
  {
    g_task_return_error (task, err);
    g_object_unref (task);
    return;
  }

  if (afp_backend->volumes)
    g_ptr_array_unref (afp_backend->volumes);
  afp_backend->volumes = volumes;

  g_task_return_boolean (task, TRUE);
  g_object_unref (task);
}

static void
update_cache (GVfsBackendAfpBrowse *afp_backend,
              GCancellable *cancellable,
              GAsyncReadyCallback callback,
              gpointer user_data)
{
  GTask *task;

  task = g_task_new (afp_backend, cancellable, callback, user_data);
  g_task_set_source_tag (task, update_cache);

  g_vfs_afp_server_get_volumes (afp_backend->server, cancellable, get_volumes_cb, task);
}

static gboolean
update_cache_finish (GVfsBackendAfpBrowse *afp_backend,
                     GAsyncResult         *res,
                     GError              **error)
{
  g_return_val_if_fail (g_task_is_valid (res, afp_backend), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (res, update_cache), FALSE);

  return g_task_propagate_boolean (G_TASK (res), error);
}

static GVfsAfpVolumeData *
find_volume (GVfsBackendAfpBrowse *afp_backend,
             char *filename)
{
  char *end;
  guint len;
  guint i;

  while (*filename == '/')
    filename++;

  end = strchr (filename, '/');
  if (end)
  {
    len = end - filename;

    while (*end == '/')
      end++;

    if (*end != 0)
      return NULL;
  }
  else
    len = strlen (filename);

  for (i = 0; i < afp_backend->volumes->len; i++)
  {
    GVfsAfpVolumeData *vol_data = g_ptr_array_index (afp_backend->volumes, i);

    if (strlen (vol_data->name) == len && strncmp (vol_data->name, filename, len) == 0)
      return vol_data;
  }

  return NULL;
}

static void
mount_mountable_cb (GObject      *source_object,        
                    GAsyncResult *res,
                    gpointer      user_data)
{
  GVfsBackendAfpBrowse *afp_backend = G_VFS_BACKEND_AFP_BROWSE (source_object);
  GVfsJobMountMountable *job = G_VFS_JOB_MOUNT_MOUNTABLE (user_data);

  GError *err;
  GVfsAfpVolumeData *vol_data;
  GMountSpec *mount_spec;

  if (!update_cache_finish (afp_backend, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  vol_data = find_volume (afp_backend, job->filename);
  if (!vol_data)
  {
    g_vfs_job_failed (G_VFS_JOB (job),  G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      _("File doesn’t exist"));
    return;
  }

  mount_spec = g_mount_spec_new ("afp-volume");
  g_mount_spec_set (mount_spec, "host",
                    g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)));
  g_mount_spec_set (mount_spec, "volume", vol_data->name);
  g_mount_spec_set (mount_spec, "user", afp_backend->logged_in_user);

  g_vfs_job_mount_mountable_set_target (job, mount_spec, "/", TRUE);
  g_mount_spec_unref (mount_spec);

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_mount_mountable (GVfsBackend *backend,
                     GVfsJobMountMountable *job,
                     const char *filename,
                     GMountSource *mount_source)
{
  GVfsBackendAfpBrowse *afp_backend = G_VFS_BACKEND_AFP_BROWSE (backend);
  
  if (is_root (filename))
  {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_MOUNTABLE_FILE,
                      _("Not a mountable file"));
    return TRUE;
  }

  update_cache (afp_backend, G_VFS_JOB (job)->cancellable, mount_mountable_cb, job);

  return TRUE;
                                      
}

static void
fill_info (GFileInfo *info, GVfsAfpVolumeData *vol_data, GVfsBackendAfpBrowse *afp_backend)
{
  GIcon *icon;
  GMountSpec *mount_spec;
  char *uri;

  g_file_info_set_name (info, vol_data->name);
  g_file_info_set_display_name (info, vol_data->name);
  g_file_info_set_edit_name (info, vol_data->name);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_VIRTUAL, TRUE);
  g_file_info_set_content_type (info, "inode/directory");
  g_file_info_set_file_type (info, G_FILE_TYPE_MOUNTABLE);

  g_file_info_set_attribute_boolean (info, "afp::volume-password-protected", (vol_data->flags & 0x01));

  icon = g_themed_icon_new_with_default_fallbacks ("folder-remote-afp");
  g_file_info_set_icon (info, icon);
  g_object_unref (icon);

  icon = g_themed_icon_new_with_default_fallbacks ("folder-remote-symbolic");
  g_file_info_set_symbolic_icon (info, icon);
  g_object_unref (icon);

  mount_spec = g_mount_spec_new ("afp-volume");
  g_mount_spec_set (mount_spec, "host",
                    g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)));
  g_mount_spec_set (mount_spec, "volume", vol_data->name);
  g_mount_spec_set (mount_spec, "user", afp_backend->logged_in_user);

  if (g_mount_tracker_has_mount_spec (afp_backend->mount_tracker, mount_spec))
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT, FALSE);
  else
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT, TRUE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_UNMOUNT, FALSE);
  g_mount_spec_unref (mount_spec);

  uri = g_strdup_printf ("afp://%s/%s",
                         g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)),
                         vol_data->name);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,
                                    uri);
  g_free (uri);
}

static void
enumerate_cache_updated_cb (GObject      *source_object,
                            GAsyncResult *res,
                            gpointer      user_data)
{
  GVfsBackendAfpBrowse *afp_backend = G_VFS_BACKEND_AFP_BROWSE (source_object);
  GVfsJobEnumerate *job = G_VFS_JOB_ENUMERATE (user_data);

  GError *err = NULL;
  guint i;

  if (!update_cache_finish (afp_backend, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  for (i = 0; i < afp_backend->volumes->len; i++)
  {
    GVfsAfpVolumeData *vol_data = g_ptr_array_index (afp_backend->volumes, i);
    
    GFileInfo *info;

    info = g_file_info_new ();
    fill_info (info, vol_data, afp_backend);
    g_vfs_job_enumerate_add_info (job, info);
    
    g_object_unref (info);
  }

  g_vfs_job_enumerate_done (job);
}

static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  GVfsBackendAfpBrowse *afp_backend = G_VFS_BACKEND_AFP_BROWSE (backend);

  if (!is_root(filename))
  {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      _("File doesn’t exist"));
    return TRUE;
  }

  update_cache (afp_backend, G_VFS_JOB (job)->cancellable,
                enumerate_cache_updated_cb, job);
  
  return TRUE;
}

static void
query_info_cb (GObject      *source_object,
               GAsyncResult *res,
               gpointer      user_data)
{
  GVfsBackendAfpBrowse *afp_backend = G_VFS_BACKEND_AFP_BROWSE (source_object);
  GVfsJobQueryInfo *job = G_VFS_JOB_QUERY_INFO (user_data);

  GError *err = NULL;
  GVfsAfpVolumeData *vol_data;

  if (!update_cache_finish (afp_backend, res, &err))
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
    g_error_free (err);
    return;
  }

  vol_data = find_volume (afp_backend, job->filename);
  if (!vol_data)
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job),  G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("File doesn’t exist"));
    return;
  }

  fill_info (job->file_info, vol_data, afp_backend);
  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static gboolean
try_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  if (is_root (filename))
  {
    GIcon *icon;
    
    g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
    g_file_info_set_name (info, "/");
    g_file_info_set_display_name (info, g_vfs_backend_get_display_name (backend));
    g_file_info_set_content_type (info, "inode/directory");
    icon = g_vfs_backend_get_icon (backend);
    if (icon != NULL)
      g_file_info_set_icon (info, icon);

    icon = g_vfs_backend_get_symbolic_icon (backend);
    if (icon != NULL)
      g_file_info_set_symbolic_icon (info, icon);

    g_vfs_job_succeeded (G_VFS_JOB (job));
  }
  else
    update_cache (G_VFS_BACKEND_AFP_BROWSE (backend), G_VFS_JOB (job)->cancellable,
                  query_info_cb, job);
  
  return TRUE;
}

static void
do_unmount (GVfsBackend *backend,
            GVfsJobUnmount *job,
            GMountUnmountFlags flags,
            GMountSource *mount_source)
{
  GVfsBackendAfpBrowse *afp_backend = G_VFS_BACKEND_AFP_BROWSE (backend);

  if (!(flags & G_MOUNT_UNMOUNT_FORCE))
  {
    g_vfs_afp_server_logout_sync (afp_backend->server, G_VFS_JOB (job)->cancellable,
                                  NULL);
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  GVfsBackendAfpBrowse *afp_backend = G_VFS_BACKEND_AFP_BROWSE (backend);

  gboolean res;
  GError *err = NULL;

  const GVfsAfpServerInfo *info;
  GMountSpec *afp_mount_spec;
  char       *server_name;
  char       *display_name;

  afp_backend->server = g_vfs_afp_server_new (afp_backend->addr);

  res = g_vfs_afp_server_login (afp_backend->server, afp_backend->user, mount_source,
                                &afp_backend->logged_in_user,
                                G_VFS_JOB (job)->cancellable, &err);
  if (!res)
    goto error;
  
  /* set mount info */
  afp_mount_spec = g_mount_spec_new ("afp-server");
  g_mount_spec_set (afp_mount_spec, "host",
                    g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)));
  if (afp_backend->user)
    g_mount_spec_set (afp_mount_spec, "user", afp_backend->user);

  g_vfs_backend_set_mount_spec (backend, afp_mount_spec);
  g_mount_spec_unref (afp_mount_spec);

  info = g_vfs_afp_server_get_info (afp_backend->server);
  
  if (info->utf8_server_name)
    server_name = info->utf8_server_name;
  else
    server_name = info->server_name;
  
  if (afp_backend->user)
    /* Translators: first %s is username and second serververname */
    display_name = g_strdup_printf (_("%s on %s"), afp_backend->user,
                                    server_name);
  else
    /* Translators: %s is the servername */
    display_name = g_strdup_printf (_("%s"),
                                    server_name);
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);

  g_vfs_backend_set_icon_name (backend, "network-server-afp");
  g_vfs_backend_set_user_visible (backend, FALSE);

    
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return;

error:
  g_vfs_job_failed_from_error (G_VFS_JOB (job), err);
}
  
static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendAfpBrowse *afp_backend = G_VFS_BACKEND_AFP_BROWSE (backend);

  const char *host, *portstr, *user;
  guint16 port = 548;

  host = g_mount_spec_get (mount_spec, "host");
  if (host == NULL)
  {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                      _("No hostname specified"));
    return TRUE;
  }

  portstr = g_mount_spec_get (mount_spec, "port");
  if (portstr != NULL)
  {
    port = atoi (portstr);
  }

  afp_backend->addr = G_NETWORK_ADDRESS (g_network_address_new (host, port));

  user = g_mount_spec_get (mount_spec, "user");
  afp_backend->user = g_strdup (user);

  return FALSE;
}

static void
g_vfs_backend_afp_browse_init (GVfsBackendAfpBrowse *object)
{
  GVfsBackendAfpBrowse *afp_backend = G_VFS_BACKEND_AFP_BROWSE (object);

  afp_backend->mount_tracker = g_mount_tracker_new (NULL, FALSE);

  afp_backend->addr = NULL;
  afp_backend->user = NULL;

  afp_backend->logged_in_user = NULL;
  afp_backend->volumes = NULL;
}

static void
g_vfs_backend_afp_browse_finalize (GObject *object)
{
  GVfsBackendAfpBrowse *afp_backend = G_VFS_BACKEND_AFP_BROWSE (object);

  g_object_unref (afp_backend->mount_tracker);

  if (afp_backend->addr)
    g_object_unref (afp_backend->addr);
  
  g_free (afp_backend->user);

  g_free (afp_backend->logged_in_user);
  if (afp_backend->volumes)
    g_ptr_array_unref (afp_backend->volumes);

  G_OBJECT_CLASS (g_vfs_backend_afp_browse_parent_class)->finalize (object);
}

static gboolean
try_query_fs_info (GVfsBackend *backend,
                   GVfsJobQueryFsInfo *job,
                   const char *filename,
                   GFileInfo *info,
                   GFileAttributeMatcher *matcher)
{
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "afp");
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, TRUE);
  g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USE_PREVIEW, G_FILESYSTEM_PREVIEW_TYPE_NEVER);
  g_vfs_job_succeeded (G_VFS_JOB (job));
  return TRUE;
}

static void
g_vfs_backend_afp_browse_class_init (GVfsBackendAfpBrowseClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

  object_class->finalize = g_vfs_backend_afp_browse_finalize;

  backend_class->try_mount = try_mount;
  backend_class->mount = do_mount;
  backend_class->unmount = do_unmount;
  backend_class->try_query_info = try_query_info;
  backend_class->try_enumerate = try_enumerate;
  backend_class->try_mount_mountable = try_mount_mountable;
  backend_class->try_query_fs_info = try_query_fs_info;
}

void
g_vfs_afp_browse_daemon_init (void)
{
  g_set_application_name (_("Apple Filing Protocol Service"));

#ifdef HAVE_GCRYPT
  gcry_check_version (NULL);
  gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
#endif
}
