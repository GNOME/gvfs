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

#include "gdaemonfilemonitor.h"
#include <gio/gio.h>
#include <gvfsdaemondbus.h>
#include <gvfsdaemonprotocol.h>
#include "gvfsdbusutils.h"
#include "gmountspec.h"
#include "gdaemonfile.h"

#define OBJ_PATH_PREFIX "/org/gtk/vfs/client/filemonitor/"

/* atomic */
static volatile gint path_counter = 1;

static gboolean g_daemon_file_monitor_cancel (GFileMonitor* monitor);
static DBusHandlerResult g_daemon_file_monitor_dbus_filter (DBusConnection     *connection,
							    DBusMessage        *message,
							    void               *user_data);


struct _GDaemonFileMonitor
{
  GFileMonitor parent_instance;

  char *object_path;
  char *remote_obj_path;
  char *remote_id;
};

G_DEFINE_TYPE (GDaemonFileMonitor, g_daemon_file_monitor, G_TYPE_FILE_MONITOR)

static void
g_daemon_file_monitor_finalize (GObject* object)
{
  GDaemonFileMonitor *daemon_monitor;
  
  daemon_monitor = G_DAEMON_FILE_MONITOR (object);

  _g_dbus_unregister_vfs_filter (daemon_monitor->object_path);
  
  g_free (daemon_monitor->object_path);
  g_free (daemon_monitor->remote_id);
  g_free (daemon_monitor->remote_obj_path);
  
  if (G_OBJECT_CLASS (g_daemon_file_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_file_monitor_parent_class)->finalize) (object);
}

static void
g_daemon_file_monitor_class_init (GDaemonFileMonitorClass* klass)
{
  GObjectClass* gobject_class = G_OBJECT_CLASS (klass);
  GFileMonitorClass *file_monitor_class = G_FILE_MONITOR_CLASS (klass);
  
  gobject_class->finalize = g_daemon_file_monitor_finalize;

  file_monitor_class->cancel = g_daemon_file_monitor_cancel;
}

static void
g_daemon_file_monitor_init (GDaemonFileMonitor* daemon_monitor)
{
  gint id;
  
  id = g_atomic_int_exchange_and_add (&path_counter, 1);

  daemon_monitor->object_path = g_strdup_printf (OBJ_PATH_PREFIX"%d", id);
  
  _g_dbus_register_vfs_filter (daemon_monitor->object_path,
			       g_daemon_file_monitor_dbus_filter,
			       G_OBJECT (daemon_monitor));
}

GFileMonitor*
g_daemon_file_monitor_new (const char *remote_id,
				const char *remote_obj_path)
{
  GDaemonFileMonitor* daemon_monitor;
  DBusMessage *message;
  
  daemon_monitor = g_object_new (G_TYPE_DAEMON_FILE_MONITOR, NULL);

  daemon_monitor->remote_id = g_strdup (remote_id);
  daemon_monitor->remote_obj_path = g_strdup (remote_obj_path);

  message =
    dbus_message_new_method_call (daemon_monitor->remote_id,
				  daemon_monitor->remote_obj_path,
				  G_VFS_DBUS_MONITOR_INTERFACE,
				  G_VFS_DBUS_MONITOR_OP_SUBSCRIBE);

  _g_dbus_message_append_args (message, DBUS_TYPE_OBJECT_PATH,
			       &daemon_monitor->object_path, 0);

  _g_vfs_daemon_call_async (message,
			    NULL, NULL,
			    NULL);

  dbus_message_unref (message);
  
  return G_FILE_MONITOR (daemon_monitor);
}

static DBusHandlerResult
g_daemon_file_monitor_dbus_filter (DBusConnection     *connection,
				   DBusMessage        *message,
				   void               *user_data)
{
  GDaemonFileMonitor *monitor = G_DAEMON_FILE_MONITOR (user_data);
  const char *member;
  guint32 event_type;
  DBusMessageIter iter;
  GMountSpec *spec1, *spec2;
  char *path1, *path2;
  GFile *file1, *file2;
  
  member = dbus_message_get_member (message);

  if (strcmp (member, G_VFS_DBUS_MONITOR_CLIENT_OP_CHANGED) == 0)
    {
      dbus_message_iter_init (message, &iter);
      
      if (!_g_dbus_message_iter_get_args (&iter, NULL,
					  DBUS_TYPE_UINT32, &event_type,
					  0))
	return DBUS_HANDLER_RESULT_HANDLED;
      
      spec1 = g_mount_spec_from_dbus (&iter);
      if (!_g_dbus_message_iter_get_args (&iter, NULL,
					  G_DBUS_TYPE_CSTRING, &path1,
					  0))
	{
	  g_mount_spec_unref (spec1);
	  return DBUS_HANDLER_RESULT_HANDLED;
	}

      file1 = g_daemon_file_new (spec1, path1);
      
      g_mount_spec_unref (spec1);
      g_free (path1);

      file2 = NULL;
      
      spec2 = g_mount_spec_from_dbus (&iter);
      if (spec2) {
	if (_g_dbus_message_iter_get_args (&iter, NULL,
					   G_DBUS_TYPE_CSTRING, &path2,
					   0))
	  {
	    file2 = g_daemon_file_new (spec2, path2);

	    g_free (path2);
	  }
	
	g_mount_spec_unref (spec2);
      }

      g_file_monitor_emit_event (G_FILE_MONITOR (monitor),
				 file1, file2,
				 event_type);
      
      return DBUS_HANDLER_RESULT_HANDLED;
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


static gboolean
g_daemon_file_monitor_cancel (GFileMonitor* monitor)
{
  GDaemonFileMonitor *daemon_monitor = G_DAEMON_FILE_MONITOR (monitor);
  DBusMessage *message;
  
  message =
    dbus_message_new_method_call (daemon_monitor->remote_id,
				  daemon_monitor->remote_obj_path,
				  G_VFS_DBUS_MONITOR_INTERFACE,
				  G_VFS_DBUS_MONITOR_OP_UNSUBSCRIBE);

  _g_dbus_message_append_args (message, DBUS_TYPE_OBJECT_PATH,
			       &daemon_monitor->object_path, 0);
  
  _g_vfs_daemon_call_async (message,
			    NULL, NULL, 
			    NULL);
  
  dbus_message_unref (message);
  
  return TRUE;
}

