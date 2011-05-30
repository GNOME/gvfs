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


#include "gvfsbackendafp.h"



G_DEFINE_TYPE (GVfsBackendAfp, g_vfs_backend_afp, G_VFS_TYPE_BACKEND);

static void
do_unmount (GVfsBackend *   backend,
            GVfsJobUnmount *job,
            GMountUnmountFlags flags,
            GMountSource *mount_source)
{
  GVfsBackendAfp *afp = G_VFS_BACKEND_AFP (backend);

  g_object_unref (afp->conn);
}

static void
do_mount (GVfsBackend *backend,
          GVfsJobMount *job,
          GMountSpec *mount_spec,
          GMountSource *mount_source,
          gboolean is_automount)
{
  GVfsBackendAfp *afp_backend = G_VFS_BACKEND_AFP (backend);

  GError *err;
  GVfsAfpCommand *command;
  GVfsAfpReply *reply;

  guint16 MachineType_offset, AFPVersionCount_offset, UAMCount_offset;
  guint16 VolumeIconAndMask_offset, Flags;
  char *ServerName;

  /* UAM */
  guint8 UAMCount;
  GSList *UAMS = NULL;
  guint i;

  err = NULL;
  afp_backend->conn = g_vfs_afp_connection_new (afp_backend->addr,
                                                G_VFS_JOB (job)->cancellable,
                                                &err);
  if (!afp_backend->conn)
    goto error;

  command = g_vfs_afp_command_new (AFP_COMMAND_GET_SRVR_INFO);
  if (!g_vfs_afp_connection_send_command_sync (afp_backend->conn, command,
                                               G_VFS_JOB (job)->cancellable, &err))
    goto error;

  reply = g_vfs_afp_connection_read_reply_sync (afp_backend->conn,
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
  
  ServerName = g_vfs_afp_reply_read_pascal (reply);
  g_debug ("ServerName: %s\n", ServerName);
  g_free (ServerName);

  /* Parse Versions */
  g_vfs_afp_reply_seek (reply, AFPVersionCount_offset, G_SEEK_SET);
  UAMCount =
    g_data_input_stream_read_byte (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  g_debug ("Versions\n");
  for (i = 0; i < UAMCount; i++)
  {
    char *version;

    version = g_vfs_afp_reply_read_pascal (reply);
    g_debug ("%d. %s\n",i + 1, version);
    g_free (version);
  }
  
  /* Parse UAMs */
  g_vfs_afp_reply_seek (reply, UAMCount_offset, G_SEEK_SET);
  UAMCount =
    g_data_input_stream_read_byte (G_DATA_INPUT_STREAM (reply), NULL, NULL);
  for (i = 0; i < UAMCount; i++)
  {
    char *UAM;

    UAM = g_vfs_afp_reply_read_pascal (reply);
    UAMS = g_slist_prepend (UAMS, UAM);
  }

  g_object_unref (reply);


  /* Anonymous login */
  if (!afp_backend->user)
  {
    if (!g_slist_find_custom (UAMS, "No User Authent", g_str_equal))
    {
      g_slist_free_full (UAMS, g_free);
      
      g_vfs_job_failed (G_VFS_JOB (job),
			  G_IO_ERROR, G_IO_ERROR_FAILED,
			  _("Failed to mount AFP share (%s)"), "Server doesn't support anonymous login");
      return;
    }

    
  }
  else
  {
    if (g_slist_find_custom (UAMS, "DHX2", g_str_equal))
    {
      g_debug ("Diffie-Hellman 2\n");
    }

    else if (g_slist_find_custom (UAMS, "DHCAST128", g_str_equal))
    {
      g_debug ("Diffie-Hellman");
    }

    else if (g_slist_find_custom (UAMS, "No User Authent", g_str_equal))
    {
      g_debug ("Anonymous login");
    }

    else
    {
      g_slist_free_full (UAMS, g_free);
      
      g_vfs_job_failed (G_VFS_JOB (job),
                        G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("Failed to mount AFP share (%s)"),
                        "No suitable authentication mechanism found");
      return;
    } 
  }

  g_slist_free_full (UAMS, g_free);
  
  g_vfs_job_succeeded (G_VFS_JOB (job));

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
	/* TODO: Add initialization code here */
}

static void
g_vfs_backend_afp_finalize (GObject *object)
{
	/* TODO: Add deinitalization code here */

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
  backend_class->unmount = do_unmount;
}

