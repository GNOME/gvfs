/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
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
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gthemedicon.h>
#include <gio/gsimpleasyncresult.h>
#include "gdaemonvolumemonitor.h"
#include "gdaemonvolume.h"
#include "gvfsdaemondbus.h"
#include "gdaemonfile.h"
#include "gvfsdaemonprotocol.h"

struct _GDaemonVolume {
  GObject     parent;

  GMountInfo *mount_info;
};

static void g_daemon_volume_volume_iface_init (GVolumeIface *iface);

G_DEFINE_TYPE_WITH_CODE (GDaemonVolume, g_daemon_volume, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME,
						g_daemon_volume_volume_iface_init))


static void
g_daemon_volume_finalize (GObject *object)
{
  GDaemonVolume *volume;
  
  volume = G_DAEMON_VOLUME (object);

  g_mount_info_unref (volume->mount_info);
  
  if (G_OBJECT_CLASS (g_daemon_volume_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_volume_parent_class)->finalize) (object);
}

static void
g_daemon_volume_class_init (GDaemonVolumeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_daemon_volume_finalize;
}

static void
g_daemon_volume_init (GDaemonVolume *daemon_volume)
{
}

GDaemonVolume *
g_daemon_volume_new (GMountInfo *mount_info)
{
  GDaemonVolume *volume;

  volume = g_object_new (G_TYPE_DAEMON_VOLUME, NULL);
  volume->mount_info = g_mount_info_ref (mount_info);

  return volume;
}

GMountInfo *
g_daemon_volume_get_mount_info (GDaemonVolume *volume)
{
  return volume->mount_info;
}

static GFile *
g_daemon_volume_get_root (GVolume *volume)
{
  GDaemonVolume *daemon_volume = G_DAEMON_VOLUME (volume);

  return g_daemon_file_new (daemon_volume->mount_info->mount_spec, "/");
}

static GIcon *
g_daemon_volume_get_icon (GVolume *volume)
{
  GDaemonVolume *daemon_volume = G_DAEMON_VOLUME (volume);

  return g_themed_icon_new (daemon_volume->mount_info->icon);
}

static char *
g_daemon_volume_get_name (GVolume *volume)
{
  GDaemonVolume *daemon_volume = G_DAEMON_VOLUME (volume);

  return g_strdup (daemon_volume->mount_info->display_name);
}

static GDrive *
g_daemon_volume_get_drive (GVolume *volume)
{
  return NULL;
}

static gboolean
g_daemon_volume_can_unmount (GVolume *volume)
{
  return TRUE;
}

static gboolean
g_daemon_volume_can_eject (GVolume *volume)
{
  return FALSE;
}

static void
unmount_reply (DBusMessage *reply,
	       DBusConnection *connection,
	       GError *io_error,
	       gpointer _data)
{
  GSimpleAsyncResult *result = _data;

  if (io_error != NULL)
    g_simple_async_result_set_from_error (result, io_error);
  
  g_simple_async_result_complete (result);
  g_object_unref (result);
}

static void
g_daemon_volume_unmount (GVolume *volume,
			 GCancellable *cancellable,
			 GAsyncReadyCallback callback,
			 gpointer         user_data)
{
  GDaemonVolume *daemon_volume = G_DAEMON_VOLUME (volume);
  DBusMessage *message;
  GMountInfo *mount_info;
  GSimpleAsyncResult *res;

  mount_info = daemon_volume->mount_info;
  
  message =
    dbus_message_new_method_call (mount_info->dbus_id,
				  mount_info->object_path,
				  G_VFS_DBUS_MOUNT_INTERFACE,
				  G_VFS_DBUS_MOUNT_OP_UNMOUNT);

  res = g_simple_async_result_new (G_OBJECT (volume),
				   callback, user_data,
				   g_daemon_volume_unmount);
  
  _g_vfs_daemon_call_async (message,
			    unmount_reply, res,
			    cancellable);
  
  dbus_message_unref (message);
}

static gboolean
g_daemon_volume_unmount_finish (GVolume *volume,
				GAsyncResult *result,
				GError **error)
{
  return TRUE;
}


static void
g_daemon_volume_volume_iface_init (GVolumeIface *iface)
{
  iface->get_root = g_daemon_volume_get_root;
  iface->get_name = g_daemon_volume_get_name;
  iface->get_icon = g_daemon_volume_get_icon;
  iface->get_drive = g_daemon_volume_get_drive;
  iface->can_unmount = g_daemon_volume_can_unmount;
  iface->can_eject = g_daemon_volume_can_eject;
  iface->unmount = g_daemon_volume_unmount;
  iface->unmount_finish = g_daemon_volume_unmount_finish;
}
