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

#include "gvfsjobmount.h"
#include "gvfsjobenumerate.h"


#include "gvfsbackendafp.h"



G_DEFINE_TYPE (GVfsBackendAfp, g_vfs_backend_afp, G_VFS_TYPE_BACKEND);


#define AFP_UAM_NO_USER   "No User Authent"
#define AFP_UAM_DHX       "DHCAST128"
#define AFP_UAM_DHX2      "DHX2"

static const char *
afp_version_to_string (AfpVersion afp_version)
{
  const char *version_strings[] = { "AFPX03", "AFP3.1", "AFP3.2", "AFP3.3" };

  return version_strings[afp_version - 1];
}

static AfpVersion
string_to_afp_version (const char *str)
{
  gint i;
  
  const char *version_strings[] = { "AFPX03", "AFP3.1", "AFP3.2", "AFP3.3" };

  for (i = 0; i < G_N_ELEMENTS (version_strings); i++)
  {
    if (g_str_equal (str, version_strings[i]))
      return i + 1;
  }

  return AFP_VERSION_INVALID;
}

static void
get_srvr_parms_cb (GVfsAfpConnection *afp_connection,
                   GVfsAfpReply      *reply,
                   GError            *error,
                   gpointer           user_data)
{
  GVfsJobEnumerate *job = G_VFS_JOB_ENUMERATE (user_data);
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (job->backend);

  AfpErrorCode error_code;
  guint8 num_volumes, i;

  if (!reply)
  {
    g_vfs_job_failed_from_error (G_VFS_JOB (job), error);
    return;
  }

  error_code = g_vfs_afp_reply_get_error_code (reply);
  if (error_code != AFP_ERROR_NONE)
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

    icon = g_themed_icon_new ("folder-remote");
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
  }

  g_vfs_job_enumerate_done (job);
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

static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

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

  g_vfs_afp_connection_queue_command (afp_backend->conn, comm,
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

static gboolean
do_login (GVfsBackendAfp *afp_backend,
          const char *username,
          const char *password,
          GCancellable *cancellable,
          GError **error)
{
  /* anonymous login */
  if (!username)
  {
    GVfsAfpCommand *comm;
    GVfsAfpReply *reply;
    guint32 error_code;
    
    if (!g_slist_find_custom (afp_backend->uams, AFP_UAM_NO_USER, g_str_equal))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                           _("Server \"%s\" doesn't support anonymous login"),
                   afp_backend->server_name);
      return FALSE;
    }

    comm = g_vfs_afp_command_new (AFP_COMMAND_LOGIN);

    g_vfs_afp_command_put_pascal (comm,
                                  afp_version_to_string (afp_backend->version));
    g_vfs_afp_command_put_pascal (comm, AFP_UAM_NO_USER);
    if (!g_vfs_afp_connection_send_command_sync (afp_backend->conn, comm,
                                                 cancellable, error))
      return FALSE;

    reply = g_vfs_afp_connection_read_reply_sync (afp_backend->conn, cancellable, error);
    if (!reply)
      return FALSE;

    error_code = g_vfs_afp_reply_get_error_code (reply);
    g_object_unref (reply);
    
    if (error_code != AFP_ERROR_NONE)
    {
      if (error_code == AFP_ERROR_USER_NOT_AUTH)
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                     _("Anonymous login on server \"%s\" failed"),
                     afp_backend->server_name);
        return FALSE;
      }
      else
      {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     _("Login to server \"%s\" failed"), afp_backend->server_name);
        return FALSE;
      }
    }
  }

  else {

    /* Diffie-Hellman 2 */
    if (g_slist_find_custom (afp_backend->uams, AFP_UAM_DHX2, g_str_equal))
    {
      g_debug ("Diffie-Hellman 2\n");
    }

    /* Diffie-Hellman */
    else if (g_slist_find_custom (afp_backend->uams, AFP_UAM_DHX, g_str_equal))
    {
      g_debug ("Diffie-Hellman\n");
    }

    else
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   _("Login to server \"%s\" failed, no suitable authentication mechanism found"),
                   afp_backend->server_name);
      return FALSE;
    }
  }

  return TRUE;
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  gboolean res;
  GError *err = NULL;
  GVfsAfpReply *reply;

  guint16 MachineType_offset, AFPVersionCount_offset, UAMCount_offset;
  guint16 VolumeIconAndMask_offset, Flags;
  guint8 count;
  guint i;

  GMountSpec *afp_mount_spec;
  char       *display_name;

  afp_backend->conn = g_vfs_afp_connection_new (afp_backend->addr);
  
  reply = g_vfs_afp_connection_get_server_info (afp_backend->conn,
                                                G_VFS_JOB (job)->cancellable,
                                                &err);
  if (!reply)
    goto error;
  
  MachineType_offset =
    g_data_input_stream_read_uint16 (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  AFPVersionCount_offset = 
    g_data_input_stream_read_uint16 (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  UAMCount_offset =
    g_data_input_stream_read_uint16 (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  VolumeIconAndMask_offset =
    g_data_input_stream_read_uint16 (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  Flags =
    g_data_input_stream_read_uint16 (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  
  afp_backend->server_name = g_vfs_afp_reply_read_pascal (reply);

  /* Parse Versions */
  g_vfs_afp_reply_seek (reply, AFPVersionCount_offset, G_SEEK_SET);
  count = g_data_input_stream_read_byte (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  for (i = 0; i < count; i++)
  {
    char *version;
    AfpVersion afp_version;

    version = g_vfs_afp_reply_read_pascal (reply);
    afp_version = string_to_afp_version (version);
    if (afp_version > afp_backend->version)
      afp_backend->version = afp_version;
  }

  if (afp_backend->version == AFP_VERSION_INVALID)
  {
    g_object_unref (reply);
    g_vfs_job_failed (G_VFS_JOB (job),
                      G_IO_ERROR, G_IO_ERROR_FAILED,
                      _("Failed to connect to server (%s)"), "Server doesn't support AFP version 3.0 or later");
    return;
  }
  
  /* Parse UAMs */
  g_vfs_afp_reply_seek (reply, UAMCount_offset, G_SEEK_SET);
  count = g_data_input_stream_read_byte (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  for (i = 0; i < count; i++)
  {
    char *uam;

    uam = g_vfs_afp_reply_read_pascal (reply);
    afp_backend->uams = g_slist_prepend (afp_backend->uams, uam);
  }
  g_object_unref (reply);


  /* Open connection */
  res = g_vfs_afp_connection_open (afp_backend->conn, G_VFS_JOB (job)->cancellable,
                                   &err);
  if (!res)
    goto error;

  res = do_login (afp_backend, NULL, NULL, G_VFS_JOB (job)->cancellable,
                  &err);
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
  
  display_name = g_strdup_printf (_("AFP shares on %s"), afp_backend->server_name);
  g_vfs_backend_set_display_name (backend, display_name);
  g_free (display_name);

  g_vfs_backend_set_icon_name (backend, "network-server");
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
	GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);
	
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

	afp_backend->addr = g_network_address_new (host, port);
	
	user = g_mount_spec_get (mount_spec, "user");
	afp_backend->user = g_strdup (user);
	
	return FALSE;
}

static void
g_vfs_backend_afp_init (GVfsBackendAfp *object)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (object);

  afp_backend->mount_tracker = g_mount_tracker_new (NULL);
  
  afp_backend->server_name = NULL;
  afp_backend->uams = NULL;
  afp_backend->version = AFP_VERSION_INVALID;
}

static void
g_vfs_backend_afp_finalize (GObject *object)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (object);
  
  g_free (afp_backend->server_name);
  g_slist_free_full (afp_backend->uams, g_free);
  
	G_OBJECT_CLASS (g_vfs_backend_afp_parent_class)->finalize (object);
}

static void
g_vfs_backend_afp_class_init (GVfsBackendAfpClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);

	object_class->finalize = g_vfs_backend_afp_finalize;

	backend_class->try_mount = try_mount;
  backend_class->mount = do_mount;
  backend_class->try_query_info = try_query_info;
  backend_class->try_enumerate = try_enumerate;
}

