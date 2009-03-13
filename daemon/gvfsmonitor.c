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

#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gi18n.h>
#include <gvfsmonitor.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsdaemonutils.h>
#include <gvfsjobcloseread.h>
#include <gvfsjobclosewrite.h>

#define OBJ_PATH_PREFIX "/org/gtk/vfs/daemon/dirmonitor/"

/* TODO: Real P_() */
#define P_(_x) (_x)


/* TODO: Handle a connection dying and unregister its subscription */

typedef struct {
  DBusConnection *connection;
  char *id;
  char *object_path;
} Subscriber;

struct _GVfsMonitorPrivate
{
  GVfsDaemon *daemon;
  GVfsBackend *backend; /* weak ref */
  GMountSpec *mount_spec;
  char *object_path;
  GList *subscribers;
};

/* atomic */
static volatile gint path_counter = 1;

G_DEFINE_TYPE (GVfsMonitor, g_vfs_monitor, G_TYPE_OBJECT)

static void unsubscribe (GVfsMonitor *monitor,
			 Subscriber *subscriber);

static void
backend_died (GVfsMonitor *monitor,
	      GObject     *old_backend)
{
  Subscriber *subscriber;
  
  monitor->priv->backend = NULL;

  while (monitor->priv->subscribers != NULL)
    {
      subscriber = monitor->priv->subscribers->data;
      unsubscribe (monitor, subscriber);
    }
}

static void
g_vfs_monitor_finalize (GObject *object)
{
  GVfsMonitor *monitor;

  monitor = G_VFS_MONITOR (object);

  if (monitor->priv->backend)
    g_object_weak_unref (G_OBJECT (monitor->priv->backend),
			 (GWeakNotify)backend_died,
			 monitor);

  g_vfs_daemon_unregister_path (monitor->priv->daemon, monitor->priv->object_path);
  g_object_unref (monitor->priv->daemon);

  g_mount_spec_unref (monitor->priv->mount_spec);
  
  g_free (monitor->priv->object_path);
  
  if (G_OBJECT_CLASS (g_vfs_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_monitor_parent_class)->finalize) (object);
}

static void
g_vfs_monitor_class_init (GVfsMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GVfsMonitorPrivate));
  
  gobject_class->finalize = g_vfs_monitor_finalize;
}

static void
g_vfs_monitor_init (GVfsMonitor *monitor)
{
  gint id;
  
  monitor->priv = G_TYPE_INSTANCE_GET_PRIVATE (monitor,
					       G_TYPE_VFS_MONITOR,
					       GVfsMonitorPrivate);
  
  id = g_atomic_int_exchange_and_add (&path_counter, 1);
  monitor->priv->object_path = g_strdup_printf (OBJ_PATH_PREFIX"%d", id);
}

static gboolean
matches_subscriber (Subscriber *subscriber,
		    DBusConnection *connection,
		    const char *object_path,
		    const char *dbus_id)
{
  return (subscriber->connection == connection &&
	  strcmp (subscriber->object_path, object_path) == 0 &&
	  ((dbus_id == NULL && subscriber->id == NULL) ||
	   (dbus_id != NULL && subscriber->id != NULL &&
	    strcmp (subscriber->id, dbus_id) == 0)));
}

static void
unsubscribe (GVfsMonitor *monitor,
	     Subscriber *subscriber)
{
  monitor->priv->subscribers = g_list_remove (monitor->priv->subscribers, subscriber);
  
  dbus_connection_unref (subscriber->connection);
  g_free (subscriber->id);
  g_free (subscriber->object_path);
  g_free (subscriber);
  g_object_unref (monitor);
}

static DBusHandlerResult
vfs_monitor_message_callback (DBusConnection  *connection,
			      DBusMessage     *message,
			      void            *user_data)
{
  GVfsMonitor *monitor = user_data;
  char *object_path;
  DBusError derror;
  GList *l;
  Subscriber *subscriber;
  DBusMessage *reply;
  
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_MONITOR_INTERFACE,
				   G_VFS_DBUS_MONITOR_OP_SUBSCRIBE))
    {
      dbus_error_init (&derror);
      if (!dbus_message_get_args (message, &derror, 
				  DBUS_TYPE_OBJECT_PATH, &object_path,
				  0))
	{
	  reply = dbus_message_new_error (message,
					  derror.name,
					  derror.message);
	  dbus_error_free (&derror);
	  
	  dbus_connection_send (connection, reply, NULL);
	}
      else
	{
	  subscriber = g_new0 (Subscriber, 1);
	  subscriber->connection = dbus_connection_ref (connection);
	  subscriber->id = g_strdup (dbus_message_get_sender (message));
	  subscriber->object_path = g_strdup (object_path);

	  g_object_ref (monitor);
	  monitor->priv->subscribers = g_list_prepend (monitor->priv->subscribers, subscriber);

	  reply = dbus_message_new_method_return (message);
	  dbus_connection_send (connection, reply, NULL);
	}
      
      return DBUS_HANDLER_RESULT_HANDLED;
    }
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MONITOR_INTERFACE,
					G_VFS_DBUS_MONITOR_OP_UNSUBSCRIBE))
    {
      dbus_error_init (&derror);
      if (!dbus_message_get_args (message, &derror, 
				  DBUS_TYPE_OBJECT_PATH, &object_path,
				  0))
	{
	  reply = dbus_message_new_error (message,
					  derror.name,
					  derror.message);
	  dbus_error_free (&derror);
	  
	  dbus_connection_send (connection, reply, NULL);
	}
      else
	{
	  g_object_ref (monitor); /* Keep alive during possible last remove */
	  for (l = monitor->priv->subscribers; l != NULL; l = l->next)
	    {
	      subscriber = l->data;

	      if (matches_subscriber (subscriber,
				      connection,
				      object_path,
				      dbus_message_get_sender (message)))
		{
		  unsubscribe (monitor, subscriber);
		  break;
		}
	    }
	  
	  reply = dbus_message_new_method_return (message);
	  dbus_connection_send (connection, reply, NULL);

	  g_object_unref (monitor);
	}
      
      return DBUS_HANDLER_RESULT_HANDLED;
    }
      
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  
}

GVfsMonitor *
g_vfs_monitor_new (GVfsBackend *backend)
{
  GVfsMonitor *monitor;
  
  monitor = g_object_new (G_TYPE_VFS_MONITOR, NULL);

  monitor->priv->backend = backend;

  g_object_weak_ref (G_OBJECT (backend),
		     (GWeakNotify)backend_died,
		     monitor);
  
  monitor->priv->daemon = g_object_ref (g_vfs_backend_get_daemon (backend));
  monitor->priv->mount_spec = g_mount_spec_ref (g_vfs_backend_get_mount_spec (backend));

  g_vfs_daemon_register_path (monitor->priv->daemon,
			      monitor->priv->object_path,
			      vfs_monitor_message_callback,
			      monitor);

  return monitor;  
}

const char *
g_vfs_monitor_get_object_path (GVfsMonitor *monitor)
{
  return monitor->priv->object_path;
}

void
g_vfs_monitor_emit_event (GVfsMonitor       *monitor,
			  GFileMonitorEvent  event_type,
			  const char        *file_path,
			  const char        *other_file_path)
{
  GList *l;
  Subscriber *subscriber;
  DBusMessage *message;
  DBusMessageIter iter;
  guint32 event_type_dbus;
  
  for (l = monitor->priv->subscribers; l != NULL; l = l->next)
    {
      subscriber = l->data;

      message =
	dbus_message_new_method_call (subscriber->id,
				      subscriber->object_path,
				      G_VFS_DBUS_MONITOR_CLIENT_INTERFACE,
				      G_VFS_DBUS_MONITOR_CLIENT_OP_CHANGED);

      dbus_message_iter_init_append (message, &iter);
      event_type_dbus = event_type;
      dbus_message_iter_append_basic (&iter,
				      DBUS_TYPE_UINT32,
				      &event_type_dbus);
      g_mount_spec_to_dbus (&iter, monitor->priv->mount_spec);
      _g_dbus_message_iter_append_cstring (&iter, file_path);

      if (other_file_path)
	{
	  g_mount_spec_to_dbus (&iter, monitor->priv->mount_spec);
	  _g_dbus_message_iter_append_cstring (&iter, other_file_path);
	}

      dbus_message_set_no_reply (message, FALSE);
      
      dbus_connection_send (subscriber->connection, message, NULL);
      dbus_message_unref (message);
    }
}
