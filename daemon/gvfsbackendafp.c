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
#include <glib/gi18n.h>
#include <gio/gio.h>

#include "gvfsjobmount.h"


#include "gvfsbackendafp.h"



G_DEFINE_TYPE (GVfsBackendAfp, g_vfs_backend_afp, G_VFS_TYPE_BACKEND);

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
}

