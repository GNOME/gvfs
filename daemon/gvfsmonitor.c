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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
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
#include <gvfsdaemonprotocol.h>
#include <gvfsdaemonutils.h>
#include <gvfsjobcloseread.h>
#include <gvfsjobclosewrite.h>
#include <gvfsdbus.h>

#define OBJ_PATH_PREFIX "/org/gtk/vfs/daemon/dirmonitor/"


typedef struct {
  GDBusConnection *connection;
  char *id;
  char *object_path;
  GVfsMonitor *monitor;
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

G_DEFINE_TYPE_WITH_PRIVATE (GVfsMonitor, g_vfs_monitor, G_TYPE_OBJECT)

static void unsubscribe (Subscriber *subscriber);

static void
backend_died (GVfsMonitor *monitor,
	      GObject     *old_backend)
{
  Subscriber *subscriber;

  /*
   * Take an extra ref on the monitor because
   * unsubscribing may lead to the last ref
   * being released.
   */
  g_object_ref (G_OBJECT (monitor));

  monitor->priv->backend = NULL;

  while (monitor->priv->subscribers != NULL)
    {
      subscriber = monitor->priv->subscribers->data;
      unsubscribe (subscriber);
    }

  g_object_unref (G_OBJECT (monitor));
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

  gobject_class->finalize = g_vfs_monitor_finalize;
}

static void
g_vfs_monitor_init (GVfsMonitor *monitor)
{
  gint id;

  monitor->priv = g_vfs_monitor_get_instance_private (monitor);

  id = g_atomic_int_add (&path_counter, 1);
  monitor->priv->object_path = g_strdup_printf (OBJ_PATH_PREFIX"%d", id);
}

static gboolean
matches_subscriber (Subscriber *subscriber,
		    GDBusConnection *connection,
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
unsubscribe (Subscriber *subscriber)
{
  subscriber->monitor->priv->subscribers = g_list_remove (subscriber->monitor->priv->subscribers, subscriber);
  
  g_signal_handlers_disconnect_by_data (subscriber->connection, subscriber);
  g_object_unref (subscriber->connection);
  g_free (subscriber->id);
  g_free (subscriber->object_path);
  g_object_unref (subscriber->monitor);
  g_free (subscriber);
}

static void
subscriber_connection_closed (GDBusConnection *connection,
                              gboolean         remote_peer_vanished,
                              GError          *error,
                              Subscriber      *subscriber)
{
  unsubscribe (subscriber);
}

static gboolean
handle_subscribe (GVfsDBusMonitor *object,
                  GDBusMethodInvocation *invocation,
                  const gchar *arg_object_path,
                  GVfsMonitor *monitor)
{
  Subscriber *subscriber;

  subscriber = g_new0 (Subscriber, 1);
  subscriber->connection = g_object_ref (g_dbus_method_invocation_get_connection (invocation));
  subscriber->id = g_strdup (g_dbus_method_invocation_get_sender (invocation));
  subscriber->object_path = g_strdup (arg_object_path);
  subscriber->monitor = g_object_ref (monitor);
  
  g_signal_connect (subscriber->connection, "closed", G_CALLBACK (subscriber_connection_closed), subscriber);

  monitor->priv->subscribers = g_list_prepend (monitor->priv->subscribers, subscriber);
  
  gvfs_dbus_monitor_complete_subscribe (object, invocation);
  
  return TRUE;
}

static gboolean
handle_unsubscribe (GVfsDBusMonitor *object,
                    GDBusMethodInvocation *invocation,
                    const gchar *arg_object_path,
                    GVfsMonitor *monitor)
{
  Subscriber *subscriber;
  GList *l;

  g_object_ref (monitor); /* Keep alive during possible last remove */
  for (l = monitor->priv->subscribers; l != NULL; l = l->next)
    {
      subscriber = l->data;

      if (matches_subscriber (subscriber,
                              g_dbus_method_invocation_get_connection (invocation),
                              arg_object_path,
                              g_dbus_method_invocation_get_sender (invocation)))
        {
          unsubscribe (subscriber);
          break;
        }
    }
  g_object_unref (monitor);

  gvfs_dbus_monitor_complete_unsubscribe (object, invocation);

  return TRUE;
}

static GDBusInterfaceSkeleton *
register_path_cb (GDBusConnection *conn,
                  const char *obj_path,
                  gpointer data)
{
  GError *error;
  GVfsDBusMonitor *skeleton;
  
  skeleton = gvfs_dbus_monitor_skeleton_new ();
  g_signal_connect (skeleton, "handle-subscribe", G_CALLBACK (handle_subscribe), data);
  g_signal_connect (skeleton, "handle-unsubscribe", G_CALLBACK (handle_unsubscribe), data);
  
  error = NULL;
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (skeleton),
                                         conn,
                                         obj_path,
                                         &error))
    {
      g_warning ("Error registering path: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }

  return G_DBUS_INTERFACE_SKELETON (skeleton);
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
			      register_path_cb,
			      monitor);

  return monitor;  
}

const char *
g_vfs_monitor_get_object_path (GVfsMonitor *monitor)
{
  return monitor->priv->object_path;
}


typedef struct {
  GVfsMonitor *monitor;
  GFileMonitorEvent event_type;
  gchar *file_path;
  gchar *other_file_path;
} EmitEventData;

static void
emit_event_data_free (EmitEventData *data)
{
  g_object_unref (data->monitor);
  g_free (data->file_path);
  g_free (data->other_file_path);
  g_free (data);
}

static void
changed_cb (GVfsDBusMonitorClient *proxy,
            GAsyncResult *res,
            EmitEventData *data)
{
  GError *error = NULL;

  if (! gvfs_dbus_monitor_client_call_changed_finish (proxy, res, &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_printerr ("Error calling org.gtk.vfs.MonitorClient.Changed(): %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }
  
  emit_event_data_free (data);
}

static void
got_proxy_cb (GObject *source_object,
              GAsyncResult *res,
              EmitEventData *data)
{
  GError *error = NULL;
  GVfsDBusMonitorClient *proxy;
  
  proxy = gvfs_dbus_monitor_client_proxy_new_finish (res, &error);
  if (proxy == NULL)
    {
      g_printerr ("Error creating proxy: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      emit_event_data_free (data);
      return;
    }
  
  gvfs_dbus_monitor_client_call_changed (proxy,
                                         data->event_type,
                                         g_mount_spec_to_dbus (data->monitor->priv->mount_spec),
                                         data->file_path,
                                         g_mount_spec_to_dbus (data->monitor->priv->mount_spec),
                                         data->other_file_path ? data->other_file_path : "",
                                         NULL,
                                         (GAsyncReadyCallback) changed_cb,
                                         data);
  g_object_unref (proxy);
}

void
g_vfs_monitor_emit_event (GVfsMonitor       *monitor,
			  GFileMonitorEvent  event_type,
			  const char        *file_path,
			  const char        *other_file_path)
{
  GList *l;
  Subscriber *subscriber;

  for (l = monitor->priv->subscribers; l != NULL; l = l->next)
    {
      EmitEventData *data;
      
      subscriber = l->data;

      data = g_new0 (EmitEventData, 1);
      data->monitor = g_object_ref (monitor);
      data->event_type = event_type;
      data->file_path = g_strdup (file_path);
      data->other_file_path = g_strdup (other_file_path);

      gvfs_dbus_monitor_client_proxy_new (subscriber->connection,
                                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                                          subscriber->id,
                                          subscriber->object_path,
                                          NULL,
                                          (GAsyncReadyCallback) got_proxy_cb,
                                          data);
    }
}
