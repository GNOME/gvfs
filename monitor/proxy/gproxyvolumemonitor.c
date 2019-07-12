/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/* gvfs - extensions for gio
 *
 * Copyright (C) 2006-2009 Red Hat, Inc.
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
 * Author: David Zeuthen <davidz@redhat.com>
 */

/* TODO: handle force_rescan in g_mount_guess_content_type(); right now we
 *       just scan in the daemon first time the GMount is seen and
 *       cache that result forever.
 */

#include <config.h>

#include <limits.h>
#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include "gproxyvolumemonitor.h"
#include "gproxymount.h"
#include "gproxyvolume.h"
#include "gproxydrive.h"
#include "gproxymountoperation.h"
#include "gvfsvolumemonitordbus.h"
#include "gvfsmonitorimpl.h"
#include "gvfsdbus.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsutils.h"

G_LOCK_DEFINE_STATIC(proxy_vm);

static GHashTable *the_volume_monitors = NULL;

struct _GProxyVolumeMonitor {
  GNativeVolumeMonitor parent;
  
  guint name_owner_id;
  GVfsRemoteVolumeMonitor *proxy;

  GHashTable *drives;
  GHashTable *volumes;
  GHashTable *mounts;

  gulong name_watcher_id;
};

G_DEFINE_DYNAMIC_TYPE_EXTENDED (GProxyVolumeMonitor,
                                g_proxy_volume_monitor,
                                G_TYPE_NATIVE_VOLUME_MONITOR,
                                G_TYPE_FLAG_ABSTRACT,
                                {})

static gboolean g_proxy_volume_monitor_setup_session_bus_connection (void);

static void seed_monitor (GProxyVolumeMonitor  *monitor);

static void signal_emit_in_idle (gpointer object, const char *signal_name, gpointer other_object);

static void dispose_in_idle (gpointer object);

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

static void
g_proxy_volume_monitor_finalize (GObject *object)
{
  g_warning ("finalize() called on instance of type %s but instances of this type "
             "are supposed to live forever. This is a reference counting bug in "
             "the application or library using GVolumeMonitor.",
             g_type_name (G_OBJECT_TYPE (object)));
}

static void
g_proxy_volume_monitor_dispose (GObject *object)
{
  /* since we want instances to live forever, don't even chain up since
   * GObject's default implementation destroys e.g. qdata on the instance
   */
}

static gboolean
drive_compare (GDrive *a, GDrive *b)
{
  return g_strcmp0 (g_drive_get_sort_key (a), g_drive_get_sort_key (b));
}

static gboolean
volume_compare (GVolume *a, GVolume *b)
{
  return g_strcmp0 (g_volume_get_sort_key (a), g_volume_get_sort_key (b));
}

static gboolean
mount_compare (GMount *a, GMount *b)
{
  return g_strcmp0 (g_mount_get_sort_key (a), g_mount_get_sort_key (b));
}

static GList *
get_mounts (GVolumeMonitor *volume_monitor)
{
  GProxyVolumeMonitor *monitor;
  GList *l;
  GHashTableIter hash_iter;
  GProxyMount *mount;
  GProxyVolume *volume;

  monitor = G_PROXY_VOLUME_MONITOR (volume_monitor);
  l = NULL;

  G_LOCK (proxy_vm);

  g_hash_table_iter_init (&hash_iter, monitor->mounts);
  while (g_hash_table_iter_next (&hash_iter, NULL, (gpointer) &mount))
    l = g_list_append (l, g_object_ref (mount));

  /* also return shadow mounts */
  g_hash_table_iter_init (&hash_iter, monitor->volumes);
  while (g_hash_table_iter_next (&hash_iter, NULL, (gpointer) &volume))
    {
      GProxyShadowMount *shadow_mount;
      shadow_mount = g_proxy_volume_get_shadow_mount (volume);
      if (shadow_mount != NULL)
        l = g_list_append (l, shadow_mount);
    }

  G_UNLOCK (proxy_vm);

  l = g_list_sort (l, (GCompareFunc) mount_compare);

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

  l = g_list_sort (l, (GCompareFunc) volume_compare);

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

  l = g_list_sort (l, (GCompareFunc) drive_compare);

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
         found_volume == NULL)
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
         found_mount == NULL)
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
  static GVolumeMonitor *union_monitor = NULL;

  /* There's a problem here insofar that this static method on GNativeVolumeMonitor can
   * be called *before* any of our monitors are constructed. Since this method doesn't
   * pass in the class structure we *do not know* which native remote monitor to use.
   *
   * To work around that, we get the singleton GVolumeMonitor... This will trigger
   * construction of a GUnionVolumeMonitor in gio which will construct the *appropriate*
   * remote volume monitors to use (it's up to gio to pick which one to use).
   *
   * Note that we will *hold* on to this reference effectively making us a resident
   * module. And effectively keeping volume monitoring alive.
   *
   * The reason we hold on to the reference is that otherwise we'd be constructing/destructing
   * *all* proxy volume monitors (which includes synchronous D-Bus calls to seed the monitor)
   * every time this method is called.
   *
   * Note that *simple* GIO apps that a) don't use volume monitors; and b) don't use the
   * g_file_find_enclosing_mount() method will never see any volume monitor overhead.
   */

  /* Note that g_volume_monitor_get() is thread safe. We don't want to call it while
   * holding the proxy_vm lock since it might end up calling our constructor.
   */
  if (union_monitor == NULL)
    union_monitor = g_volume_monitor_get ();

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
            mount = G_MOUNT (g_object_ref (candidate_mount));
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
drive_changed (GVfsRemoteVolumeMonitor *object,
               const gchar *arg_dbus_name,
               const gchar *arg_id,
               GVariant *arg_drive,
               gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;
  GProxyDrive *d;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  d = g_hash_table_lookup (monitor->drives, arg_id);
  if (d != NULL)
    {
      g_proxy_drive_update (d, arg_drive);
      signal_emit_in_idle (d, "changed", NULL);
      signal_emit_in_idle (monitor, "drive-changed", d);
    }
    
  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
drive_connected (GVfsRemoteVolumeMonitor *object,
                 const gchar *arg_dbus_name,
                 const gchar *arg_id,
                 GVariant *arg_drive,
                 gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;
  GProxyDrive *d;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  d = g_hash_table_lookup (monitor->drives, arg_id);
  if (d == NULL)
    {
      d = g_proxy_drive_new (monitor);
      g_proxy_drive_update (d, arg_drive);
      g_hash_table_insert (monitor->drives, g_strdup (g_proxy_drive_get_id (d)), d);
      signal_emit_in_idle (monitor, "drive-connected", d);
    }
    
  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
drive_disconnected (GVfsRemoteVolumeMonitor *object,
                    const gchar *arg_dbus_name,
                    const gchar *arg_id,
                    GVariant *arg_drive,
                    gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;
  GProxyDrive *d;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  d = g_hash_table_lookup (monitor->drives, arg_id);
  if (d != NULL)
    {
      g_object_ref (d);
      g_hash_table_remove (monitor->drives, arg_id);
      signal_emit_in_idle (d, "disconnected", NULL);
      signal_emit_in_idle (monitor, "drive-disconnected", d);
      g_object_unref (d);
    }
  
  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
drive_eject_button (GVfsRemoteVolumeMonitor *object,
                    const gchar *arg_dbus_name,
                    const gchar *arg_id,
                    GVariant *arg_drive,
                    gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;
  GProxyDrive *d;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  d = g_hash_table_lookup (monitor->drives, arg_id);
  if (d != NULL)
    {
      signal_emit_in_idle (d, "eject-button", NULL);
      signal_emit_in_idle (monitor, "drive-eject-button", d);
    }

  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
drive_stop_button (GVfsRemoteVolumeMonitor *object,
                   const gchar *arg_dbus_name,
                   const gchar *arg_id,
                   GVariant *arg_drive,
                   gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;
  GProxyDrive *d;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  d = g_hash_table_lookup (monitor->drives, arg_id);
  if (d != NULL)
    {
      signal_emit_in_idle (d, "stop-button", NULL);
      signal_emit_in_idle (monitor, "drive-stop-button", d);
    }

  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
mount_added (GVfsRemoteVolumeMonitor *object,
             const gchar *arg_dbus_name,
             const gchar *arg_id,
             GVariant *arg_mount,
             gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;
  GProxyMount *m;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  m = g_hash_table_lookup (monitor->mounts, arg_id);
  if (m == NULL)
    {
      m = g_proxy_mount_new (monitor);
      g_proxy_mount_update (m, arg_mount);
      g_hash_table_insert (monitor->mounts, g_strdup (g_proxy_mount_get_id (m)), m);
      signal_emit_in_idle (monitor, "mount-added", m);
    }
    
  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
mount_changed (GVfsRemoteVolumeMonitor *object,
               const gchar *arg_dbus_name,
               const gchar *arg_id,
               GVariant *arg_mount,
               gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;
  GProxyMount *m;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  m = g_hash_table_lookup (monitor->mounts, arg_id);
  if (m != NULL)
    {
      g_proxy_mount_update (m, arg_mount);
      signal_emit_in_idle (m, "changed", NULL);
      signal_emit_in_idle (monitor, "mount-changed", m);
    }
    
  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
mount_pre_unmount (GVfsRemoteVolumeMonitor *object,
                   const gchar *arg_dbus_name,
                   const gchar *arg_id,
                   GVariant *arg_mount,
                   gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;
  GProxyMount *m;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  m = g_hash_table_lookup (monitor->mounts, arg_id);
  if (m != NULL)
    {
      signal_emit_in_idle (m, "pre-unmount", NULL);
      signal_emit_in_idle (monitor, "mount-pre-unmount", m);
    }

  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
mount_removed (GVfsRemoteVolumeMonitor *object,
               const gchar *arg_dbus_name,
               const gchar *arg_id,
               GVariant *arg_mount,
               gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;
  GProxyMount *m;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  m = g_hash_table_lookup (monitor->mounts, arg_id);
  if (m != NULL)
    {
      g_object_ref (m);
      g_hash_table_remove (monitor->mounts, arg_id);
      signal_emit_in_idle (m, "unmounted", NULL);
      signal_emit_in_idle (monitor, "mount-removed", m);
      g_object_unref (m);
    }
    
  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
mount_op_aborted (GVfsRemoteVolumeMonitor *object,
                  const gchar *arg_dbus_name,
                  const gchar *arg_id,
                  gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  g_proxy_mount_operation_handle_aborted (arg_id);

  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
mount_op_ask_password (GVfsRemoteVolumeMonitor *object,
                       const gchar *arg_dbus_name,
                       const gchar *arg_id,
                       const gchar *arg_message_to_show,
                       const gchar *arg_default_user,
                       const gchar *arg_default_domain,
                       guint arg_flags,
                       gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  g_proxy_mount_operation_handle_ask_password (arg_id,
                                               arg_message_to_show,
                                               arg_default_user,
                                               arg_default_domain,
                                               arg_flags);

  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
mount_op_ask_question (GVfsRemoteVolumeMonitor *object,
                       const gchar *arg_dbus_name,
                       const gchar *arg_id,
                       const gchar *arg_message_to_show,
                       const gchar *const *arg_choices,
                       gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  g_proxy_mount_operation_handle_ask_question (arg_id,
                                               arg_message_to_show,
                                               arg_choices);

  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
mount_op_show_processes (GVfsRemoteVolumeMonitor *object,
                         const gchar *arg_dbus_name,
                         const gchar *arg_id,
                         const gchar *arg_message_to_show,
                         GVariant *arg_pid,
                         const gchar *const *arg_choices,
                         gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  g_proxy_mount_operation_handle_show_processes (arg_id,
                                                 arg_message_to_show,
                                                 arg_pid,
                                                 arg_choices);

  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
mount_op_show_unmount_progress (GVfsRemoteVolumeMonitor *object,
                                const gchar *arg_dbus_name,
                                const gchar *arg_id,
                                const gchar *arg_message_to_show,
                                gint64       arg_time_left,
                                gint64       arg_bytes_left,
                                gpointer     user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  g_proxy_mount_operation_handle_show_unmount_progress (arg_id,
                                                        arg_message_to_show,
                                                        arg_time_left,
                                                        arg_bytes_left);

  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
volume_added (GVfsRemoteVolumeMonitor *object,
              const gchar *arg_dbus_name,
              const gchar *arg_id,
              GVariant *arg_volume,
              gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;
  GProxyVolume *v;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  v = g_hash_table_lookup (monitor->volumes, arg_id);
  if (v == NULL)
    {
      v = g_proxy_volume_new (monitor);
      g_proxy_volume_update (v, arg_volume);
      g_hash_table_insert (monitor->volumes, g_strdup (g_proxy_volume_get_id (v)), v);
      signal_emit_in_idle (monitor, "volume-added", v);
    }
    
  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
volume_changed (GVfsRemoteVolumeMonitor *object,
                const gchar *arg_dbus_name,
                const gchar *arg_id,
                GVariant *arg_volume,
                gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;
  GProxyVolume *v;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  v = g_hash_table_lookup (monitor->volumes, arg_id);
  if (v != NULL)
    {
      GProxyShadowMount *shadow_mount;

      g_proxy_volume_update (v, arg_volume);
      signal_emit_in_idle (v, "changed", NULL);
      signal_emit_in_idle (monitor, "volume-changed", v);

      shadow_mount = g_proxy_volume_get_shadow_mount (v);
      if (shadow_mount != NULL)
        {
          signal_emit_in_idle (shadow_mount, "changed", NULL);
          signal_emit_in_idle (monitor, "mount-changed", shadow_mount);
          g_object_unref (shadow_mount);
        }
    }

  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
volume_removed (GVfsRemoteVolumeMonitor *object,
                const gchar *arg_dbus_name,
                const gchar *arg_id,
                GVariant *arg_volume,
                gpointer user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;
  GProxyVolume *v;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  if (strcmp (arg_dbus_name, klass->dbus_name) != 0)
    goto not_for_us;
  
  v = g_hash_table_lookup (monitor->volumes, arg_id);
  if (v != NULL)
    {
      g_object_ref (v);
      g_hash_table_remove (monitor->volumes, arg_id);
      signal_emit_in_idle (v, "removed", NULL);
      signal_emit_in_idle (monitor, "volume-removed", v);
      dispose_in_idle (v);
      g_object_unref (v);
    }
    
  not_for_us:
   G_UNLOCK (proxy_vm);
}

static void
name_owner_appeared (GProxyVolumeMonitor *monitor)
{
  GHashTableIter hash_iter;
  GProxyDrive *drive;
  GProxyVolume *volume;
  GProxyMount *mount;

  G_LOCK (proxy_vm);

  seed_monitor (monitor);

  /* emit signals for all the drives/volumes/mounts "added" */
  g_hash_table_iter_init (&hash_iter, monitor->drives);
  while (g_hash_table_iter_next (&hash_iter, NULL, (gpointer) &drive))
    signal_emit_in_idle (monitor, "drive-connected", drive);

  g_hash_table_iter_init (&hash_iter, monitor->volumes);
  while (g_hash_table_iter_next (&hash_iter, NULL, (gpointer) &volume))
    signal_emit_in_idle (monitor, "volume-added", volume);

  g_hash_table_iter_init (&hash_iter, monitor->mounts);
  while (g_hash_table_iter_next (&hash_iter, NULL, (gpointer) &mount))
    signal_emit_in_idle (monitor, "mount-added", mount);

  G_UNLOCK (proxy_vm);
}

static void
name_owner_vanished (GProxyVolumeMonitor *monitor)
{
  GHashTableIter hash_iter;
  GProxyDrive *drive;
  GProxyVolume *volume;
  GProxyMount *mount;

  G_LOCK (proxy_vm);

  g_hash_table_iter_init (&hash_iter, monitor->mounts);
  while (g_hash_table_iter_next (&hash_iter, NULL, (gpointer) &mount))
    {
      signal_emit_in_idle (mount, "unmounted", NULL);
      signal_emit_in_idle (monitor, "mount-removed", mount);
    }
  g_hash_table_remove_all (monitor->mounts);

  g_hash_table_iter_init (&hash_iter, monitor->volumes);
  while (g_hash_table_iter_next (&hash_iter, NULL, (gpointer) &volume))
    {
      signal_emit_in_idle (volume, "removed", NULL);
      signal_emit_in_idle (monitor, "volume-removed", volume);
    }
  g_hash_table_remove_all (monitor->volumes);

  g_hash_table_iter_init (&hash_iter, monitor->drives);
  while (g_hash_table_iter_next (&hash_iter, NULL, (gpointer) &drive))
    {
      signal_emit_in_idle (drive, "disconnected", NULL);
      signal_emit_in_idle (monitor, "drive-disconnected", drive);
    }
  g_hash_table_remove_all (monitor->drives);

  G_UNLOCK (proxy_vm);
}

static void
name_owner_changed (GObject    *gobject,
                    GParamSpec *pspec,
                    gpointer    user_data)
{
  GProxyVolumeMonitor *monitor = G_PROXY_VOLUME_MONITOR (user_data);
  GProxyVolumeMonitorClass *klass;
  gchar *name_owner = NULL;

  klass = G_PROXY_VOLUME_MONITOR_CLASS (G_OBJECT_GET_CLASS (monitor));

  g_object_get (gobject, "g-name-owner", &name_owner, NULL);

  if (name_owner != NULL)
    {
      name_owner_appeared (monitor);
    }
  else
    {
      g_warning ("Owner of volume monitor %s disconnected from the bus; removing drives/volumes/mounts",
                 klass->dbus_name);

      name_owner_vanished (monitor);

      /* TODO: maybe try to relaunch the monitor? */
  }

  g_free (name_owner);
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
  GError *error;
  const char *dbus_name;
  gchar *name_owner;

  G_LOCK (proxy_vm);

  klass = G_PROXY_VOLUME_MONITOR_CLASS (g_type_class_peek (type));
  object = g_hash_table_lookup (the_volume_monitors, (gpointer) type);
  if (object != NULL)
    goto out;

  dbus_name = klass->dbus_name;

  /* Invoke parent constructor. */
  klass = G_PROXY_VOLUME_MONITOR_CLASS (g_type_class_peek (G_TYPE_PROXY_VOLUME_MONITOR));
  parent_class = G_OBJECT_CLASS (g_type_class_peek_parent (klass));
  object = parent_class->constructor (type,
                                      n_construct_properties,
                                      construct_properties);

  monitor = G_PROXY_VOLUME_MONITOR (object);

  monitor->drives = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  monitor->volumes = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  monitor->mounts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  error = NULL;
  monitor->proxy = gvfs_remote_volume_monitor_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                                      G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                                      dbus_name,
                                                                      "/org/gtk/Private/RemoteVolumeMonitor",
                                                                      NULL,
                                                                      &error);
  if (monitor->proxy == NULL)
    {
      g_printerr ("Error creating proxy: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      goto out;
    }

  /* listen to volume monitor signals */
  g_signal_connect (monitor->proxy, "drive-changed", G_CALLBACK (drive_changed), monitor);
  g_signal_connect (monitor->proxy, "drive-connected", G_CALLBACK (drive_connected), monitor);
  g_signal_connect (monitor->proxy, "drive-disconnected", G_CALLBACK (drive_disconnected), monitor);
  g_signal_connect (monitor->proxy, "drive-eject-button", G_CALLBACK (drive_eject_button), monitor);
  g_signal_connect (monitor->proxy, "drive-stop-button", G_CALLBACK (drive_stop_button), monitor);
  g_signal_connect (monitor->proxy, "mount-added", G_CALLBACK (mount_added), monitor);
  g_signal_connect (monitor->proxy, "mount-changed", G_CALLBACK (mount_changed), monitor);
  g_signal_connect (monitor->proxy, "mount-op-aborted", G_CALLBACK (mount_op_aborted), monitor);
  g_signal_connect (monitor->proxy, "mount-op-ask-password", G_CALLBACK (mount_op_ask_password), monitor);
  g_signal_connect (monitor->proxy, "mount-op-ask-question", G_CALLBACK (mount_op_ask_question), monitor);
  g_signal_connect (monitor->proxy, "mount-op-show-processes", G_CALLBACK (mount_op_show_processes), monitor);
  g_signal_connect (monitor->proxy, "mount-op-show-unmount-progress", G_CALLBACK (mount_op_show_unmount_progress), monitor);
  g_signal_connect (monitor->proxy, "mount-pre-unmount", G_CALLBACK (mount_pre_unmount), monitor);
  g_signal_connect (monitor->proxy, "mount-removed", G_CALLBACK (mount_removed), monitor);
  g_signal_connect (monitor->proxy, "volume-added", G_CALLBACK (volume_added), monitor);
  g_signal_connect (monitor->proxy, "volume-changed", G_CALLBACK (volume_changed), monitor);
  g_signal_connect (monitor->proxy, "volume-removed", G_CALLBACK (volume_removed), monitor);

  /* listen to when the owner of the service appears/disappears */
  g_signal_connect (monitor->proxy, "notify::g-name-owner", G_CALLBACK (name_owner_changed), monitor);
  /* initially seed drives/volumes/mounts if we have an owner */
  name_owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (monitor->proxy));
  if (name_owner != NULL)
    {
      seed_monitor (monitor);
      g_free (name_owner);
    }

  g_hash_table_insert (the_volume_monitors, (gpointer) type, object);

 out:
  /* Take an extra reference to make the instance live forever - see also
   * the dispose() and finalize() vfuncs
   */
  g_object_ref (object);

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

static gboolean
dispose_in_idle_do (GObject *object)
{
  g_object_run_dispose (object);
  g_object_unref (object);

  return FALSE;
}

static void
dispose_in_idle (gpointer object)
{
  g_idle_add ((GSourceFunc) dispose_in_idle_do, g_object_ref (object));
}

/* Typically called from g_proxy_volume_monitor_constructor() with proxy_vm lock being held */
static void
g_proxy_volume_monitor_init (GProxyVolumeMonitor *monitor)
{
  g_proxy_volume_monitor_setup_session_bus_connection ();
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
  gboolean is_supported;
  GVfsRemoteVolumeMonitor *proxy;
  GError *error;

  is_supported = FALSE;
  error = NULL;

  proxy = gvfs_remote_volume_monitor_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                             G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS | G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                             dbus_name,
                                                             "/org/gtk/Private/RemoteVolumeMonitor",
                                                             NULL,
                                                             &error);
  if (proxy == NULL)
    {
      g_printerr ("Error creating proxy: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      goto out;
    }

  error = NULL;
  if (!gvfs_remote_volume_monitor_call_is_supported_sync (proxy,
                                                          &is_supported,
                                                          NULL,
                                                          &error))
    {
      g_printerr ("invoking IsSupported() failed for remote volume monitor with dbus name %s:: %s (%s, %d)\n",
                  dbus_name, error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      goto out;
    }
  
  if (!is_supported)
    g_warning ("remote volume monitor with dbus name %s is not supported", dbus_name);

 out:
  if (proxy != NULL)
    g_object_unref (proxy);
  return is_supported;
}

static gboolean
is_supported (GProxyVolumeMonitorClass *klass)
{
  gboolean res;

  G_LOCK (proxy_vm);
  res = g_proxy_volume_monitor_setup_session_bus_connection ();
  G_UNLOCK (proxy_vm);
  
  if (res)
    res = is_remote_monitor_supported (klass->dbus_name);

  return res;
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
  gobject_class->dispose = g_proxy_volume_monitor_dispose;

  monitor_class->get_mounts = get_mounts;
  monitor_class->get_volumes = get_volumes;
  monitor_class->get_connected_drives = get_connected_drives;
  monitor_class->get_volume_for_uuid = get_volume_for_uuid;
  monitor_class->get_mount_for_uuid = get_mount_for_uuid;

  i = klass->is_supported_nr;
  is_supported_classes[i] = klass;
  monitor_class->is_supported = is_supported_funcs[i];

  native_class->get_mount_for_mount_path = get_mount_for_mount_path;
}

/* Call with proxy_vm lock held */
static void
seed_monitor (GProxyVolumeMonitor *monitor)
{
  GVariant *Drives;
  GVariant *Volumes;
  GVariant *Mounts;
  GVariantIter iter;
  GVariant *child;
  GError *error;

  error = NULL;
  if (!gvfs_remote_volume_monitor_call_list_sync (monitor->proxy,
                                                  &Drives,
                                                  &Volumes,
                                                  &Mounts,
                                                  NULL,
                                                  &error))
    {
      g_warning ("invoking List() failed for type %s: %s (%s, %d)",
                 G_OBJECT_TYPE_NAME (monitor),
                 error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      goto fail;
    }

  /* drives */
  g_variant_iter_init (&iter, Drives);
  while ((child = g_variant_iter_next_value (&iter)))
    {
      GProxyDrive *drive;
      const char *id;
      drive = g_proxy_drive_new (monitor);
      g_proxy_drive_update (drive, child);
      id = g_proxy_drive_get_id (drive);
      g_hash_table_insert (monitor->drives, g_strdup (id), drive);
      g_variant_unref (child);
    }

  /* volumes */
  g_variant_iter_init (&iter, Volumes);
  while ((child = g_variant_iter_next_value (&iter)))
    {
      GProxyVolume *volume;
      const char *id;
      volume = g_proxy_volume_new (monitor);
      g_proxy_volume_update (volume, child);
      id = g_proxy_volume_get_id (volume);
      g_hash_table_insert (monitor->volumes, g_strdup (id), volume);
      g_variant_unref (child);
    }

  /* mounts */
  g_variant_iter_init (&iter, Mounts);
  while ((child = g_variant_iter_next_value (&iter)))
    {
      GProxyMount *mount;
      const char *id;
      mount = g_proxy_mount_new (monitor);
      g_proxy_mount_update (mount, child);
      id = g_proxy_mount_get_id (mount);
      g_hash_table_insert (monitor->mounts, g_strdup (id), mount);
      g_variant_unref (child);
    }

  g_variant_unref (Drives);
  g_variant_unref (Volumes);
  g_variant_unref (Mounts);

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
_get_identifiers (GVariantIter *iter)
{
  GHashTable *hash_table;
  char *key;
  char *value;

  hash_table = g_hash_table_new_full (g_str_hash,
                                      g_str_equal,
                                      g_free,
                                      g_free);

  while (g_variant_iter_next (iter, "{ss}", &key, &value))
    g_hash_table_insert (hash_table, key, value);
  
  return hash_table;
}

GVfsRemoteVolumeMonitor *
g_proxy_volume_monitor_get_dbus_proxy (GProxyVolumeMonitor *volume_monitor)
{
  return g_object_ref (volume_monitor->proxy);
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
static gboolean
g_proxy_volume_monitor_setup_session_bus_connection (void)
{
  /* This is so that system daemons can use gio
   * without spawning private dbus instances.
   * See bug 526454.
   */
  if (!gvfs_have_session_bus ())
    return FALSE;

  if (the_volume_monitors == NULL)
    the_volume_monitors = g_hash_table_new (g_direct_hash, g_direct_equal);

  return TRUE;
}

void
g_proxy_volume_monitor_unload_cleanup (void)
{
  G_LOCK (proxy_vm);
  if (the_volume_monitors != NULL)
    {
      g_hash_table_unref (the_volume_monitors);
      the_volume_monitors = NULL;
    }
  G_UNLOCK (proxy_vm);
}

void
g_proxy_volume_monitor_register (GIOModule *module)
{
  GList *impls, *l;
  gboolean res, got_list;

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

  impls = NULL;
  got_list = FALSE;

  G_LOCK (proxy_vm);
  res = g_proxy_volume_monitor_setup_session_bus_connection ();
  G_UNLOCK (proxy_vm);

  if (res)
    {
      GVfsDBusDaemon *proxy;
      GError *error = NULL;

      proxy = gvfs_dbus_daemon_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                       G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS | G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                       G_VFS_DBUS_DAEMON_NAME,
                                                       G_VFS_DBUS_DAEMON_PATH,
                                                       NULL,
                                                       &error);
      if (proxy != NULL)
        {
          GVariant *monitors, *child;
          GVfsMonitorImplementation *impl;
          int i;

          if (gvfs_dbus_daemon_call_list_monitor_implementations_sync (proxy,
                                                                       &monitors, NULL, &error))
            {
              got_list = TRUE;
              for (i = 0; i < g_variant_n_children (monitors); i++)
                {
                  child = g_variant_get_child_value (monitors, i);
                  impl = g_vfs_monitor_implementation_from_dbus (child);
                  impls = g_list_prepend (impls, impl);
                  g_variant_unref (child);
                }
              g_variant_unref (monitors);
            }
          else
            {
              if (!g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
                g_debug ("Error: %s\n", error->message);
              g_error_free (error);
            }

          g_clear_object (&proxy);
        }
      else
        {
          g_debug ("Error: %s\n", error->message);
          g_error_free (error);
        }
    }

  /* Fall back on the old non-dbus version for compatibility with older
     versions of the services */
  if (!got_list)
    impls = g_vfs_list_monitor_implementations ();

  for (l = impls; l != NULL; l = l->next)
    {
      GVfsMonitorImplementation *impl = l->data;

      register_volume_monitor (G_TYPE_MODULE (module),
                               impl->type_name,
                               impl->dbus_name,
                               impl->is_native,
                               impl->native_priority);
    }

  g_list_free_full (impls, (GDestroyNotify)g_vfs_monitor_implementation_free);
}
