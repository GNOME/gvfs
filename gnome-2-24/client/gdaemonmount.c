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
#include <gio/gio.h>
#include "gdaemonvolumemonitor.h"
#include "gdaemonmount.h"
#include "gvfsdaemondbus.h"
#include "gdaemonfile.h"
#include "gvfsdaemonprotocol.h"
#include "gdbusutils.h"

/* Protects all fields of GDaemonMount that can change
   which at this point is just foreign_volume */
G_LOCK_DEFINE_STATIC(daemon_mount);

struct _GDaemonMount {
  GObject     parent;

  GMountInfo *mount_info;

  GVolume *foreign_volume;
  GVolumeMonitor *volume_monitor;
};

static void g_daemon_mount_mount_iface_init (GMountIface *iface);

G_DEFINE_TYPE_WITH_CODE (GDaemonMount, g_daemon_mount, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT,
						g_daemon_mount_mount_iface_init))

static void
g_daemon_mount_finalize (GObject *object)
{
  GDaemonMount *mount;
  
  mount = G_DAEMON_MOUNT (object);

  if (mount->foreign_volume != NULL)
    g_object_unref (mount->foreign_volume);

  if (mount->volume_monitor != NULL)
    g_object_remove_weak_pointer (G_OBJECT (mount->volume_monitor), (gpointer) &(mount->volume_monitor));

  g_mount_info_unref (mount->mount_info);
  
  if (G_OBJECT_CLASS (g_daemon_mount_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_mount_parent_class)->finalize) (object);
}

static void
g_daemon_mount_class_init (GDaemonMountClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_daemon_mount_finalize;
}

static void
g_daemon_mount_init (GDaemonMount *daemon_mount)
{
}

GDaemonMount *
g_daemon_mount_new (GMountInfo     *mount_info,
                    GVolumeMonitor *volume_monitor)
{
  GDaemonMount *mount;

  mount = g_object_new (G_TYPE_DAEMON_MOUNT, NULL);
  mount->mount_info = g_mount_info_ref (mount_info);
  mount->volume_monitor = volume_monitor;
  g_object_set_data (G_OBJECT (mount), "g-stable-name", (gpointer) mount_info->stable_name);
  if (mount->volume_monitor != NULL)
    g_object_add_weak_pointer (G_OBJECT (volume_monitor), (gpointer) &(mount->volume_monitor));

  return mount;
}

GMountInfo *
g_daemon_mount_get_mount_info (GDaemonMount *mount)
{
  return mount->mount_info;
}

static GFile *
g_daemon_mount_get_root (GMount *mount)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);

  return g_daemon_file_new (daemon_mount->mount_info->mount_spec, "/");
}

static GIcon *
g_daemon_mount_get_icon (GMount *mount)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);

  return g_themed_icon_new_with_default_fallbacks (daemon_mount->mount_info->icon);
}

static char *
g_daemon_mount_get_name (GMount *mount)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);

  return g_strdup (daemon_mount->mount_info->display_name);
}

static char *
g_daemon_mount_get_uuid (GMount *mount)
{
  return NULL;
}

static GVolume *
g_daemon_mount_get_volume (GMount *mount)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);
  if (daemon_mount->foreign_volume != NULL)
    return g_object_ref (daemon_mount->foreign_volume);
  return NULL;
}

static GDrive *
g_daemon_mount_get_drive (GMount *mount)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);
  GDrive *drive;

  G_LOCK (daemon_mount);
  drive = NULL;
  if (daemon_mount->foreign_volume != NULL)
    drive = g_volume_get_drive (daemon_mount->foreign_volume);
  G_UNLOCK (daemon_mount);
  return drive;
}

static gboolean
g_daemon_mount_can_unmount (GMount *mount)
{
  return TRUE;
}

static gboolean
g_daemon_mount_can_eject (GMount *mount)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);
  gboolean res;
  
  G_LOCK (daemon_mount);
  res = FALSE;
  if (daemon_mount->foreign_volume != NULL)
    res = g_volume_can_eject (daemon_mount->foreign_volume);
  G_UNLOCK (daemon_mount);
  
  return res;
}

static void
foreign_volume_removed (GVolume *volume, gpointer user_data)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (user_data);

  G_LOCK (daemon_mount);

  g_object_ref (daemon_mount);
  
  if (daemon_mount->foreign_volume == volume)
    {
      g_object_unref (daemon_mount->foreign_volume);
      daemon_mount->foreign_volume = NULL;
    }

  G_UNLOCK (daemon_mount);
  
  g_signal_emit_by_name (daemon_mount, "changed");
  if (daemon_mount->volume_monitor != NULL)
    g_signal_emit_by_name (daemon_mount->volume_monitor, "mount_changed", daemon_mount);
  
  g_object_unref (daemon_mount);
}

void
g_daemon_mount_set_foreign_volume (GDaemonMount *mount, 
                                   GVolume *foreign_volume)
{
  G_LOCK (daemon_mount);
  
  if (mount->foreign_volume != NULL)
    g_object_unref (mount->foreign_volume);

  if (foreign_volume != NULL)
    {
      mount->foreign_volume = foreign_volume;
      g_signal_connect_object (foreign_volume, "removed", (GCallback) foreign_volume_removed, mount, 0);
    }
  else
    mount->foreign_volume = NULL;

  G_UNLOCK (daemon_mount);
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
g_daemon_mount_unmount (GMount *mount,
			GMountUnmountFlags flags,
			GCancellable *cancellable,
			GAsyncReadyCallback callback,
			gpointer         user_data)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);
  DBusMessage *message;
  GMountInfo *mount_info;
  GSimpleAsyncResult *res;
  guint32 dbus_flags;

  mount_info = daemon_mount->mount_info;

  message =
    dbus_message_new_method_call (mount_info->dbus_id,
				  mount_info->object_path,
				  G_VFS_DBUS_MOUNT_INTERFACE,
				  G_VFS_DBUS_MOUNT_OP_UNMOUNT);

  dbus_flags = flags;
  _g_dbus_message_append_args (message, DBUS_TYPE_UINT32, &dbus_flags, 0);
  
  res = g_simple_async_result_new (G_OBJECT (mount),
				   callback, user_data,
				   g_daemon_mount_unmount);
  
  _g_vfs_daemon_call_async (message,
			    unmount_reply, res,
			    cancellable);
  
  dbus_message_unref (message);
}

static gboolean
g_daemon_mount_unmount_finish (GMount *mount,
				GAsyncResult *result,
				GError **error)
{
  return TRUE;
}

typedef struct {
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
} EjectWrapperOp;

static void
eject_wrapper_callback (GObject *source_object,
                        GAsyncResult *res,
                        gpointer user_data)
{
  EjectWrapperOp *data  = user_data;
  data->callback (data->object, res, data->user_data);
  g_free (data);
}

static void
g_daemon_mount_eject (GMount              *mount,
		      GMountUnmountFlags   flags,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);
  GDrive *drive;

  G_LOCK (daemon_mount);

  drive = NULL;
  if (daemon_mount->foreign_volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (daemon_mount->foreign_volume));
      
  G_UNLOCK (daemon_mount);
  
  if (drive != NULL)
    {
      EjectWrapperOp *data;
      data = g_new0 (EjectWrapperOp, 1);
      data->object = G_OBJECT (mount);
      data->callback = callback;
      data->user_data = user_data;
      g_drive_eject (drive, flags, cancellable, eject_wrapper_callback, data);
      g_object_unref (drive);
    }
}

static gboolean
g_daemon_mount_eject_finish (GMount        *mount,
			     GAsyncResult  *result,
			     GError       **error)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);
  GDrive *drive;
  gboolean res;

  res = TRUE;

  G_LOCK (daemon_mount);

  drive = NULL;
  if (daemon_mount->foreign_volume != NULL)
    drive = g_volume_get_drive (G_VOLUME (daemon_mount->foreign_volume));

  G_UNLOCK (daemon_mount);
  
  if (drive != NULL)
    {
      res = g_drive_eject_finish (drive, result, error);
      g_object_unref (drive);
    }
  
  return res;
}

static char **
g_daemon_mount_guess_content_type_sync (GMount              *mount,
                                        gboolean             force_rescan,
                                        GCancellable        *cancellable,
                                        GError             **error)
{
  GDaemonMount *daemon_mount = G_DAEMON_MOUNT (mount);
  char **result;

  result = NULL;
  G_LOCK (daemon_mount);
  if (daemon_mount->mount_info->x_content_types != NULL &&
      strlen (daemon_mount->mount_info->x_content_types) > 0)
    result = g_strsplit (daemon_mount->mount_info->x_content_types, " ", 0);
  G_UNLOCK (daemon_mount);

  return result;
}

static void
g_daemon_mount_guess_content_type (GMount              *mount,
                                   gboolean             force_rescan,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data)
{
  GSimpleAsyncResult *simple;
  simple = g_simple_async_result_new (G_OBJECT (mount),
                                      callback,
                                      user_data,
                                      NULL);
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}

static char **
g_daemon_mount_guess_content_type_finish (GMount              *mount,
                                          GAsyncResult        *result,
                                          GError             **error)
{
  return g_daemon_mount_guess_content_type_sync (mount, FALSE, NULL, error);
}

static void
g_daemon_mount_mount_iface_init (GMountIface *iface)
{
  iface->get_root = g_daemon_mount_get_root;
  iface->get_name = g_daemon_mount_get_name;
  iface->get_icon = g_daemon_mount_get_icon;
  iface->get_uuid = g_daemon_mount_get_uuid;
  iface->get_volume = g_daemon_mount_get_volume;
  iface->get_drive = g_daemon_mount_get_drive;
  iface->can_unmount = g_daemon_mount_can_unmount;
  iface->can_eject = g_daemon_mount_can_eject;
  iface->unmount = g_daemon_mount_unmount;
  iface->unmount_finish = g_daemon_mount_unmount_finish;
  iface->eject = g_daemon_mount_eject;
  iface->eject_finish = g_daemon_mount_eject_finish;
  iface->guess_content_type = g_daemon_mount_guess_content_type;
  iface->guess_content_type_finish = g_daemon_mount_guess_content_type_finish;
  iface->guess_content_type_sync = g_daemon_mount_guess_content_type_sync;
}
