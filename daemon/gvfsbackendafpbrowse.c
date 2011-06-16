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
};


G_DEFINE_TYPE (GVfsBackendAfpBrowse, g_vfs_backend_afp_browse, G_VFS_TYPE_BACKEND);


static gboolean
is_root (const char *filename)
{
  const char *p;

  p = filename;
  while (*p == '/')
    p++;

  return *p == 0;
}

static void
mount_get_srvr_parms_cb (GVfsAfpConnection *afp_connection,
                         GVfsAfpReply      *reply,
                         GError            *error,
                         gpointer           user_data)
{
  GVfsJobMountMountable *job = G_VFS_JOB_MOUNT_MOUNTABLE (user_data);
  GVfsBackendAfpBrowse *afp_backend = G_VFS_BACKEND_AFP_BROWSE (job->backend);

  AfpResultCode res_code;
  char *filename, *end;
  guint len;
  guint8 num_volumes, i;

  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_FAILED, _("Volume enumeration failed"));
    return;
  }

  filename = job->filename;
  
  while (*filename == '/')
    filename++;

  end = strchr (filename, '/');
  if (end)
  {
    len = end - filename;

    while (*end == '/')
      end++;

    if (*end != 0)
      return;
  }
  else
    len = strlen (filename);
  
  /* server time */
  (void)g_data_input_stream_read_int32 (G_DATA_INPUT_STREAM (reply), NULL, NULL);

  num_volumes = g_data_input_stream_read_byte (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  for (i = 0; i < num_volumes; i++)
  {
    char *vol_name;

    /* flags*/
    g_data_input_stream_read_byte (G_DATA_INPUT_STREAM (reply), NULL, NULL);
    
    vol_name = g_vfs_afp_reply_read_pascal (reply);
    if (!vol_name)
      continue;

    if (strlen (vol_name) == len && strncmp (vol_name, filename, len) == 0)
    {
      GMountSpec *mount_spec;
      
      mount_spec = g_mount_spec_new ("afp-volume");
      g_mount_spec_set (mount_spec, "host",
                        g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)));
      g_mount_spec_set (mount_spec, "volume", vol_name);

      g_vfs_job_mount_mountable_set_target (job, mount_spec, "/", TRUE);
      g_mount_spec_unref (mount_spec);
      
      g_vfs_job_succeeded (G_VFS_JOB (job));

      g_free (vol_name);
      return;
    }

    g_free (vol_name);
  }

  g_vfs_job_failed (G_VFS_JOB (job),  G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			 _("File doesn't exist"));
}

static gboolean
try_mount_mountable (GVfsBackend *backend,
		     GVfsJobMountMountable *job,
		     const char *filename,
		     GMountSource *mount_source)
{
  GVfsBackendAfpBrowse *afp_backend = G_VFS_BACKEND_AFP_BROWSE (backend);
  GVfsAfpCommand *comm;
  
  if (is_root (filename))
  {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_MOUNTABLE_FILE,
                      _("The file is not a mountable"));
    return TRUE;
  }

  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_SRVR_PARMS);
  /* pad byte */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);

  g_vfs_afp_connection_queue_command (afp_backend->server->conn, comm,
                                      mount_get_srvr_parms_cb,
                                      G_VFS_JOB (job)->cancellable, job);

  return TRUE;
                                      
}

static void
get_srvr_parms_cb (GVfsAfpConnection *afp_connection,
                   GVfsAfpReply      *reply,
                   GError            *error,
                   gpointer           user_data)
{
  GVfsJobEnumerate *job = G_VFS_JOB_ENUMERATE (user_data);
  GVfsBackendAfpBrowse *afp_backend = G_VFS_BACKEND_AFP_BROWSE (job->backend);

  AfpResultCode res_code;
  guint8 num_volumes, i;

  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  res_code = g_vfs_afp_reply_get_result_code (reply);
  if (res_code != AFP_RESULT_NO_ERROR)
  {
    g_vfs_job_failed_literal (G_VFS_JOB (job), G_IO_ERROR,
                              G_IO_ERROR_FAILED, _("Volume enumeration failed"));
    return;
  }

  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  /* server time */
  (void)g_data_input_stream_read_int32 (G_DATA_INPUT_STREAM (reply), NULL, NULL);

  num_volumes = g_data_input_stream_read_byte (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  for (i = 0; i < num_volumes; i++)
  {
    guint8 flags;
    char *vol_name;

    GFileInfo *info;
    GIcon *icon;
    GMountSpec *mount_spec;
    char *uri;

    flags = g_data_input_stream_read_byte (G_DATA_INPUT_STREAM (reply), NULL, NULL);
    vol_name = g_vfs_afp_reply_read_pascal (reply);
    if (!vol_name)
      continue;
    
    info = g_file_info_new ();
    
    g_file_info_set_name (info, vol_name);
    g_file_info_set_display_name (info, vol_name);
    g_file_info_set_edit_name (info, vol_name);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_STANDARD_IS_VIRTUAL, TRUE);
    g_file_info_set_content_type (info, "inode/directory");
    g_file_info_set_file_type (info, G_FILE_TYPE_MOUNTABLE);

    g_file_info_set_attribute_boolean (info, "afp::volume-password-protected", (flags & 0x01));

    icon = g_themed_icon_new_with_default_fallbacks ("folder-remote-afp");
    g_file_info_set_icon (info, icon);
    g_object_unref (icon);
    
    mount_spec = g_mount_spec_new ("afp-volume");
    g_mount_spec_set (mount_spec, "host",
                      g_network_address_get_hostname (G_NETWORK_ADDRESS (afp_backend->addr)));
    g_mount_spec_set (mount_spec, "volume", vol_name);

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
                           vol_name);
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,
                                      uri);
    g_free (uri);

    g_vfs_job_enumerate_add_info (job, info);
    g_object_unref (info);

    g_free (vol_name);
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

  GVfsAfpCommand *comm;
  
  if (!is_root(filename))
  {
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                      _("File doesn't exist"));
    return TRUE;
  }

  comm = g_vfs_afp_command_new (AFP_COMMAND_GET_SRVR_PARMS);
  /* pad byte */
  g_data_output_stream_put_byte (G_DATA_OUTPUT_STREAM (comm), 0, NULL, NULL);

  g_vfs_afp_connection_queue_command (afp_backend->server->conn, comm,
                                      get_srvr_parms_cb,
                                      G_VFS_JOB (job)->cancellable, job);
  g_object_unref (comm);

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
    g_vfs_job_succeeded (G_VFS_JOB (job));
  
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
  char       *display_name;

  afp_backend->server = g_vfs_afp_server_new (afp_backend->addr);

  res = g_vfs_afp_server_login (afp_backend->server, afp_backend->user, mount_source,
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

  if (afp_backend->user)
    display_name = g_strdup_printf (_("AFP shares for %s on %s"), afp_backend->user,
                                    afp_backend->server->server_name);
  else
    display_name = g_strdup_printf (_("AFP shares on %s"),
                                    afp_backend->server->server_name);
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
}

static void
g_vfs_backend_afp_browse_finalize (GObject *object)
{
  GVfsBackendAfpBrowse *afp_backend = G_VFS_BACKEND_AFP_BROWSE (object);

  g_object_unref (afp_backend->mount_tracker);

  if (afp_backend->addr)
    g_object_unref (afp_backend->addr);
  
  g_free (afp_backend->user);
  
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
