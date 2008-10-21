/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2008 Red Hat, Inc.
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
 * Author: David Zeuthen <davidz@redhat.com>
 */

/*
 * TODO: since we already call IsSupported() at module load time (in
 * order to register the appropriate types) we really just should
 * construct all the volume monitors. This is a good idea because
 *
 *  - instead of calling IsSupported() we just call List()
 *    - e.g. exactly the same IPC overhead
 *    - neglible memory + cpu overhead
 *      - will need to construct them at some point
 *    - we can actually implement get_mount_for_mount_path()
 *      correctly even when no volume monitor is constructed
 *
 *  - implement support for GMountOperation
 *    - not implemented in the HAL volume monitor and that's all
 *      that is using it right now. Will implement at some point
 *      when it's needed or someone has spare cycles.
 */

#include <config.h>

#include <limits.h>
#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>
#include <gdbusutils.h>

#include "gproxyvolumemonitor.h"
#include "gproxymount.h"
#include "gproxyvolume.h"
#include "gproxydrive.h"

G_LOCK_DEFINE_STATIC(proxy_vm);

static DBusConnection *the_session_bus = NULL;
static gboolean the_session_bus_is_integrated = FALSE;
static GHashTable *the_volume_monitors = NULL;

struct _GProxyVolumeMonitor {
  GNativeVolumeMonitor parent;
  DBusConnection *session_bus;

  GHashTable *drives;
  GHashTable *volumes;
  GHashTable *mounts;
};

G_DEFINE_DYNAMIC_TYPE_EXTENDED (GProxyVolumeMonitor,
                                g_proxy_volume_monitor,
                                G_TYPE_NATIVE_VOLUME_MONITOR,
                                G_TYPE_FLAG_ABSTRACT,
                                {})

static void seed_monitor (GProxyVolumeMonitor  *monitor);

static DBusHandlerResult filter_function (DBusConnection *connection, DBusMessage *message, void *user_data);

static void signal_emit_in_idle (gpointer object, const char *signal_name, gpointer other_object);

static gboolean is_supported (GProxyVolumeMonitorClass *klass);

/* The is_supported API is kinda lame and doesn't pass in the class,
   so we work around this with this hack */
typedef gboolean (*is_supported_func) (void);

static GProxyVolumeMonitorClass *is_supported_classes[10] = { NULL };
static gboolean is_supported_0 (void) { return is_supported (is_supported_classes[0]); };
static gboolean is_supported_1 (void) { return is_supported (is_supported_classes[1]); };
static gboolean is_supported_2 (void) { return is_supported (is_supported_classes[2]); };
static gboolean is_supported_3 (void) { return is_supported (is_supported_classes[3]); };
static gboolean is_supported_4 (void) { return is_supported (is_supported_classes[4]); };
static gboolean is_supported_5 (void) { return is_supported (is_supported_classes[5]); };
static gboolean is_supported_6 (void) { return is_supported (is_supported_classes[6]); };
static gboolean is_supported_7 (void) { return is_supported (is_supported_classes[7]); };
static gboolean is_supported_8 (void) { return is_supported (is_supported_classes[8]); };
static gboolean is_supported_9 (void) { return is_supported (is_supported_classes[9]); };
static is_supported_func is_supported_funcs[] = {
  is_supported_0, is_supported_1, is_supported_2, is_supported_3,
  is_supported_4, is_supported_5, is_supported_6, is_supported_7,
  is_supported_8, is_supported_9,
  NULL
};

static char *
get_match_rule (GProxyVolumeMonitor *monitor)
{
  return g_strdup_printf ("type='signal',"
                          "interface='org.gtk.Private.RemoteVolumeMonitor',"
                          "sender='%s'",
                          g_proxy_volume_monitor_get_dbus_name (monitor));
}

static void
g_proxy_volume_monitor_finalize (GObject *object)
{
  GProxyVolumeMonitor *monitor;
  DBusError dbus_error;
  char *match_rule;
  GObjectClass *parent_class;

  /* since GProxyVolumeMonitor is a non-instantiatable type we're dealing with a
   * sub-type here. So we need to look at the grandparent sub-type to get the
   * parent class for GProxyVolumeMonitor */
  parent_class = G_OBJECT_CLASS (g_type_class_peek_parent (
                                 g_type_class_peek_parent (G_OBJECT_GET_CLASS (object))));

  monitor = G_PROXY_VOLUME_MONITOR (object);

  g_hash_table_unref (monitor->drives);
  g_hash_table_unref (monitor->volumes);
  g_hash_table_unref (monitor->mounts);

  dbus_connection_remove_filter (monitor->session_bus, filter_function, monitor);
  match_rule = get_match_rule (monitor);
  dbus_error_init (&dbus_error);
  dbus_bus_remove_match (monitor->session_bus,
                         match_rule,
                         &dbus_error);
  if (dbus_error_is_set (&dbus_error)) {
    /* not really a whole lot to do on failure than whine since
     * GObject finalization can't fail...
     */
    g_warning ("cannot remove match rule '%s': %s: %s", match_rule, dbus_error.name, dbus_error.message);
    dbus_error_free (&dbus_error);
  }
  g_free (match_rule);
  dbus_connection_unref (monitor->session_bus);

  if (parent_class->finalize)
    parent_class->finalize (object);
}

static GList *
get_mounts (GVolumeMonitor *volume_monitor)
{
  GProxyVolumeMonitor *monitor;
  GList *l;
  GHashTableIter hash_iter;
  GProxyMount *mount;

  monitor = G_PROXY_VOLUME_MONITOR (volume_monitor);
  l = NULL;

  G_LOCK (proxy_vm);

  g_hash_table_iter_init (&hash_iter, monitor->mounts);
  while (g_hash_table_iter_next (&hash_iter, NULL, (gpointer) &mount))
    l = g_list_append (l, g_object_ref (mount));

  G_UNLOCK (proxy_vm);

  return l;
}

static GList *
get_volumes (GVolumeMonitor *volume_monitor)
{
  GProxyVolumeMonitor *monitor;
  GList *l;
  GHashTableIter hash_iter;
  GProxyVolume *volume;

  monitor = G_PROXY_VOLUME_MONITOR (volume_monitor);
  l = NULL;

  G_LOCK (proxy_vm);

  g_hash_table_iter_init (&hash_iter, monitor->volumes);
  while (g_hash_table_iter_next (&hash_iter, NULL, (gpointer) &volume))
    l = g_list_append (l, g_object_ref (volume));

  G_UNLOCK (proxy_vm);

  return l;
}

static GList *
get_connected_drives (GVolumeMonitor *volume_monitor)
{
  GProxyVolumeMonitor *monitor;
  GList *l;
  GHashTableIter hash_iter;
  GProxyDrive *drive;

  monitor = G_PROXY_VOLUME_MONITOR (volume_monitor);
  l = NULL;

  G_LOCK (proxy_vm);

  g_hash_table_iter_init (&hash_iter, monitor->drives);
  while (g_hash_table_iter_next (&hash_iter, NULL, (gpointer) &drive))
    l = g_list_append (l, g_object_ref (drive));

  G_UNLOCK (proxy_vm);

  return l;
}

static GVolume *
get_volume_for_uuid (GVolumeMonitor *volume_monitor, const char *uuid)
{
  GProxyVolumeMonitor *monitor;
  GHashTableIter hash_iter;
  GVolume *found_volume;
  GVolume *volume;

  monitor = G_PROXY_VOLUME_MONITOR (volume_monitor);

  G_LOCK (proxy_vm);

  found_volume = NULL;
  g_hash_table_iter_init (&hash_iter, monitor->volumes);
  while (g_hash_table_iter_next (&hash_iter, NULL, (gpointer) &volume) &&
         found_volume != NULL)
    {
      char *_uuid;
      _uuid = g_volume_get_uuid (volume);
      if (_uuid != NULL)
        {
          if (strcmp (uuid, _uuid) == 0)
            found_volume = g_object_ref (volume);
          g_free (_uuid);
        }
    }

  G_UNLOCK (proxy_vm);

  return found_volume;
}

static GMount *
get_mount_for_uuid (GVolumeMonitor *volume_monitor, const char *uuid)
{
  GProxyVolumeMonitor *monitor;
  GHashTableIter hash_iter;
  GMount *found_mount;
  GMount *mount;

  monitor = G_PROXY_VOLUME_MONITOR (volume_monitor);

  G_LOCK (proxy_vm);

  found_mount = NULL;
  g_hash_table_iter_init (&hash_iter, monitor->mounts);
  while (g_hash_table_iter_next (&hash_iter, NULL, (gpointer) &mount) &&
         found_mount != NULL)
    {
      char *_uuid;
      _uuid = g_mount_get_uuid (mount);
      if (_uuid != NULL)
        {
          if (strcmp (uuid, _uuid) == 0)
            found_mount = g_object_ref (mount);
          g_free (_uuid);
        }
    }

  G_UNLOCK (proxy_vm);

  return found_mount;
}

static GMount *
get_mount_for_mount_path (const char *mount_path,
                          GCancellable *cancellable)
{
  GMount *mount;
  GProxyVolumeMonitor *volume_monitor;
  GProxyVolumeMonitorClass *klass;
  GHashTableIter vm_hash_iter;
  GHashTableIter vol_hash_iter;
  GProxyMount *candidate_mount;

  /* This static method on GNativeVolumeMonitor is a *complete* pain
   * in the ass to deal with; we need to rework gio so it's deprecated
   * and thus never will get called.
   *
   * TODO: we don't handle the case when there's no volume monitor ever
   * constructed. See TODO at the top of this file on how to handle that.
   */

  mount = NULL;
  G_LOCK (proxy_vm);

  /* First find the native volume monitor if one exists */
  g_hash_table_iter_init (&vm_hash_iter, the_volume_monitors);
  while (g_hash_table_iter_next (&vm_hash_iter, NULL, (gpointer) &volume_monitor)) {
    klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (volume_monitor));

    if (klass->is_native) {
      /* The see if we've got a mount */
      g_hash_table_iter_init (&vol_hash_iter, volume_monitor->mounts);
      while (g_hash_table_iter_next (&vol_hash_iter, NULL, (gpointer) &candidate_mount)) {
        if (g_proxy_mount_has_mount_path (candidate_mount, mount_path))
          {
            mount = g_object_ref (candidate_mount);
            goto out;
          }
      }
      goto out;
    }
  }

 out:
  G_UNLOCK (proxy_vm);
  return mount;
}

static void
volume_monitor_went_away (gpointer data,
                          GObject *where_the_object_was)
{
  GType type = (GType) data;
  G_LOCK (proxy_vm);
  g_hash_table_remove (the_volume_monitors, (gpointer) type);
  G_UNLOCK (proxy_vm);
}

static GObject *
g_proxy_volume_monitor_constructor (GType                  type,
                                    guint                  n_construct_properties,
                                    GObjectConstructParam *construct_properties)
{
  GObject *object;
  GProxyVolumeMonitor *monitor;
  GProxyVolumeMonitorClass *klass;
  GObjectClass *parent_class;
  DBusError dbus_error;
  char *match_rule;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (g_type_class_peek (type));
  object = g_hash_table_lookup (the_volume_monitors, (gpointer) type);
  if (object != NULL)
    {
      g_object_ref (object);
      goto out;
    }

  /* Invoke parent constructor. */
  klass = G_PROXY_VOLUME_MONITOR_CLASS (g_type_class_peek (G_TYPE_PROXY_VOLUME_MONITOR));
  parent_class = G_OBJECT_CLASS (g_type_class_peek_parent (klass));
  object = parent_class->constructor (type,
                                      n_construct_properties,
                                      construct_properties);

  monitor = G_PROXY_VOLUME_MONITOR (object);

  dbus_error_init (&dbus_error);
  monitor->session_bus = dbus_connection_ref (the_session_bus);
  monitor->drives = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  monitor->volumes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  monitor->mounts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  dbus_connection_add_filter (monitor->session_bus, filter_function, monitor, NULL);
  match_rule = get_match_rule (monitor);
  dbus_bus_add_match (monitor->session_bus,
                      match_rule,
                      &dbus_error);
  if (dbus_error_is_set (&dbus_error)) {
    /* not really a whole lot to do on failure than whine since
     * GObject construction can't fail...
     */
    g_warning ("cannot add match rule '%s': %s: %s", match_rule, dbus_error.name, dbus_error.message);
    dbus_error_free (&dbus_error);
  }
  g_free (match_rule);

  seed_monitor (monitor);

  g_hash_table_insert (the_volume_monitors, (gpointer) type, object);
  g_object_weak_ref (G_OBJECT (object), volume_monitor_went_away, (gpointer) type);

 out:
  G_UNLOCK (proxy_vm);
  return object;
}

typedef struct {
  const char *signal_name;
  GObject *object;
  GObject *other_object;
} SignalEmitIdleData;

static gboolean
signal_emit_in_idle_do (SignalEmitIdleData *data)
{
  if (data->other_object != NULL)
    {
      g_signal_emit_by_name (data->object, data->signal_name, data->other_object);
      g_object_unref (data->other_object);
    }
  else
    {
      g_signal_emit_by_name (data->object, data->signal_name);
    }
  g_object_unref (data->object);
  g_free (data);

  return FALSE;
}

static void
signal_emit_in_idle (gpointer object, const char *signal_name, gpointer other_object)
{
  SignalEmitIdleData *data;

  data = g_new0 (SignalEmitIdleData, 1);
  data->signal_name = signal_name;
  data->object = g_object_ref (G_OBJECT (object));
  data->other_object = other_object != NULL ? g_object_ref (G_OBJECT (other_object)) : NULL;
  g_idle_add ((GSourceFunc) signal_emit_in_idle_do, data);
}



static DBusHandlerResult
filter_function (DBusConnection *connection, DBusMessage *message, void *user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  DBusMessageIter iter;
  const char *id;
  const char *the_dbus_name;
  const char *member;
  GProxyDrive *drive;
  GProxyVolume *volume;
  GProxyMount *mount;
  GProxyVolumeMonitorClass *klass;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  member = dbus_message_get_member (message);

  if (dbus_message_is_signal (message, "org.gtk.Private.RemoteVolumeMonitor", "DriveChanged") ||
      dbus_message_is_signal (message, "org.gtk.Private.RemoteVolumeMonitor", "DriveConnected") ||
      dbus_message_is_signal (message, "org.gtk.Private.RemoteVolumeMonitor", "DriveDisconnected") ||
      dbus_message_is_signal (message, "org.gtk.Private.RemoteVolumeMonitor", "DriveEjectButton")) {

    dbus_message_iter_init (message, &iter);
    dbus_message_iter_get_basic (&iter, &the_dbus_name);
    dbus_message_iter_next (&iter);
    dbus_message_iter_get_basic (&iter, &id);
    dbus_message_iter_next (&iter);

    if (strcmp (the_dbus_name, klass->dbus_name) != 0)
      goto not_for_us;

    if (strcmp (member, "DriveChanged") == 0)
      {
        drive = g_hash_table_lookup (monitor->drives, id);
        if (drive != NULL)
          {
            g_proxy_drive_update (drive, &iter);
            signal_emit_in_idle (drive, "changed", NULL);
            signal_emit_in_idle (monitor, "drive-changed", drive);
          }
      }
    else if (strcmp (member, "DriveConnected") == 0)
      {
        drive = g_hash_table_lookup (monitor->drives, id);
        if (drive == NULL)
          {
            drive = g_proxy_drive_new (monitor);
            g_proxy_drive_update (drive, &iter);
            g_hash_table_insert (monitor->drives, g_strdup (g_proxy_drive_get_id (drive)), drive);
            signal_emit_in_idle (monitor, "drive-connected", drive);
          }
      }
    else if (strcmp (member, "DriveDisconnected") == 0)
      {
        drive = g_hash_table_lookup (monitor->drives, id);
        if (drive != NULL)
          {
            g_object_ref (drive);
            g_hash_table_remove (monitor->drives, id);
            signal_emit_in_idle (drive, "disconnected", NULL);
            signal_emit_in_idle (monitor, "drive-disconnected", drive);
            g_object_unref (drive);
          }
      }
    else if (strcmp (member, "DriveEjectButton") == 0)
      {
        drive = g_hash_table_lookup (monitor->drives, id);
        if (drive != NULL)
          {
            signal_emit_in_idle (drive, "eject-button", NULL);
            signal_emit_in_idle (monitor, "drive-eject-button", drive);
          }
      }

  } else if (dbus_message_is_signal (message, "org.gtk.Private.RemoteVolumeMonitor", "VolumeChanged") ||
             dbus_message_is_signal (message, "org.gtk.Private.RemoteVolumeMonitor", "VolumeAdded") ||
             dbus_message_is_signal (message, "org.gtk.Private.RemoteVolumeMonitor", "VolumeRemoved")) {

    dbus_message_iter_init (message, &iter);
    dbus_message_iter_get_basic (&iter, &the_dbus_name);
    dbus_message_iter_next (&iter);
    dbus_message_iter_get_basic (&iter, &id);
    dbus_message_iter_next (&iter);

    if (strcmp (the_dbus_name, klass->dbus_name) != 0)
      goto not_for_us;

    if (strcmp (member, "VolumeChanged") == 0)
      {
        volume = g_hash_table_lookup (monitor->volumes, id);
        if (volume != NULL)
          {
            g_proxy_volume_update (volume, &iter);
            signal_emit_in_idle (volume, "changed", NULL);
            signal_emit_in_idle (monitor, "volume-changed", volume);
          }
      }
    else if (strcmp (member, "VolumeAdded") == 0)
      {
        volume = g_hash_table_lookup (monitor->volumes, id);
        if (volume == NULL)
          {
            volume = g_proxy_volume_new (monitor);
            g_proxy_volume_update (volume, &iter);
            g_hash_table_insert (monitor->volumes, g_strdup (g_proxy_volume_get_id (volume)), volume);
            signal_emit_in_idle (monitor, "volume-added", volume);
          }
      }
    else if (strcmp (member, "VolumeRemoved") == 0)
      {
        volume = g_hash_table_lookup (monitor->volumes, id);
        if (volume != NULL)
          {
            g_object_ref (volume);
            g_hash_table_remove (monitor->volumes, id);
            signal_emit_in_idle (volume, "removed", NULL);
            signal_emit_in_idle (monitor, "volume-removed", volume);
            g_object_unref (volume);
          }
      }

  } else if (dbus_message_is_signal (message, "org.gtk.Private.RemoteVolumeMonitor", "MountChanged") ||
             dbus_message_is_signal (message, "org.gtk.Private.RemoteVolumeMonitor", "MountAdded") ||
             dbus_message_is_signal (message, "org.gtk.Private.RemoteVolumeMonitor", "MountPreUnmount") ||
             dbus_message_is_signal (message, "org.gtk.Private.RemoteVolumeMonitor", "MountRemoved")) {

    dbus_message_iter_init (message, &iter);
    dbus_message_iter_get_basic (&iter, &the_dbus_name);
    dbus_message_iter_next (&iter);
    dbus_message_iter_get_basic (&iter, &id);
    dbus_message_iter_next (&iter);

    if (strcmp (the_dbus_name, klass->dbus_name) != 0)
      goto not_for_us;

    if (strcmp (member, "MountChanged") == 0)
      {
        mount = g_hash_table_lookup (monitor->mounts, id);
        if (mount != NULL)
          {
            g_proxy_mount_update (mount, &iter);
            signal_emit_in_idle (mount, "changed", NULL);
            signal_emit_in_idle (monitor, "mount-changed", mount);
          }
      }
    else if (strcmp (member, "MountAdded") == 0)
      {
        mount = g_hash_table_lookup (monitor->mounts, id);
        if (mount == NULL)
          {
            mount = g_proxy_mount_new (monitor);
            g_proxy_mount_update (mount, &iter);
            g_hash_table_insert (monitor->mounts, g_strdup (g_proxy_mount_get_id (mount)), mount);
            signal_emit_in_idle (monitor, "mount-added", mount);
          }
      }
    else if (strcmp (member, "MountPreUnmount") == 0)
      {
        mount = g_hash_table_lookup (monitor->mounts, id);
        if (mount != NULL)
          {
            signal_emit_in_idle (mount, "pre-unmount", NULL);
            signal_emit_in_idle (monitor, "mount-pre-unmount", mount);
          }
      }
    else if (strcmp (member, "MountRemoved") == 0)
      {
        mount = g_hash_table_lookup (monitor->mounts, id);
        if (mount != NULL)
          {
            g_object_ref (mount);
            g_hash_table_remove (monitor->mounts, id);
            signal_emit_in_idle (mount, "unmounted", NULL);
            signal_emit_in_idle (monitor, "mount-removed", mount);
            g_object_unref (mount);
          }
      }

  }

 not_for_us:
  G_UNLOCK (proxy_vm);

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void
g_proxy_volume_monitor_init (GProxyVolumeMonitor *monitor)
{
  g_proxy_volume_monitor_setup_session_bus_connection (TRUE);
}

static void
g_proxy_volume_monitor_class_finalize (GProxyVolumeMonitorClass *klass)
{
  g_free (klass->dbus_name);
}

typedef struct {
  char *dbus_name;
  gboolean is_native;
  int is_supported_nr;
} ProxyClassData;

static ProxyClassData *
proxy_class_data_new (const char *dbus_name, gboolean is_native)
{
  ProxyClassData *data;
  static int is_supported_nr = 0;
  
  data = g_new0 (ProxyClassData, 1);
  data->dbus_name = g_strdup (dbus_name);
  data->is_native = is_native;
  data->is_supported_nr = is_supported_nr++;

  g_assert (is_supported_funcs[data->is_supported_nr] != NULL);
  
  return data;
}

static void
g_proxy_volume_monitor_class_intern_init_pre (GProxyVolumeMonitorClass *klass, gconstpointer class_data)
{
  ProxyClassData *data = (ProxyClassData *) class_data;
  klass->dbus_name = g_strdup (data->dbus_name);
  klass->is_native = data->is_native;
  klass->is_supported_nr = data->is_supported_nr;
  g_proxy_volume_monitor_class_intern_init (klass);
}

static gboolean
is_remote_monitor_supported (const char *dbus_name)
{
  DBusMessage *message;
  DBusMessage *reply;
  DBusError dbus_error;
  dbus_bool_t is_supported;

  message = NULL;
  reply = NULL;
  is_supported = FALSE;

  message = dbus_message_new_method_call (dbus_name,
                                          "/",
                                          "org.gtk.Private.RemoteVolumeMonitor",
                                          "IsSupported");
  if (message == NULL)
    {
      g_warning ("Cannot allocate memory for DBusMessage");
      goto fail;
    }
  dbus_error_init (&dbus_error);
  reply = dbus_connection_send_with_reply_and_block (the_session_bus,
                                                     message,
                                                     -1,
                                                     &dbus_error);
  if (dbus_error_is_set (&dbus_error))
    {
      g_warning ("invoking IsSupported() failed for remote volume monitor with dbus name %s: %s: %s",
                 dbus_name,
                 dbus_error.name,
                 dbus_error.message);
      dbus_error_free (&dbus_error);
      goto fail;
    }

  if (!dbus_message_get_args (reply, &dbus_error,
                              DBUS_TYPE_BOOLEAN, &is_supported,
                              DBUS_TYPE_INVALID))
    {
      g_warning ("Error parsing args in reply for IsSupported(): %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto fail;
    }

  if (!is_supported)
    g_warning ("remote volume monitor with dbus name %s is not supported", dbus_name);

 fail:
  if (message != NULL)
    dbus_message_unref (message);
  if (reply != NULL)
    dbus_message_unref (reply);
  return is_supported;
}

static gboolean
is_supported (GProxyVolumeMonitorClass *klass)
{
  gboolean res;

  G_LOCK (proxy_vm);
  res = g_proxy_volume_monitor_setup_session_bus_connection (FALSE);
  G_UNLOCK (proxy_vm);
  
  if (res)
    res = is_remote_monitor_supported (klass->dbus_name);

  return res;
}

static GVolume *
adopt_orphan_mount (GMount *mount, GVolumeMonitor *monitor)
{
  GProxyVolumeMonitor *proxy_monitor = G_PROXY_VOLUME_MONITOR (monitor);
  GFile *mount_root;
  GProxyVolume *proxy_volume;
  GVolume *ret;
  GHashTableIter hash_iter;

  ret = NULL;

  G_LOCK (proxy_vm);

  mount_root = g_mount_get_root (mount);

  /* TODO: consider what happens if two volumes wants to adopt the same mount?
   *
   * e.g. imagine two GVolume objects with activation_roots
   *
   *      ssh://server/dir1
   *      ssh://server/dir2
   */

  g_hash_table_iter_init (&hash_iter, proxy_monitor->volumes);
  while (g_hash_table_iter_next (&hash_iter, NULL, (gpointer) &proxy_volume))
    {
      GFile *activation_root;

      activation_root = g_volume_get_activation_root (G_VOLUME (proxy_volume));
      if (activation_root != NULL)
        {
          if (g_file_has_prefix (activation_root, mount_root) ||
              g_file_equal (activation_root, mount_root))
            {
              g_proxy_volume_adopt_foreign_mount (proxy_volume, mount);
              ret = g_object_ref (proxy_volume);
              g_object_unref (activation_root);
              goto found;
            }
          g_object_unref (activation_root);
        }
    }

 found:
  g_object_unref (mount_root);

  G_UNLOCK (proxy_vm);
  return ret;
}

static void
g_proxy_volume_monitor_class_init (GProxyVolumeMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVolumeMonitorClass *monitor_class = G_VOLUME_MONITOR_CLASS (klass);
  GNativeVolumeMonitorClass *native_class = G_NATIVE_VOLUME_MONITOR_CLASS (klass);
  int i;

  gobject_class->constructor = g_proxy_volume_monitor_constructor;
  gobject_class->finalize = g_proxy_volume_monitor_finalize;

  monitor_class->get_mounts = get_mounts;
  monitor_class->get_volumes = get_volumes;
  monitor_class->get_connected_drives = get_connected_drives;
  monitor_class->get_volume_for_uuid = get_volume_for_uuid;
  monitor_class->get_mount_for_uuid = get_mount_for_uuid;
  monitor_class->adopt_orphan_mount = adopt_orphan_mount;

  i = klass->is_supported_nr;
  is_supported_classes[i] = klass;
  monitor_class->is_supported = is_supported_funcs[i];

  native_class->get_mount_for_mount_path = get_mount_for_mount_path;
}

/* Must be called with no locks held */
static void
seed_monitor (GProxyVolumeMonitor *monitor)
{
  DBusMessage *message;
  DBusMessage *reply;
  DBusError dbus_error;
  DBusMessageIter iter_reply;
  DBusMessageIter iter_array;

  message = dbus_message_new_method_call (g_proxy_volume_monitor_get_dbus_name (monitor),
                                          "/",
                                          "org.gtk.Private.RemoteVolumeMonitor",
                                          "List");
  if (message == NULL)
    {
      g_warning ("Cannot allocate memory for DBusMessage");
      goto fail;
    }
  dbus_error_init (&dbus_error);
  reply = dbus_connection_send_with_reply_and_block (monitor->session_bus,
                                                     message,
                                                     -1,
                                                     &dbus_error);
  dbus_message_unref (message);
  if (dbus_error_is_set (&dbus_error))
    {
      g_warning ("invoking List() failed for type %s: %s: %s",
                 G_OBJECT_TYPE_NAME (monitor),
                 dbus_error.name,
                 dbus_error.message);
      dbus_error_free (&dbus_error);
      goto fail;
    }

  dbus_message_iter_init (reply, &iter_reply);

  /* TODO: verify signature */

  /* drives */
  dbus_message_iter_recurse (&iter_reply, &iter_array);
  while (dbus_message_iter_get_arg_type (&iter_array) != DBUS_TYPE_INVALID)
    {
      GProxyDrive *drive;
      const char *id;
      drive = g_proxy_drive_new (monitor);
      g_proxy_drive_update (drive, &iter_array);
      id = g_proxy_drive_get_id (drive);
      g_hash_table_insert (monitor->drives, g_strdup (id), drive);
      dbus_message_iter_next (&iter_array);
    }
  dbus_message_iter_next (&iter_reply);

  /* volumes */
  dbus_message_iter_recurse (&iter_reply, &iter_array);
  while (dbus_message_iter_get_arg_type (&iter_array) != DBUS_TYPE_INVALID)
    {
      GProxyVolume *volume;
      const char *id;
      volume = g_proxy_volume_new (monitor);
      g_proxy_volume_update (volume, &iter_array);
      id = g_proxy_volume_get_id (volume);
      g_hash_table_insert (monitor->volumes, g_strdup (id), volume);
      dbus_message_iter_next (&iter_array);
    }
  dbus_message_iter_next (&iter_reply);

  /* mounts */
  dbus_message_iter_recurse (&iter_reply, &iter_array);
  while (dbus_message_iter_get_arg_type (&iter_array) != DBUS_TYPE_INVALID)
    {
      GProxyMount *mount;
      const char *id;
      mount = g_proxy_mount_new (monitor);
      g_proxy_mount_update (mount, &iter_array);
      id = g_proxy_mount_get_id (mount);
      g_hash_table_insert (monitor->mounts, g_strdup (id), mount);
      dbus_message_iter_next (&iter_array);
    }
  dbus_message_iter_next (&iter_reply);

  dbus_message_unref (reply);

 fail:
  ;
}

GProxyDrive *
g_proxy_volume_monitor_get_drive_for_id  (GProxyVolumeMonitor *volume_monitor,
                                          const char          *id)
{
  GProxyDrive *drive;

  G_LOCK (proxy_vm);
  drive = g_hash_table_lookup (volume_monitor->drives, id);
  if (drive != NULL)
    g_object_ref (drive);
  G_UNLOCK (proxy_vm);

  return drive;
}

GProxyVolume *
g_proxy_volume_monitor_get_volume_for_id (GProxyVolumeMonitor *volume_monitor,
                                          const char          *id)
{
  GProxyVolume *volume;

  G_LOCK (proxy_vm);
  volume = g_hash_table_lookup (volume_monitor->volumes, id);
  if (volume != NULL)
    g_object_ref (volume);
  G_UNLOCK (proxy_vm);

  return volume;
}

GProxyMount *
g_proxy_volume_monitor_get_mount_for_id  (GProxyVolumeMonitor *volume_monitor,
                                          const char          *id)
{
  GProxyMount *mount;

  G_LOCK (proxy_vm);
  mount = g_hash_table_lookup (volume_monitor->mounts, id);
  if (mount != NULL)
    g_object_ref (mount);
  G_UNLOCK (proxy_vm);

  return mount;
}


GHashTable *
_get_identifiers (DBusMessageIter *iter)
{
  GHashTable *hash_table;
  DBusMessageIter iter_array;

  hash_table = g_hash_table_new_full (g_str_hash,
                                      g_str_equal,
                                      g_free,
                                      g_free);

  dbus_message_iter_recurse (iter, &iter_array);
  while (dbus_message_iter_get_arg_type (&iter_array) != DBUS_TYPE_INVALID)
    {
      DBusMessageIter iter_dict_entry;
      const char *key;
      const char *value;

      dbus_message_iter_recurse (&iter_array, &iter_dict_entry);
      dbus_message_iter_get_basic (&iter_dict_entry, &key);
      dbus_message_iter_next (&iter_dict_entry);
      dbus_message_iter_get_basic (&iter_dict_entry, &value);

      g_hash_table_insert (hash_table, g_strdup (key), g_strdup (value));

      dbus_message_iter_next (&iter_array);
    }

  return hash_table;
}

DBusConnection *
g_proxy_volume_monitor_get_dbus_connection (GProxyVolumeMonitor *volume_monitor)
{
  return dbus_connection_ref (volume_monitor->session_bus);
}

const char *
g_proxy_volume_monitor_get_dbus_name (GProxyVolumeMonitor *volume_monitor)
{
  GProxyVolumeMonitorClass *klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (volume_monitor));
  return klass->dbus_name;
}

static void
register_volume_monitor (GTypeModule *type_module,
                         const char *type_name,
                         const char *dbus_name,
                         gboolean is_native,
                         int priority)
{
  GType type;
  const GTypeInfo type_info = {
    sizeof (GProxyVolumeMonitorClass),
    (GBaseInitFunc) NULL,
    (GBaseFinalizeFunc) NULL,
    (GClassInitFunc) g_proxy_volume_monitor_class_intern_init_pre,
    (GClassFinalizeFunc) g_proxy_volume_monitor_class_finalize,
    (gconstpointer) proxy_class_data_new (dbus_name, is_native),  /* class_data (leaked!) */
    sizeof (GProxyVolumeMonitor),
    0,      /* n_preallocs */
    (GInstanceInitFunc) g_proxy_volume_monitor_init,
    NULL    /* value_table */
  };

  type = g_type_module_register_type (type_module,
                                      G_TYPE_PROXY_VOLUME_MONITOR,
                                      type_name,
                                      &type_info,
                                      0 /* type_flags */);

  g_io_extension_point_implement (is_native ? G_NATIVE_VOLUME_MONITOR_EXTENSION_POINT_NAME :
                                              G_VOLUME_MONITOR_EXTENSION_POINT_NAME,
                                  type,
                                  type_name,
                                  priority);
}

/* Call with proxy_vm lock held */
gboolean
g_proxy_volume_monitor_setup_session_bus_connection (gboolean need_integration)
{
  gboolean ret;
  DBusError dbus_error;

  ret = FALSE;

  if (the_session_bus != NULL)
    goto has_bus_already;

  /* This is so that system daemons can use gio
   * without spawning private dbus instances.
   * See bug 526454.
   */
  if (g_getenv ("DBUS_SESSION_BUS_ADDRESS") == NULL)
    goto out;

  dbus_error_init (&dbus_error);
  the_session_bus = dbus_bus_get_private (DBUS_BUS_SESSION, &dbus_error);
  if (dbus_error_is_set (&dbus_error))
    {
      g_warning ("cannot connect to the session bus: %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }

  the_volume_monitors = g_hash_table_new (g_direct_hash, g_direct_equal);

 has_bus_already:
  
  if (need_integration && !the_session_bus_is_integrated)
    {
      _g_dbus_connection_integrate_with_main (the_session_bus);
      the_session_bus_is_integrated = TRUE;
    }
  
  ret = TRUE;

 out:
  return ret;
}

void
g_proxy_volume_monitor_teardown_session_bus_connection (void)
{
  G_LOCK (proxy_vm);
  if (the_session_bus != NULL)
    {
      if (the_session_bus_is_integrated)
        _g_dbus_connection_remove_from_main (the_session_bus);
      the_session_bus_is_integrated = FALSE;      
      dbus_connection_close (the_session_bus);
      the_session_bus = NULL;

      g_hash_table_unref (the_volume_monitors);
      the_volume_monitors = NULL;
    }
  G_UNLOCK (proxy_vm);
}

void
g_proxy_volume_monitor_register (GIOModule *module)
{
  GDir *dir;
  GError *error;

  /* first register the abstract base type... */
  g_proxy_volume_monitor_register_type (G_TYPE_MODULE (module));

  /* ... then register instantiable types for each remote volume
   * monitor - each remote volume monitor is defined in a key-value
   * file in $(datadir)/gvfs/remote-volume-monitors that must have
   * the suffix .monitor. Each file specifies
   *
   * - the name of the volume monitor
   * - the name of the D-Bus service
   * - whether the volume monitor is native
   *   - and if so the priority
   */

  error = NULL;
  dir = g_dir_open (REMOTE_VOLUME_MONITORS_DIR, 0, &error);
  if (dir == NULL)
    {
      g_warning ("cannot open directory " REMOTE_VOLUME_MONITORS_DIR ": %s", error->message);
      g_error_free (error);
    }
  else
    {
      const char *name;

      while ((name = g_dir_read_name (dir)) != NULL)
        {
          GKeyFile *key_file;
          char *type_name;
          char *path;
          char *dbus_name;
          gboolean is_native;
          int native_priority;

          type_name = NULL;
          key_file = NULL;
          dbus_name = NULL;
          path = NULL;

          if (!g_str_has_suffix (name, ".monitor"))
            goto cont;

          path = g_build_filename (REMOTE_VOLUME_MONITORS_DIR, name, NULL);

          key_file = g_key_file_new ();
          error = NULL;
          if (!g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, &error))
            {
              g_warning ("error loading key-value file %s: %s", path, error->message);
              g_error_free (error);
              goto cont;
            }

          type_name = g_key_file_get_string (key_file, "RemoteVolumeMonitor", "Name", &error);
          if (error != NULL)
            {
              g_warning ("error extracting Name key from %s: %s", path, error->message);
              g_error_free (error);
              goto cont;
            }

          dbus_name = g_key_file_get_string (key_file, "RemoteVolumeMonitor", "DBusName", &error);
          if (error != NULL)
            {
              g_warning ("error extracting DBusName key from %s: %s", path, error->message);
              g_error_free (error);
              goto cont;
            }

          is_native = g_key_file_get_boolean (key_file, "RemoteVolumeMonitor", "IsNative", &error);
          if (error != NULL)
            {
              g_warning ("error extracting IsNative key from %s: %s", path, error->message);
              g_error_free (error);
              goto cont;
            }

          if (is_native)
            {
              native_priority = g_key_file_get_integer (key_file, "RemoteVolumeMonitor", "NativePriority", &error);
              if (error != NULL)
                {
                  g_warning ("error extracting NativePriority key from %s: %s", path, error->message);
                  g_error_free (error);
                  goto cont;
                }
            }
          else
            {
              native_priority = 0;
            }

          register_volume_monitor (G_TYPE_MODULE (module),
                                   type_name,
                                   dbus_name,
                                   is_native,
                                   native_priority);

        cont:

          g_free (type_name);
          g_free (dbus_name);
          g_free (path);
          if (key_file != NULL)
              g_key_file_free (key_file);
        }
      g_dir_close (dir);
    }
}
