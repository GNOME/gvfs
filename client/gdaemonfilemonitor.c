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
#include <string.h>

#include "gdaemonfilemonitor.h"
#include <gio/gio.h>
#include <gvfsdaemondbus.h>
#include <gvfsdaemonprotocol.h>
#include "gmountspec.h"
#include "gdaemonfile.h"
#include <gvfsdbus.h>

#define OBJ_PATH_PREFIX "/org/gtk/vfs/client/filemonitor/"

/* atomic */
static volatile gint path_counter = 1;

static gboolean g_daemon_file_monitor_cancel (GFileMonitor* monitor);


struct _GDaemonFileMonitor
{
  GFileMonitor parent_instance;

  char *object_path;
  char *remote_obj_path;
  char *remote_id;
  GDBusConnection *connection;
  GVfsDBusMonitor *proxy;
  GVfsDBusMonitorClient *skeleton;
};

G_DEFINE_TYPE (GDaemonFileMonitor, g_daemon_file_monitor, G_TYPE_FILE_MONITOR)

static void
g_daemon_file_monitor_finalize (GObject* object)
{
  GDaemonFileMonitor *daemon_monitor;
  
  daemon_monitor = G_DAEMON_FILE_MONITOR (object);

  if (daemon_monitor->skeleton)
    {
      g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (daemon_monitor->skeleton));
      g_object_unref (daemon_monitor->skeleton);
    }

  g_clear_object (&daemon_monitor->proxy);

  g_clear_object (&daemon_monitor->connection);

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

static gboolean
handle_changed (GVfsDBusMonitorClient *object,
                GDBusMethodInvocation *invocation,
                guint arg_event_type,
                GVariant *arg_mount_spec,
                const gchar *arg_file_path,
                GVariant *arg_other_mount_spec,
                const gchar *arg_other_file_path,
                gpointer user_data)
{
  GDaemonFileMonitor *monitor = G_DAEMON_FILE_MONITOR (user_data);
  GMountSpec *spec1, *spec2;
  GFile *file1, *file2;

  spec1 = g_mount_spec_from_dbus (arg_mount_spec);
  file1 = g_daemon_file_new (spec1, arg_file_path);
  g_mount_spec_unref (spec1);

  file2 = NULL;
  
  if (strlen (arg_other_file_path) > 0)
    {
      spec2 = g_mount_spec_from_dbus (arg_other_mount_spec);
      file2 = g_daemon_file_new (spec2, arg_other_file_path);
      g_mount_spec_unref (spec2);
    }

  g_file_monitor_emit_event (G_FILE_MONITOR (monitor),
                             file1, file2,
                             arg_event_type);
  
  gvfs_dbus_monitor_client_complete_changed (object, invocation);
  
  return TRUE;
}

static void
g_daemon_file_monitor_init (GDaemonFileMonitor* daemon_monitor)
{
  gint id;

  id = g_atomic_int_add (&path_counter, 1);

  daemon_monitor->object_path = g_strdup_printf (OBJ_PATH_PREFIX"%d", id);

  daemon_monitor->skeleton = gvfs_dbus_monitor_client_skeleton_new ();
  g_signal_connect (daemon_monitor->skeleton, "handle-changed", G_CALLBACK (handle_changed), daemon_monitor);
}

static void
subscribe_cb (GVfsDBusMonitor *proxy,
              GAsyncResult *res,
              GDaemonFileMonitor* monitor)
{
  GError *error = NULL;

  if (! gvfs_dbus_monitor_call_subscribe_finish (proxy, res, &error))
    {
      g_printerr ("Error calling org.gtk.vfs.Monitor.Subscribe(): %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }

  /* If its cancelled already, unsubscribe and don't save stuff */
  if (g_file_monitor_is_cancelled (G_FILE_MONITOR (monitor)))
    {
      gvfs_dbus_monitor_call_unsubscribe (proxy,
                                          monitor->object_path,
                                          NULL, NULL, NULL);
      g_clear_object (&monitor->connection);
      g_object_unref (monitor);
      return;
    }

  monitor->proxy = g_object_ref (proxy);

  g_object_unref (monitor);
}

static void
async_proxy_new_cb (GObject *source_object,
                    GAsyncResult *res,
                    gpointer user_data)
{
  GDaemonFileMonitor* monitor = user_data;
  GVfsDBusMonitor *proxy;
  GError *error = NULL;

  proxy = gvfs_dbus_monitor_proxy_new_finish (res, &error);
  if (proxy == NULL)
    {
      g_printerr ("Error creating monitor proxy: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      g_clear_object (&monitor->connection);
      g_object_unref (monitor);
      return;
    }

  if (g_file_monitor_is_cancelled (G_FILE_MONITOR (monitor)))
    {
      g_clear_object (&monitor->connection);
      g_object_unref (proxy);
      g_object_unref (monitor);
      return;
    }

  gvfs_dbus_monitor_call_subscribe (proxy,
                                    monitor->object_path,
                                    NULL,
                                    (GAsyncReadyCallback) subscribe_cb,
                                    monitor);
  g_object_unref (proxy);
}

static void
async_got_connection_cb (GDBusConnection *connection,
                         GError *io_error,
                         gpointer callback_data)
{
  GDaemonFileMonitor* monitor = callback_data;
  GError *error;

  if (! connection)
    {
      g_printerr ("Error getting connection for monitoring: %s (%s, %d)\n",
                   io_error->message, g_quark_to_string (io_error->domain), io_error->code);
      g_object_unref (monitor);
      return;
    }

  error = NULL;
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (monitor->skeleton),
                                         connection,
                                         monitor->object_path,
                                         &error))
    {
      g_warning ("Error registering path: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }

  monitor->connection = g_object_ref (connection);
  gvfs_dbus_monitor_proxy_new (connection,
                               G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                               monitor->remote_id,
                               monitor->remote_obj_path,
                               NULL,
                               async_proxy_new_cb,
                               monitor);
}

GFileMonitor*
g_daemon_file_monitor_new (const char *remote_id,
				const char *remote_obj_path)
{
  GDaemonFileMonitor* daemon_monitor;

  daemon_monitor = g_object_new (G_TYPE_DAEMON_FILE_MONITOR, NULL);

  daemon_monitor->remote_id = g_strdup (remote_id);
  daemon_monitor->remote_obj_path = g_strdup (remote_obj_path);

  _g_dbus_connection_get_for_async (daemon_monitor->remote_id,
                                    async_got_connection_cb,
                                    g_object_ref (daemon_monitor),
                                    NULL);

  return G_FILE_MONITOR (daemon_monitor);
}

static gboolean
g_daemon_file_monitor_cancel (GFileMonitor* monitor)
{
  GDaemonFileMonitor *daemon_monitor = G_DAEMON_FILE_MONITOR (monitor);

  if (daemon_monitor->proxy != NULL)
    gvfs_dbus_monitor_call_unsubscribe (daemon_monitor->proxy,
					daemon_monitor->object_path,
					NULL, NULL, NULL);

  return TRUE;
}

