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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
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

  GSList *volumes;
};


G_DEFINE_TYPE (GVfsBackendAfpBrowse, g_vfs_backend_afp_browse, G_VFS_TYPE_BACKEND);


typedef struct
{
  char *name;
  guint16 flags;
} VolumeData;

static void
volume_data_free (VolumeData *vol_data)
{
  g_free (vol_data->name);
  g_slice_free (VolumeData, vol_data);
}

static gboolean
is_root (const char *filename)
{
  const char *p;

  p = filename;
  while (*p == '/')
    p++;

  return *p == 0;
}
typedef void (*UpdateCacheCb) (GVfsBackendAfpBrowse *afp_backend,
                                 GError               *error,
                                 gpointer              user_data);

typedef struct
{
  GVfsBackendAfpBrowse *afp_backend;
  gpointer user_data;
  UpdateCacheCb cb;
} UpdateCacheData;

static void
get_srvr_parms_cb (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  GVfsAfpConnection *afp_conn = G_VFS_AFP_CONNECTION (source_object);
  UpdateCacheData *data = (UpdateCacheData *)user_data;

  GVfsAfpReply *reply;
  GError *err = NULL;
  AfpResultCode res_code;
  
  guint8 num_volumes, i;

  reply = g_vfs_afp_connection_send_command_finish (afp_conn, res, &err);
  if (!reply)
  {
    data->cb (data->afp_backend, err, data->user_data);

    g_error_free (err);
    g_slice_free (UpdateCacheData, data);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_object_unref (reply);
    
    err = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                       _("Got error code: %d from server"), res_code);
    data->cb (data->afp_backend, err, data->user_data);

    g_error_free (err);
    g_slice_free (UpdateCacheData, data);
    return;
  }
  
  /* server time */
  g_vfs_afp_reply_read_int32 (reply, NULL);

  g_slist_free_full (data->afp_backend->volumes, (GDestroyNotify) volume_data_free);
  data->afp_backend->volumes = NULL;

  g_vfs_afp_reply_read_byte (reply, &num_volumes);
  for (i = 0; i < num_volumes; i++)
  {
    guint8 flags;
    char *vol_name;

    VolumeData *volume_data;

    g_vfs_afp_reply_read_byte (reply, &flags);
    g_vfs_afp_reply_read_pascal (reply, &vol_name);
    if (!vol_name)
      continue;

    volume_data = g_slice_new (VolumeData);
    volume_data->flags = flags;
    volume_data->name = vol_name;

    data->afp_backend->volumes = g_slist_prepend (data->afp_backend->volumes, volume_data);
  }
  g_object_unref (reply);

  data->cb (data->afp_backend, NULL, data->user_data);
  g_slice_free (UpdateCacheData, data);
}

static void
update_cache (GVfsBackendAfpBrowse *afp_backend, GCancellable *cancellable,
              UpdateCacheCb cb, gpointer user_data)
{
  GVfsAfpCommand *comm;
  UpdateCacheData *data;

  data = g_slice_new (UpdateCacheData);
  data->afp_backend = afp_backend;
  data->cb = cb;
  data->user_data = user_data;
  
  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_SRVR_PARMS);
  /* pad byte */
  g_vfs_afp_command_put_byte (comm, 0);
  
  g_vfs_afp_connection_send_command (afp_backend->server->conn, comm,
                                     get_srvr_parms_cb,
                                     cancellable, data);
  g_object_unref (comm); 
}

static VolumeData *
find_volume (GVfsBackendAfpBrowse *afp_backend,
             char *filename)
{
  char *end;
  guint len;
  GSList *l;

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

  for (l = afp_backend->volumes; l; l = g_slist_next (l))
  {
    VolumeData *vol_data = (VolumeData *)l->data;

    if (strlen (vol_data->name) == len && strncmp (vol_data->name, filename, len) == 0)
      return vol_data;
  }

  return NULL;
}
static void
mount_mountable_cb (GVfsBackendAfpBrowse *afp_backend,
                    GError *error,
                    gpointer user_data)
{
  GVfsJobMountMountable *job = G_VFS_JOB_MOUNT_MOUNTABLE (user_data);

  VolumeData *vol_data;
  GMountSpec *mount_spec;
  
  if (error != NULL)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  vol_data = find_volume (afp_backend, job->filename);
  if (!vol_data)
  {
    g_vfs_job_failed (G_VFS_JOB (job),  G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      _("File doesn't exist"));
    return;
  }

  mount_spec = g_mount_spec_new ("afp-volume");
  g_mount_spec_set (mount_spec, "host",
                    g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)));
  g_mount_spec_set (mount_spec, "volume", vol_data->name);

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
                      _("The file is not a mountable"));
    return TRUE;
  }

  update_cache (afp_backend, G_VFS_JOB (job)->cancellable, mount_mountable_cb, job);

  return TRUE;
                                      
}

static void
fill_info (GFileInfo *info, VolumeData *vol_data, GVfsBackendAfpBrowse *afp_backend)
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

  mount_spec = g_mount_spec_new ("afp-volume");
  g_mount_spec_set (mount_spec, "host",
                    g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)));
  g_mount_spec_set (mount_spec, "volume", vol_data->name);

  if (g_mount_tracker_has_mount_spec (afp_backend->mount_tracker, mount_spec))
  {
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_UNMOUNT, TRUE);
  }
  else
  {
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT, TRUE);
    g_file_info_set_attribute_boolean(info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_UNMOUNT, FALSE);
  }
  g_mount_spec_unref (mount_spec);

  uri = g_strdup_printf ("afp://%s/%s",
                         g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)),
                         vol_data->name);
  g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,
                                    uri);
  g_free (uri);
}

static void
enumerate_cache_updated_cb (GVfsBackendAfpBrowse *afp_backend,
                            GError *error,
                            gpointer user_data)
{
  GVfsJobEnumerate *job = G_VFS_JOB_ENUMERATE (user_data);

  GSList *l;

  if (error != NULL)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  for (l = afp_backend->volumes; l; l = l->next)
  {
    VolumeData *vol_data = l->data;
    
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
                      _("File doesn't exist"));
    return TRUE;
  }

  update_cache (afp_backend, G_VFS_JOB (job)->cancellable,
                enumerate_cache_updated_cb, job);
  
  return TRUE;
}

static void
query_info_cb (GVfsBackendAfpBrowse *afp_backend,
               GError *error,
               gpointer user_data)
{
  GVfsJobQueryInfo *job = G_VFS_JOB_QUERY_INFO (user_data);

  VolumeData *vol_data;

  if (error != NULL)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  vol_data = find_volume (afp_backend, job->filename);
  if (!vol_data)
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job),  G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                              _("File doesn't exist"));
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
    g_vfs_job_succeeded (G_VFS_JOB (job));
  }
  else
    update_cache (G_VFS_BACKEND_AFP_BROWSE (backend), G_VFS_JOB (job)->cancellable,
                  query_info_cb, job);
  
  return TRUE;
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

  GMountSpec *afp_mount_spec;
  char       *server_name;
  char       *display_name;

  afp_backend->server = g_vfs_afp_server_new (afp_backend->addr);

  res = g_vfs_afp_server_login (afp_backend->server, afp_backend->user, mount_source,
                                NULL, G_VFS_JOB (job)->cancellable, &err);
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

  if (afp_backend->server->utf8_server_name)
    server_name = afp_backend->server->utf8_server_name;
  else
    server_name = afp_backend->server->server_name;
  
  if (afp_backend->user)
    display_name = g_strdup_printf (_("AFP shares for %s on %s"), afp_backend->user,
                                    server_name);
  else
    display_name = g_strdup_printf (_("AFP shares on %s"),
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

  afp_backend->mount_tracker = g_mount_tracker_new (NULL);

  afp_backend->addr = NULL;
  afp_backend->user = NULL;

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

  g_slist_free_full (afp_backend->volumes, (GDestroyNotify)volume_data_free);

  G_OBJECT_CLASS (g_vfs_backend_afp_browse_parent_class)->finalize (object);
}

static void
g_vfs_backend_afp_browse_class_init (GVfsBackendAfpBrowseClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

  object_class->finalize = g_vfs_backend_afp_browse_finalize;

  backend_class->try_mount = try_mount;
  backend_class->mount = do_mount;
  backend_class->try_query_info = try_query_info;
  backend_class->try_enumerate = try_enumerate;
  backend_class->try_mount_mountable = try_mount_mountable;
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
