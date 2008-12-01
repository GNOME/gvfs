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

#include <config.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n-lib.h>

#include <gdbusutils.h>

#include "gproxyvolumemonitor.h"
#include "gproxydrive.h"
#include "gproxyvolume.h"

/* Protects all fields of GProxyDrive that can change */
G_LOCK_DEFINE_STATIC(proxy_drive);

struct _GProxyDrive {
  GObject parent;

  GProxyVolumeMonitor  *volume_monitor;

  char *id;
  char *name;
  GIcon *icon;
  char **volume_ids;
  gboolean can_eject;
  gboolean can_poll_for_media;
  gboolean is_media_check_automatic;
  gboolean has_media;
  gboolean is_media_removable;

  GHashTable *identifiers;
};

static void g_proxy_drive_drive_iface_init (GDriveIface *iface);

#define _G_IMPLEMENT_INTERFACE_DYNAMIC(TYPE_IFACE, iface_init)       { \
  const GInterfaceInfo g_implement_interface_info = { \
    (GInterfaceInitFunc) iface_init, NULL, NULL \
  }; \
  g_type_module_add_interface (type_module, g_define_type_id, TYPE_IFACE, &g_implement_interface_info); \
}

G_DEFINE_DYNAMIC_TYPE_EXTENDED (GProxyDrive, g_proxy_drive, G_TYPE_OBJECT, 0,
                                _G_IMPLEMENT_INTERFACE_DYNAMIC (G_TYPE_DRIVE,
                                                                g_proxy_drive_drive_iface_init))

static void
g_proxy_drive_finalize (GObject *object)
{
  GProxyDrive *drive;

  drive = G_PROXY_DRIVE (object);

  if (drive->volume_monitor != NULL)
    g_object_unref (drive->volume_monitor);
  g_free (drive->id);
  g_free (drive->name);
  if (drive->icon != NULL)
    g_object_unref (drive->icon);
  g_strfreev (drive->volume_ids);
  if (drive->identifiers != NULL)
    g_hash_table_unref (drive->identifiers);

  if (G_OBJECT_CLASS (g_proxy_drive_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_proxy_drive_parent_class)->finalize) (object);
}

static void
g_proxy_drive_class_init (GProxyDriveClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_proxy_drive_finalize;
}

static void
g_proxy_drive_class_finalize (GProxyDriveClass *klass)
{
}

static void
g_proxy_drive_init (GProxyDrive *proxy_drive)
{
}

GProxyDrive *
g_proxy_drive_new (GProxyVolumeMonitor *volume_monitor)
{
  GProxyDrive *drive;
  drive = g_object_new (G_TYPE_PROXY_DRIVE, NULL);
  drive->volume_monitor = g_object_ref (volume_monitor);
  g_object_set_data (G_OBJECT (drive),
                     "g-proxy-drive-volume-monitor-name",
                     (gpointer) g_type_name (G_TYPE_FROM_INSTANCE (volume_monitor)));
  return drive;
}

/* string               id
 * string               name
 * string               gicon_data
 * boolean              can-eject
 * boolean              can-poll-for-media
 * boolean              has-media
 * boolean              is-media-removable
 * array:string         volume-ids
 * dict:string->string  identifiers
 */
#define DRIVE_STRUCT_TYPE "(sssbbbasa{ss})"

void
g_proxy_drive_update (GProxyDrive         *drive,
                      DBusMessageIter     *iter)
{
  DBusMessageIter iter_struct;
  DBusMessageIter iter_volume_ids_iter;
  const char *id;
  const char *name;
  const char *gicon_data;
  dbus_bool_t can_eject;
  dbus_bool_t can_poll_for_media;
  dbus_bool_t has_media;
  dbus_bool_t is_media_removable;
  GPtrArray *volume_ids;
  GHashTable *identifiers;

  dbus_message_iter_recurse (iter, &iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &id);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &name);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &gicon_data);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &can_eject);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &can_poll_for_media);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &has_media);
  dbus_message_iter_next (&iter_struct);
  dbus_message_iter_get_basic (&iter_struct, &is_media_removable);
  dbus_message_iter_next (&iter_struct);

  volume_ids = g_ptr_array_new ();
  dbus_message_iter_recurse (&iter_struct, &iter_volume_ids_iter);
  while (dbus_message_iter_get_arg_type (&iter_volume_ids_iter) != DBUS_TYPE_INVALID)
    {
      const char *volume_id;
      dbus_message_iter_get_basic (&iter_volume_ids_iter, &volume_id);
      dbus_message_iter_next (&iter_volume_ids_iter);
      g_ptr_array_add (volume_ids, (gpointer) volume_id);
    }
  g_ptr_array_add (volume_ids, NULL);
  dbus_message_iter_next (&iter_struct);

  identifiers = _get_identifiers (&iter_struct);
  dbus_message_iter_next (&iter_struct);

  if (drive->id != NULL && strcmp (drive->id, id) != 0)
    {
      g_warning ("id mismatch during update of drive");
      goto out;
    }

  if (strlen (name) == 0)
    name = NULL;

  /* out with the old */
  g_free (drive->id);
  g_free (drive->name);
  if (drive->icon != NULL)
    g_object_unref (drive->icon);
  g_strfreev (drive->volume_ids);
  if (drive->identifiers != NULL)
    g_hash_table_unref (drive->identifiers);

  /* in with the new */
  drive->id = g_strdup (id);
  drive->name = g_strdup (name);
  if (*gicon_data == 0)
    drive->icon = NULL;
  else
    drive->icon = g_icon_new_for_string (gicon_data, NULL);

  drive->can_eject = can_eject;
  drive->can_poll_for_media = can_poll_for_media;
  drive->has_media = has_media;
  drive->is_media_removable = is_media_removable;
  drive->identifiers = identifiers != NULL ? g_hash_table_ref (identifiers) : NULL;
  drive->volume_ids = g_strdupv ((char **) volume_ids->pdata);

 out:
  g_ptr_array_free (volume_ids, TRUE);
  g_hash_table_unref (identifiers);
}

static GIcon *
g_proxy_drive_get_icon (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  GIcon *icon;

  G_LOCK (proxy_drive);
  icon = proxy_drive->icon != NULL ? g_object_ref (proxy_drive->icon) : NULL;
  G_UNLOCK (proxy_drive);

  return icon;
}

static char *
g_proxy_drive_get_name (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  char *name;

  G_LOCK (proxy_drive);
  name = g_strdup (proxy_drive->name);
  G_UNLOCK (proxy_drive);

  return name;
}

static GList *
g_proxy_drive_get_volumes (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  GList *l;

  l = NULL;

  G_LOCK (proxy_drive);
  if (proxy_drive->volume_monitor != NULL && proxy_drive->volume_ids != NULL)
    {
      int n;

      for (n = 0; proxy_drive->volume_ids[n] != NULL; n++)
        {
          GProxyVolume *volume;
          volume = g_proxy_volume_monitor_get_volume_for_id (proxy_drive->volume_monitor, proxy_drive->volume_ids[n]);
          if (volume != NULL)
            l = g_list_append (l, volume);
        }
    }
  G_UNLOCK (proxy_drive);

  return l;
}

static gboolean
g_proxy_drive_has_volumes (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = (proxy_drive->volume_ids != NULL && g_strv_length (proxy_drive->volume_ids) > 0);
  G_UNLOCK (proxy_drive);

  return res;
}

static gboolean
g_proxy_drive_is_media_removable (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = proxy_drive->is_media_removable;
  G_UNLOCK (proxy_drive);

  return res;
}

static gboolean
g_proxy_drive_has_media (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = proxy_drive->has_media;
  G_UNLOCK (proxy_drive);

  return res;
}

static gboolean
g_proxy_drive_is_media_check_automatic (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = proxy_drive->is_media_check_automatic;
  G_UNLOCK (proxy_drive);

  return res;
}

static gboolean
g_proxy_drive_can_eject (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = proxy_drive->can_eject;
  G_UNLOCK (proxy_drive);

  return res;
}

static gboolean
g_proxy_drive_can_poll_for_media (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  gboolean res;

  G_LOCK (proxy_drive);
  res = proxy_drive->can_poll_for_media;
  G_UNLOCK (proxy_drive);

  return res;
}

static char *
g_proxy_drive_get_identifier (GDrive              *drive,
                              const char          *kind)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  char *res;

  G_LOCK (proxy_drive);
  if (proxy_drive->identifiers != NULL)
    res = g_strdup (g_hash_table_lookup (proxy_drive->identifiers, kind));
  else
    res = NULL;
  G_UNLOCK (proxy_drive);

  return res;
}

static void
add_identifier_key (const char *key, const char *value, GPtrArray *res)
{
  g_ptr_array_add (res, g_strdup (key));
}

static char **
g_proxy_drive_enumerate_identifiers (GDrive *drive)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  GPtrArray *res;

  res = g_ptr_array_new ();

  G_LOCK (proxy_drive);
  if (proxy_drive->identifiers != NULL)
    g_hash_table_foreach (proxy_drive->identifiers, (GHFunc) add_identifier_key, res);
  G_UNLOCK (proxy_drive);

  /* Null-terminate */
  g_ptr_array_add (res, NULL);

  return (char **) g_ptr_array_free (res, FALSE);
}

const char *
g_proxy_drive_get_id (GProxyDrive *drive)
{
  return drive->id;
}

typedef struct {
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
} DBusOp;

static void
eject_cb (DBusMessage *reply,
          GError *error,
          DBusOp *data)
{
  GSimpleAsyncResult *simple;
  if (error != NULL)
    simple = g_simple_async_result_new_from_error (data->object,
                                                   data->callback,
                                                   data->user_data,
                                                   error);
  else
    simple = g_simple_async_result_new (data->object,
                                        data->callback,
                                        data->user_data,
                                        NULL);
  g_simple_async_result_complete (simple);
  g_object_unref (simple);

  g_object_unref (data->object);
  g_free (data);
}

static void
g_proxy_drive_eject (GDrive              *drive,
                     GMountUnmountFlags   flags,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  DBusConnection *connection;
  const char *name;
  DBusMessage *message;
  DBusOp *data;
  dbus_uint32_t _flags = flags;

  G_LOCK (proxy_drive);

  data = g_new0 (DBusOp, 1);
  data->object = g_object_ref (drive);
  data->callback = callback;
  data->user_data = user_data;
  data->cancellable = cancellable;

  connection = g_proxy_volume_monitor_get_dbus_connection (proxy_drive->volume_monitor);
  name = g_proxy_volume_monitor_get_dbus_name (proxy_drive->volume_monitor);

  message = dbus_message_new_method_call (name,
                                          "/",
                                          "org.gtk.Private.RemoteVolumeMonitor",
                                          "DriveEject");
  dbus_message_append_args (message,
                            DBUS_TYPE_STRING,
                            &(proxy_drive->id),
                            DBUS_TYPE_UINT32,
                            &_flags,
                            DBUS_TYPE_INVALID);
  G_UNLOCK (proxy_drive);

  _g_dbus_connection_call_async (connection,
                                 message,
                                 -1,
                                 (GAsyncDBusCallback) eject_cb,
                                 data);
  dbus_connection_unref (connection);
  dbus_message_unref (message);
}

static gboolean
g_proxy_drive_eject_finish (GDrive        *drive,
                            GAsyncResult  *result,
                            GError       **error)
{
  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error))
    return FALSE;
  return TRUE;
}

static void
poll_for_media_cb (DBusMessage *reply,
                   GError *error,
                   DBusOp *data)
{
  GSimpleAsyncResult *simple;
  if (error != NULL)
    simple = g_simple_async_result_new_from_error (data->object,
                                                   data->callback,
                                                   data->user_data,
                                                   error);
  else
    simple = g_simple_async_result_new (data->object,
                                        data->callback,
                                        data->user_data,
                                        NULL);
  g_simple_async_result_complete (simple);
  g_object_unref (simple);

  g_object_unref (data->object);
  g_free (data);
}

static void
g_proxy_drive_poll_for_media (GDrive              *drive,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  GProxyDrive *proxy_drive = G_PROXY_DRIVE (drive);
  DBusConnection *connection;
  const char *name;
  DBusMessage *message;
  DBusOp *data;

  G_LOCK (proxy_drive);

  data = g_new0 (DBusOp, 1);
  data->object = g_object_ref (drive);
  data->callback = callback;
  data->user_data = user_data;
  data->cancellable = cancellable;

  connection = g_proxy_volume_monitor_get_dbus_connection (proxy_drive->volume_monitor);
  name = g_proxy_volume_monitor_get_dbus_name (proxy_drive->volume_monitor);

  message = dbus_message_new_method_call (name,
                                          "/",
                                          "org.gtk.Private.RemoteVolumeMonitor",
                                          "DrivePollForMedia");
  dbus_message_append_args (message,
                            DBUS_TYPE_STRING,
                            &(proxy_drive->id),
                            DBUS_TYPE_INVALID);
  G_UNLOCK (proxy_drive);

  _g_dbus_connection_call_async (connection,
                                 message,
                                 -1,
                                 (GAsyncDBusCallback) poll_for_media_cb,
                                 data);
  dbus_connection_unref (connection);
  dbus_message_unref (message);
}

static gboolean
g_proxy_drive_poll_for_media_finish (GDrive        *drive,
                                     GAsyncResult  *result,
                                     GError       **error)
{
  if (g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error))
    return FALSE;
  return TRUE;
}


static void
g_proxy_drive_drive_iface_init (GDriveIface *iface)
{
  iface->get_name = g_proxy_drive_get_name;
  iface->get_icon = g_proxy_drive_get_icon;
  iface->has_volumes = g_proxy_drive_has_volumes;
  iface->get_volumes = g_proxy_drive_get_volumes;
  iface->is_media_removable = g_proxy_drive_is_media_removable;
  iface->has_media = g_proxy_drive_has_media;
  iface->is_media_check_automatic = g_proxy_drive_is_media_check_automatic;
  iface->can_eject = g_proxy_drive_can_eject;
  iface->can_poll_for_media = g_proxy_drive_can_poll_for_media;
  iface->eject = g_proxy_drive_eject;
  iface->eject_finish = g_proxy_drive_eject_finish;
  iface->poll_for_media = g_proxy_drive_poll_for_media;
  iface->poll_for_media_finish = g_proxy_drive_poll_for_media_finish;
  iface->get_identifier = g_proxy_drive_get_identifier;
  iface->enumerate_identifiers = g_proxy_drive_enumerate_identifiers;
}

void
g_proxy_drive_register (GIOModule *module)
{
  g_proxy_drive_register_type (G_TYPE_MODULE (module));
}
