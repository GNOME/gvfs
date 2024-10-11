/* gvfswsddservice.c
 *
 * Copyright (C) 2023 Red Hat, Inc.
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
 * You should have received a copy of the GNU Library General Public
 * License along with the Gnome Library; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Authors: Ondrej Holy <oholy@redhat.com>
 */

#include <config.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>
#include <glib/gi18n.h>

#include "gvfswsddservice.h"
#include "gvfswsdddevice.h"
#include "gvfsutils.h"

#define SYSTEM_SOCKET_PATH "/run/wsdd.socket"
#define SOCKET_NAME "wsdd"
#define CONNECT_TIMEOUT 10
#define LIST_COMMAND "list pub:Computer\n"
#define PROBE_COMMAND "clear\nprobe\n"
#define RELOAD_TIMEOUT 15
#define PROBE_TIMEOUT 5

struct _GVfsWsddService
{
  GObject parent_instance;

  GSocket *socket;
  GSocketAddress *socket_address;
  GSocketConnection *socket_connection;
  GInputStream *input_stream;
  GDataInputStream *data_input_stream;
  GOutputStream *output_stream;

  guint reload_source_id;
  GList *devices;
  GList *new_devices;

  GNetworkMonitor *network_monitor;
  guint probe_source_id;
  gboolean network_changed;

  gboolean extra_debug;

  GCancellable *cancellable;
  GError *error;
};

enum
{
  DEVICE_CHANGED_SIGNAL,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void initable_iface_init (GInitableIface *initable_iface);
static void async_initable_iface_init (GAsyncInitableIface *async_initable_iface);

G_DEFINE_TYPE_WITH_CODE (GVfsWsddService,
                         g_vfs_wsdd_service,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                initable_iface_init)
                         G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE,
                                                async_initable_iface_init))

static gboolean probe_devices (gpointer user_data);
static gboolean reload_devices (gpointer user_data);

static void
g_vfs_wsdd_service_dispose (GObject *object)
{
  GVfsWsddService *service = G_VFS_WSDD_SERVICE (object);

  g_cancellable_cancel (service->cancellable);

  G_OBJECT_CLASS (g_vfs_wsdd_service_parent_class)->dispose (object);
}


static void
g_vfs_wsdd_service_finalize (GObject *object)
{
  GVfsWsddService *service = G_VFS_WSDD_SERVICE (object);

  g_clear_object (&service->socket);
  g_clear_object (&service->socket_address);
  g_clear_object (&service->socket_connection);
  g_clear_object (&service->data_input_stream);

  g_clear_handle_id (&service->reload_source_id, g_source_remove);

  g_clear_list (&service->devices, (GDestroyNotify)g_object_unref);
  g_clear_list (&service->new_devices, (GDestroyNotify)g_object_unref);

  if (service->network_monitor != NULL)
    {
      g_signal_handlers_disconnect_by_data (service->network_monitor, service);
      g_clear_object (&service->network_monitor);
    }

  g_clear_handle_id (&service->probe_source_id, g_source_remove);

  g_clear_object (&service->cancellable);
  g_clear_error (&service->error);

  G_OBJECT_CLASS (g_vfs_wsdd_service_parent_class)->finalize (object);
}

static void
g_vfs_wsdd_service_init (GVfsWsddService *service)
{
}

static void
g_vfs_wsdd_service_class_init (GVfsWsddServiceClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->dispose = g_vfs_wsdd_service_dispose;
  gobject_class->finalize = g_vfs_wsdd_service_finalize;

  /**
   * GVfsWsddService::device-changed:
   * @wsdd_service: the #GVfsWsddService
   * @uuid: an uuid
   * @event_type: a #GFileMonitorEvent
   *
   * Emitted when #GVfsWsddDevice with @uuid has been changed.
   **/
  signals[DEVICE_CHANGED_SIGNAL] = g_signal_new ("device-changed",
                                                 G_TYPE_FROM_CLASS (klass),
                                                 G_SIGNAL_RUN_LAST,
                                                 0,
                                                 NULL,
                                                 NULL,
                                                 g_cclosure_marshal_generic,
                                                 G_TYPE_NONE,
                                                 2,
                                                 G_TYPE_STRING,
                                                 G_TYPE_FILE_MONITOR_EVENT);
}

void
g_vfs_wsdd_service_new (GCancellable *cancellable,
                        GAsyncReadyCallback callback,
                        gpointer user_data)
{
  g_async_initable_new_async (G_VFS_TYPE_WSDD_SERVICE,
                              G_PRIORITY_DEFAULT,
                              cancellable,
                              callback,
                              user_data,
                              NULL);
}

GVfsWsddService *
g_vfs_wsdd_service_new_finish (GAsyncResult *result,
                               GError **error)
{
  g_autoptr(GObject) source_object = NULL;
  GObject *object;

  source_object = g_async_result_get_source_object (result);
  object = g_async_initable_new_finish (G_ASYNC_INITABLE (source_object),
                                        result,
                                        error);
  if (object == NULL)
    {
      return NULL;
    }

  return G_VFS_WSDD_SERVICE (object);
}

static void
schedule_reload_or_probe_devices (GVfsWsddService *service)
{
  g_clear_handle_id (&service->probe_source_id, g_source_remove);
  g_clear_handle_id (&service->reload_source_id, g_source_remove);

  if (service->network_changed)
    {
      service->probe_source_id = g_timeout_add_seconds (PROBE_TIMEOUT,
                                                        probe_devices,
                                                        service);
    }
  else
    {
      service->reload_source_id = g_timeout_add_seconds (RELOAD_TIMEOUT,
                                                         reload_devices,
                                                         service);
    }
}

static void
probe_devices_cb (GObject* source_object,
                  GAsyncResult* result,
                  gpointer user_data)
{
  GVfsWsddService *service = G_VFS_WSDD_SERVICE (user_data);
  g_autoptr(GError) error = NULL;

  g_output_stream_write_all_finish (G_OUTPUT_STREAM (source_object),
                                    result,
                                    NULL,
                                    &error);
  if (error != NULL)
    {
      g_warning ("Writing to wsdd socket failed: %s\n",  error->message);

      if (service->error == NULL)
        {
          g_set_error (&service->error,
                       G_IO_ERROR,
                       G_IO_ERROR_FAILED,
                       _("Communication with the underlying wsdd daemon failed."));
        }

      g_object_unref (service);

      return;
    }

  schedule_reload_or_probe_devices (service);

  g_object_unref (service);
}

static gboolean
probe_devices (gpointer user_data)
{
  GVfsWsddService *service = G_VFS_WSDD_SERVICE (user_data);

  service->probe_source_id = 0;
  service->network_changed = FALSE;

  if (service->extra_debug)
    {
      g_debug ("Probing for devices\n");
    }

  g_output_stream_write_all_async (service->output_stream,
                                   PROBE_COMMAND,
                                   strlen (PROBE_COMMAND),
                                   G_PRIORITY_DEFAULT,
                                   service->cancellable,
                                   probe_devices_cb,
                                   g_object_ref (service));

  return G_SOURCE_REMOVE;
}

static void
reload_devices_finish (GVfsWsddService *service)
{
  GList *old_devices = service->devices;
  GList *old = old_devices;
  GList *new = service->new_devices;
  gint order;

  service->devices = g_steal_pointer (&service->new_devices);

  while (old != NULL || new != NULL)
    {
      order = (old == NULL) - (new == NULL);
      if (order == 0)
        {
          order = g_vfs_wsdd_device_compare (old->data, new->data);
        }

      if (order < 0)
        {
          g_signal_emit (service,
                         signals[DEVICE_CHANGED_SIGNAL],
                         0,
                         g_vfs_wsdd_device_get_uuid (old->data),
                         G_FILE_MONITOR_EVENT_DELETED);

          old = old->next;
        }
      else if (order > 0)
        {
          g_signal_emit (service,
                         signals[DEVICE_CHANGED_SIGNAL],
                         0,
                         g_vfs_wsdd_device_get_uuid (new->data),
                         G_FILE_MONITOR_EVENT_CREATED);

          new = new->next;
        }
      else
        {
          if (!g_vfs_wsdd_device_equal (old->data, new->data))
            {
              g_signal_emit (service,
                             signals[DEVICE_CHANGED_SIGNAL],
                             0,
                             g_vfs_wsdd_device_get_uuid (new->data),
                             G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED);
            }

          old = old->next;
          new = new->next;
        }
    }

  schedule_reload_or_probe_devices (service);

  g_list_free_full (old_devices, g_object_unref);
  g_object_unref (service);
}

static GVfsWsddDevice *
g_vfs_wsdd_device_new_from_line (const gchar *line)
{
  g_auto(GStrv) fields = NULL;

  g_return_val_if_fail (line != NULL, NULL);

  /* The line is tab-separated and consisted of the uuid, name, association,
   * last_seen, and addresses fields.
   */
  fields = g_strsplit (line, "\t", 0);
  if (g_strv_length (fields) < 5)
    {
      g_warning ("Unexpected format of the line: %s", line);

      return NULL;
    }

  return g_vfs_wsdd_device_new (fields[0],
                                fields[1],
                                fields[4]);
}

static void
reload_devices_read_cb (GObject* source_object,
                        GAsyncResult* result,
                        gpointer user_data)
{
  GVfsWsddService *service = G_VFS_WSDD_SERVICE (user_data);
  GVfsWsddDevice *device;
  g_autofree gchar *line = NULL;
  g_autoptr(GError) error = NULL;

  line = g_data_input_stream_read_line_finish (G_DATA_INPUT_STREAM (source_object),
                                               result,
                                               NULL,
                                               &error);
  if (error != NULL)
    {
      g_warning ("Reading from wsdd socket failed: %s\n", error->message);

      if (service->error == NULL)
        {
          g_set_error (&service->error,
                       G_IO_ERROR,
                       G_IO_ERROR_FAILED,
                       _("Communication with the underlying wsdd daemon failed."));
        }

      g_object_unref (service);

      return;
    }

  if (service->extra_debug)
    {
      g_debug ("%s\n", line);
    }

  /* The last line consist of "." character. */
  if (g_str_equal (line, "."))
    {
      reload_devices_finish (service);

      return;
    }

  device = g_vfs_wsdd_device_new_from_line (line);
  if (device != NULL)
    {
      service->new_devices = g_list_insert_sorted (service->new_devices,
                                                   device,
                                                   (GCompareFunc)g_vfs_wsdd_device_compare);
    }

  g_data_input_stream_read_line_async (service->data_input_stream,
                                       G_PRIORITY_DEFAULT,
                                       service->cancellable,
                                       reload_devices_read_cb,
                                       service);
}

static void
reload_devices_list_cb (GObject* source_object,
                        GAsyncResult* result,
                        gpointer user_data)
{
  GVfsWsddService *service = G_VFS_WSDD_SERVICE (user_data);
  g_autoptr(GError) error = NULL;

  g_output_stream_write_all_finish (G_OUTPUT_STREAM (source_object),
                                    result,
                                    NULL,
                                    &error);
  if (error != NULL)
    {
      g_warning ("Writing to wsdd socket failed: %s\n",  error->message);

      if (service->error == NULL)
        {
          g_set_error (&service->error,
                       G_IO_ERROR,
                       G_IO_ERROR_FAILED,
                       _("Communication with the underlying wsdd daemon failed."));
        }

      g_object_unref (service);

      return;
    }

  g_data_input_stream_read_line_async (service->data_input_stream,
                                       G_PRIORITY_DEFAULT,
                                       service->cancellable,
                                       reload_devices_read_cb,
                                       service);
}

static gboolean
reload_devices (gpointer user_data)
{
  GVfsWsddService *service = G_VFS_WSDD_SERVICE (user_data);

  service->reload_source_id = 0;

  if (service->extra_debug)
    {
      g_debug ("Reloading devices\n");
    }

  g_output_stream_write_all_async (service->output_stream,
                                   LIST_COMMAND,
                                   strlen (LIST_COMMAND),
                                   G_PRIORITY_DEFAULT,
                                   service->cancellable,
                                   reload_devices_list_cb,
                                   g_object_ref (service));

  return G_SOURCE_REMOVE;
}

static void
network_changed_cb (GNetworkMonitor *network_monitor,
                    gboolean available,
                    gpointer user_data)
{
  GVfsWsddService *service = G_VFS_WSDD_SERVICE (user_data);

  if (!service->network_changed)
    {
      g_debug ("Network change detected\n");
    }

  service->network_changed = TRUE;

  /* (Re)schedule only if none of those operations is running already. */
  if (service->reload_source_id != 0 ||
      service->probe_source_id != 0)
    {
      schedule_reload_or_probe_devices (service);
    }
}

static void
child_watch_cb (GPid pid,
                gint status,
                gpointer user_data)
{
  GVfsWsddService *service = G_VFS_WSDD_SERVICE (user_data);

  g_warning ("The wsdd daemon exited unexpectedly.");

  g_clear_handle_id (&service->reload_source_id, g_source_remove);
  g_clear_handle_id (&service->probe_source_id, g_source_remove);

  g_clear_error (&service->error);
  g_set_error (&service->error,
               G_IO_ERROR,
               G_IO_ERROR_FAILED,
               _("The underlying wsdd daemon exited unexpectedly."));

  g_spawn_close_pid (pid);
}

static gboolean
initable_init (GInitable *initable,
               GCancellable *cancellable,
               GError **error)
{
  GVfsWsddService *service = G_VFS_WSDD_SERVICE (initable);
  g_autofree gchar *socket_dir = NULL;
  g_autofree gchar *socket_path = NULL;
  g_autoptr(GError) local_error = NULL;
  const gchar *debug;

  debug = g_getenv ("GVFS_WSDD_DEBUG");
  service->extra_debug = (debug != NULL);

  service->socket = g_socket_new (G_SOCKET_FAMILY_UNIX,
                                  G_SOCKET_TYPE_STREAM,
                                  G_SOCKET_PROTOCOL_DEFAULT,
                                  &local_error);
  if (local_error != NULL)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));

      return FALSE;
    }

  /* Try to connect to the socket-activated service. */
  service->socket_address = g_unix_socket_address_new (SYSTEM_SOCKET_PATH);
  g_socket_connect (service->socket,
                    service->socket_address,
                    cancellable,
                    &local_error);

  if (local_error != NULL)
    {
      /* Fall back to spawning our own wsdd daemon. */
      g_clear_error (&local_error);
      g_clear_object (&service->socket_address);
      socket_dir = gvfs_get_socket_dir ();
      socket_path = g_build_filename (socket_dir, SOCKET_NAME, NULL);
      service->socket_address = g_unix_socket_address_new (socket_path);

      /* Try to connect to the already running wsdd daemon. */
      g_socket_connect (service->socket,
                        service->socket_address,
                        cancellable,
                        &local_error);
    }

  if (local_error != NULL)
    {
      gchar *args[7] = { 0 };
      GPid pid;
      gint i;

      /* The wsdd daemon is not probably running yet. */
      g_clear_error (&local_error);

      g_debug ("Spawning our own wsdd daemon\n");

      /* Try to spawn the wsdd daemon first. */
      args[0] = WSDD_PROGRAM;
      args[1] = "--no-host";
      args[2] = "--discovery";
      args[3] = "--listen";
      args[4] = socket_path;

      if (debug != NULL)
        {
          if (g_strcmp0 (debug, "all") == 0)
            {
              args[5] = "-vvv";
            }
          else
            {
              args[5] = "-v";
            }
        }

      g_spawn_async (NULL,
                     args,
                     NULL,
                     G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                     NULL,
                     NULL,
                     &pid,
                     &local_error);
      if (local_error != NULL)
        {
          g_warning ("Failed to spawn the wsdd daemon: %s",
                     local_error->message);

          g_set_error (error,
                       G_IO_ERROR,
                       G_IO_ERROR_FAILED,
                       _("Failed to spawn the underlying wsdd daemon."));

          return FALSE;
        }

      g_child_watch_add (pid, child_watch_cb, service);

      /* Wait until the socket is available. */
      for (i = 0; i < CONNECT_TIMEOUT; i++)
        {
          g_clear_error (&local_error);
          g_socket_connect (service->socket,
                            service->socket_address,
                            cancellable,
                            &local_error);
          if (local_error == NULL)
            {
              break;
            }

          sleep (1);
        }

      if (local_error != NULL)
        {
          g_warning ("Unable to connect to the wsdd socket: %s",
                     local_error->message);

          g_set_error (error,
                       G_IO_ERROR,
                       G_IO_ERROR_FAILED,
                       _("Failed to establish connection with the underlying wsdd daemon."));

          return FALSE;
        }
    }
  else
    {
      /* Force probe devices when the daemon is already running. */
      service->network_changed = TRUE;
    }

  service->socket_connection = g_socket_connection_factory_create_connection (service->socket);
  service->input_stream = g_io_stream_get_input_stream (G_IO_STREAM (service->socket_connection));
  service->data_input_stream = g_data_input_stream_new (service->input_stream);
  service->output_stream = g_io_stream_get_output_stream (G_IO_STREAM (service->socket_connection));

  schedule_reload_or_probe_devices (service);

  service->network_monitor = g_network_monitor_get_default ();
  g_signal_connect (service->network_monitor,
                    "network-changed",
                    G_CALLBACK (network_changed_cb),
                    service);

  return TRUE;
}

static void
initable_iface_init (GInitableIface *initable_iface)
{
  initable_iface->init = initable_init;
}

static void
async_initable_iface_init (GAsyncInitableIface *async_initable_iface)
{
}

GList *
g_vfs_wsdd_service_get_devices (GVfsWsddService *service, GError **error)
{
  if (service->error != NULL)
    {
      if (error != NULL)
        {
          *error = g_error_copy (service->error);
        }

      return NULL;
    }

  return service->devices;
}
