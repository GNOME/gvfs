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
#include <locale.h>
#include <gio/gio.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>

#include "gdbusutils.h"
#include "gvfsproxyvolumemonitordaemon.h"

static GVolumeMonitor *monitor = NULL;
static DBusConnection *connection = NULL;
static GType the_volume_monitor_type;

static void monitor_try_create (void);

static char *
_g_icon_serialize (GIcon *icon)
{
  char *ret;

  g_return_val_if_fail (icon != NULL, NULL);
  g_return_val_if_fail (G_IS_ICON (icon), NULL);

  if (G_IS_FILE_ICON (icon))
    {
      GFileIcon *file_icon = G_FILE_ICON (icon);
      GFile *file;
      char *uri;
      char *escaped_uri;

      file = g_file_icon_get_file (file_icon);
      uri = g_file_get_uri (file);
      escaped_uri = g_uri_escape_string (uri, NULL, TRUE);

      ret = g_strdup_printf ("2 GFileIcon %s", escaped_uri);

      g_free (uri);
      g_free (escaped_uri);
    }
  else if (G_IS_THEMED_ICON (icon))
    {
      GThemedIcon *themed_icon = G_THEMED_ICON (icon);
      char *escaped_name;
      char **names;
      GString *s;

      g_object_get (themed_icon,
                    "names", &names,
                    NULL);

      s = g_string_new (0); 
      g_string_append_printf (s, "%d GThemedIcon", g_strv_length (names) + 1);

      if (names != NULL)
        {
          int n;
          for (n = 0; names[n] != NULL; n++)
            {
              escaped_name = g_uri_escape_string (names[n], NULL, TRUE);
              g_string_append_c (s, ' ');
              g_string_append (s, escaped_name);
              g_free (escaped_name);
            }
        }

      ret = g_string_free (s, FALSE);

      g_strfreev (names);
    }
  else if (G_IS_EMBLEMED_ICON (icon))
    {
      char *base;
      char *emblem;
      int n;

      base = _g_icon_serialize (g_emblemed_icon_get_icon (G_EMBLEMED_ICON (icon)));
      emblem = _g_icon_serialize (g_emblemed_icon_get_emblem (G_EMBLEMED_ICON (icon)));

      n = atoi (base) + atoi (emblem) + 3;
      ret = g_strdup_printf ("%d GEmblemedIcon %s %s", n, base, emblem);
      g_free (base);
      g_free (emblem);
    }
  else
    {
      ret = NULL;
      g_warning ("unknown icon type; please add support");
    }

  return ret;
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
#define DRIVE_STRUCT_TYPE "(sssbbbbasa{ss})"

static void
append_drive (GDrive *drive, DBusMessageIter *iter_array)
{
  DBusMessageIter iter_struct;
  DBusMessageIter iter_volume_array;
  DBusMessageIter iter_identifiers;
  char *id;
  char *name;
  GIcon *icon;
  char *icon_data;
  gboolean can_eject;
  gboolean can_poll_for_media;
  gboolean has_media;
  gboolean is_media_removable;
  GList *volumes, *l;
  char **identifiers;
  int n;

  dbus_message_iter_open_container (iter_array, DBUS_TYPE_STRUCT, NULL, &iter_struct);

  id = g_strdup_printf ("%p", drive);
  name = g_drive_get_name (drive);
  icon = g_drive_get_icon (drive);
  icon_data = _g_icon_serialize (icon);
  can_eject = g_drive_can_eject (drive);
  can_poll_for_media = g_drive_can_poll_for_media (drive);
  has_media = g_drive_has_media (drive);
  is_media_removable = g_drive_is_media_removable (drive);
  volumes = g_drive_get_volumes (drive);
  identifiers = g_drive_enumerate_identifiers (drive);

  if (name == NULL)
    name = g_strdup ("");

  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &id);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &name);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &icon_data);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_BOOLEAN, &can_eject);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_BOOLEAN, &can_poll_for_media);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_BOOLEAN, &has_media);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_BOOLEAN, &is_media_removable);

  dbus_message_iter_open_container (&iter_struct, DBUS_TYPE_ARRAY, "s", &iter_volume_array);
  for (l = volumes; l != NULL; l = l->next)
    {
      GVolume *volume = G_VOLUME (l->data);
      char *volume_id;
      volume_id = g_strdup_printf ("%p", volume);
      dbus_message_iter_append_basic (&iter_volume_array, DBUS_TYPE_STRING, &volume_id);
      g_free (volume_id);
    }
  dbus_message_iter_close_container (&iter_struct, &iter_volume_array);

  dbus_message_iter_open_container (&iter_struct, DBUS_TYPE_ARRAY, "{ss}", &iter_identifiers);
  for (n = 0; identifiers[n] != NULL; n++)
    {
      DBusMessageIter iter_dict_entry;
      char *id_value;
      id_value = g_drive_get_identifier (drive, identifiers[n]);
      dbus_message_iter_open_container (&iter_identifiers,
                                        DBUS_TYPE_DICT_ENTRY,
                                        NULL,
                                        &iter_dict_entry);
      dbus_message_iter_append_basic (&iter_dict_entry, DBUS_TYPE_STRING, &(identifiers[n]));
      dbus_message_iter_append_basic (&iter_dict_entry, DBUS_TYPE_STRING, &id_value);
      dbus_message_iter_close_container (&iter_identifiers, &iter_dict_entry);
      g_free (id_value);
    }
  dbus_message_iter_close_container (&iter_struct, &iter_identifiers);

  g_strfreev (identifiers);
  g_list_foreach (volumes, (GFunc) g_object_unref, NULL);
  g_list_free (volumes);
  g_free (icon_data);
  g_object_unref (icon);
  g_free (name);
  g_free (id);

  dbus_message_iter_close_container (iter_array, &iter_struct);
}

/* string               id
 * string               name
 * string               gicon_data
 * string               uuid
 * string               activation_uri
 * boolean              can-mount
 * boolean              should-automount
 * string               drive-id
 * string               mount-id
 * dict:string->string  identifiers
 */
#define VOLUME_STRUCT_TYPE "(sssssbbssa{ss})"

static void
append_volume (GVolume *volume, DBusMessageIter *iter_array)
{
  DBusMessageIter iter_struct;
  DBusMessageIter iter_identifiers;
  char *id;
  char *name;
  GIcon *icon;
  char *icon_data;
  char *uuid;
  GFile *activation_root;
  char *activation_uri;
  gboolean can_mount;
  gboolean should_automount;
  GDrive *drive;
  char *drive_id;
  GMount *mount;
  char *mount_id;
  char **identifiers;
  int n;

  dbus_message_iter_open_container (iter_array, DBUS_TYPE_STRUCT, NULL, &iter_struct);

  id = g_strdup_printf ("%p", volume);
  name = g_volume_get_name (volume);
  icon = g_volume_get_icon (volume);
  icon_data = _g_icon_serialize (icon);
  uuid = g_volume_get_uuid (volume);
  activation_root = g_volume_get_activation_root (volume);
  if (activation_root == NULL)
    activation_uri = g_strdup ("");
  else
    activation_uri = g_file_get_uri (activation_root);
  can_mount = g_volume_can_mount (volume);
  should_automount = g_volume_should_automount (volume);
  drive = g_volume_get_drive (volume);
  if (drive == NULL)
    drive_id = g_strdup ("");
  else
    drive_id = g_strdup_printf ("%p", drive);
  mount = g_volume_get_mount (volume);
  if (mount == NULL)
    mount_id = g_strdup ("");
  else
    mount_id = g_strdup_printf ("%p", mount);
  identifiers = g_volume_enumerate_identifiers (volume);

  if (name == NULL)
    name = g_strdup ("");
  if (uuid == NULL)
    uuid = g_strdup ("");

  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &id);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &name);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &icon_data);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &uuid);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &activation_uri);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_BOOLEAN, &can_mount);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_BOOLEAN, &should_automount);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &drive_id);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &mount_id);

  dbus_message_iter_open_container (&iter_struct, DBUS_TYPE_ARRAY, "{ss}", &iter_identifiers);
  for (n = 0; identifiers[n] != NULL; n++)
    {
      DBusMessageIter iter_dict_entry;
      char *id_value;
      id_value = g_volume_get_identifier (volume, identifiers[n]);
      dbus_message_iter_open_container (&iter_identifiers,
                                        DBUS_TYPE_DICT_ENTRY,
                                        NULL,
                                        &iter_dict_entry);
      dbus_message_iter_append_basic (&iter_dict_entry, DBUS_TYPE_STRING, &(identifiers[n]));
      dbus_message_iter_append_basic (&iter_dict_entry, DBUS_TYPE_STRING, &id_value);
      dbus_message_iter_close_container (&iter_identifiers, &iter_dict_entry);
      g_free (id_value);
    }
  dbus_message_iter_close_container (&iter_struct, &iter_identifiers);

  g_strfreev (identifiers);
  g_free (mount_id);
  if (mount != NULL)
    g_object_unref (mount);
  g_free (drive_id);
  if (drive != NULL)
    g_object_unref (drive);
  g_free (uuid);
  if (activation_root != NULL)
    g_object_unref (activation_root);
  g_free (activation_uri);
  g_free (icon_data);
  g_object_unref (icon);
  g_free (name);
  g_free (id);

  dbus_message_iter_close_container (iter_array, &iter_struct);
}

/* string               id
 * string               name
 * string               gicon_data
 * string               uuid
 * string               root_uri
 * boolean              can-unmount
 * string               volume-id
 * array:string         x-content-types
 */
#define MOUNT_STRUCT_TYPE "(sssssbsas)"

static void
append_mount (GMount *mount, DBusMessageIter *iter_array)
{
  DBusMessageIter iter_struct;
  DBusMessageIter iter_x_content_types_array;
  char *id;
  char *name;
  GIcon *icon;
  char *icon_data;
  char *uuid;
  GFile *root;
  char *root_uri;
  gboolean can_unmount;
  GVolume *volume;
  char *volume_id;

  dbus_message_iter_open_container (iter_array, DBUS_TYPE_STRUCT, NULL, &iter_struct);

  id = g_strdup_printf ("%p", mount);
  name = g_mount_get_name (mount);
  icon = g_mount_get_icon (mount);
  icon_data = _g_icon_serialize (icon);
  uuid = g_mount_get_uuid (mount);
  root = g_mount_get_root (mount);
  root_uri = g_file_get_uri (root);
  can_unmount = g_mount_can_unmount (mount);
  volume = g_mount_get_volume (mount);
  if (volume == NULL)
    volume_id = g_strdup ("");
  else
    volume_id = g_strdup_printf ("%p", volume);

  if (name == NULL)
    name = g_strdup ("");
  if (uuid == NULL)
    uuid = g_strdup ("");

  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &id);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &name);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &icon_data);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &uuid);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &root_uri);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_BOOLEAN, &can_unmount);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_STRING, &volume_id);

  dbus_message_iter_open_container (&iter_struct, DBUS_TYPE_ARRAY, "s", &iter_x_content_types_array);
  /* TODO: append x-content types */
  dbus_message_iter_close_container (&iter_struct, &iter_x_content_types_array);

  g_free (volume_id);
  if (volume != NULL)
    g_object_unref (volume);
  g_free (root_uri);
  g_object_unref (root);
  g_free (uuid);
  g_free (icon_data);
  g_object_unref (icon);
  g_free (name);
  g_free (id);

  dbus_message_iter_close_container (iter_array, &iter_struct);
}

static DBusHandlerResult
handle_list (DBusConnection *connection, DBusMessage *message)
{
  GList *drives;
  GList *volumes;
  GList *mounts;
  DBusMessageIter iter;
  DBusMessageIter iter_array;
  DBusMessage *reply;

  drives = g_volume_monitor_get_connected_drives (monitor);
  volumes = g_volume_monitor_get_volumes (monitor);
  mounts = g_volume_monitor_get_mounts (monitor);

  reply = dbus_message_new_method_return (message);
  dbus_message_iter_init_append (reply, &iter);

  dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY, DRIVE_STRUCT_TYPE, &iter_array);
  g_list_foreach (drives, (GFunc) append_drive, &iter_array);
  dbus_message_iter_close_container (&iter, &iter_array);

  dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY, VOLUME_STRUCT_TYPE, &iter_array);
  g_list_foreach (volumes, (GFunc) append_volume, &iter_array);
  dbus_message_iter_close_container (&iter, &iter_array);

  dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY, MOUNT_STRUCT_TYPE, &iter_array);
  g_list_foreach (mounts, (GFunc) append_mount, &iter_array);
  dbus_message_iter_close_container (&iter, &iter_array);

  g_list_foreach (drives, (GFunc) g_object_unref, NULL);
  g_list_free (drives);
  g_list_foreach (volumes, (GFunc) g_object_unref, NULL);
  g_list_free (volumes);
  g_list_foreach (mounts, (GFunc) g_object_unref, NULL);
  g_list_free (mounts);

  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);

  return DBUS_HANDLER_RESULT_HANDLED;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
mount_unmount_cb (GMount *mount, GAsyncResult *result, DBusMessage *message)
{
  GError *error;
  DBusMessage *reply;

  error = NULL;
  if (!g_mount_unmount_finish (mount, result, &error))
    {
      reply = _dbus_message_new_from_gerror (message, error);
      g_error_free (error);
    }
  else
    {
      reply = dbus_message_new_method_return (message);
    }

  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (message);
  dbus_message_unref (reply);
}

static DBusHandlerResult
handle_mount_unmount (DBusConnection *connection, DBusMessage *message)
{
  const char *id;
  dbus_uint32_t unmount_flags;
  DBusError dbus_error;
  GList *mounts, *l;
  GMount *mount;
  DBusHandlerResult ret;

  mounts = NULL;
  unmount_flags = 0;
  ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  dbus_error_init (&dbus_error);
  if (!dbus_message_get_args (message, &dbus_error,
                              DBUS_TYPE_STRING, &id,
                              DBUS_TYPE_UINT32 &unmount_flags,
                              DBUS_TYPE_INVALID))
    {
      g_warning ("Error parsing args for MountUnmount(): %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }

  ret = DBUS_HANDLER_RESULT_HANDLED;

  mount = NULL;
  mounts = g_volume_monitor_get_mounts (monitor);
  for (l = mounts; l != NULL; l = l->next)
    {
      char *mount_id;

      mount = G_MOUNT (l->data);
      mount_id = g_strdup_printf ("%p", mount);
      if (strcmp (mount_id, id) == 0)
        break;

      g_free (mount_id);
    }
  if (l == NULL)
    mount = NULL;

  if (mount == NULL)
    {
      DBusMessage *reply;
      reply = dbus_message_new_error (message,
                                      "org.gtk.Private.RemoteVolumeMonitor.NotFound",
                                      "The given mount was not found");
      dbus_connection_send (connection, reply, NULL);
      dbus_message_unref (reply);
      goto out;
    }

  g_mount_unmount (mount,
                   unmount_flags,
                   NULL,
                   (GAsyncReadyCallback) mount_unmount_cb,
                   dbus_message_ref (message));

 out:
  if (mounts != NULL)
    {
      g_list_foreach (mounts, (GFunc) g_object_unref, NULL);
      g_list_free (mounts);
    }
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
volume_mount_cb (GVolume *volume, GAsyncResult *result, DBusMessage *message)
{
  GError *error;
  DBusMessage *reply;

  error = NULL;
  if (!g_volume_mount_finish (volume, result, &error))
    {
      reply = _dbus_message_new_from_gerror (message, error);
      g_error_free (error);
    }
  else
    {
      reply = dbus_message_new_method_return (message);
    }

  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (message);
  dbus_message_unref (reply);
}

static DBusHandlerResult
handle_volume_mount (DBusConnection *connection, DBusMessage *message)
{
  const char *id;
  dbus_uint32_t mount_flags;
  dbus_bool_t use_mount_operation;
  DBusError dbus_error;
  GList *volumes, *l;
  GVolume *volume;
  DBusHandlerResult ret;
  GMountOperation *mount_operation;

  volume = NULL;
  mount_flags = 0;
  ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  dbus_error_init (&dbus_error);
  if (!dbus_message_get_args (message, &dbus_error,
                              DBUS_TYPE_STRING, &id,
                              DBUS_TYPE_UINT32 &mount_flags,
                              DBUS_TYPE_BOOLEAN, &use_mount_operation,
                              DBUS_TYPE_INVALID))
    {
      g_warning ("Error parsing args for VolumeMount(): %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }

  ret = DBUS_HANDLER_RESULT_HANDLED;

  volume = NULL;
  volumes = g_volume_monitor_get_volumes (monitor);
  for (l = volumes; l != NULL; l = l->next)
    {
      char *volume_id;

      volume = G_VOLUME (l->data);
      volume_id = g_strdup_printf ("%p", volume);
      if (strcmp (volume_id, id) == 0)
        break;

      g_free (volume_id);
    }
  if (l == NULL)
    volume = NULL;

  if (volume == NULL)
    {
      DBusMessage *reply;
      reply = dbus_message_new_error (message,
                                      "org.gtk.Private.RemoteVolumeMonitor.NotFound",
                                      "The given volume was not found");
      dbus_connection_send (connection, reply, NULL);
      dbus_message_unref (reply);
      goto out;
    }

  mount_operation = NULL;
  if (use_mount_operation)
    mount_operation = g_mount_operation_new ();

  g_volume_mount (volume,
                  mount_flags,
                  mount_operation,
                  NULL,
                  (GAsyncReadyCallback) volume_mount_cb,
                  dbus_message_ref (message));

  if (mount_operation != NULL)
    g_object_unref (mount_operation);

 out:
  if (volumes != NULL)
    {
      g_list_foreach (volumes, (GFunc) g_object_unref, NULL);
      g_list_free (volumes);
    }
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
drive_eject_cb (GDrive *drive, GAsyncResult *result, DBusMessage *message)
{
  GError *error;
  DBusMessage *reply;

  error = NULL;
  if (!g_drive_eject_finish (drive, result, &error))
    {
      reply = _dbus_message_new_from_gerror (message, error);
      g_error_free (error);
    }
  else
    {
      reply = dbus_message_new_method_return (message);
    }

  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (message);
  dbus_message_unref (reply);
}

static DBusHandlerResult
handle_drive_eject (DBusConnection *connection, DBusMessage *message)
{
  const char *id;
  dbus_uint32_t unmount_flags;
  DBusError dbus_error;
  GList *drives, *l;
  GDrive *drive;
  DBusHandlerResult ret;

  drive = NULL;
  unmount_flags = 0;
  ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  dbus_error_init (&dbus_error);
  if (!dbus_message_get_args (message, &dbus_error,
                              DBUS_TYPE_STRING, &id,
                              DBUS_TYPE_UINT32 &unmount_flags,
                              DBUS_TYPE_INVALID))
    {
      g_warning ("Error parsing args for DriveEject(): %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }

  ret = DBUS_HANDLER_RESULT_HANDLED;

  drive = NULL;
  drives = g_volume_monitor_get_connected_drives (monitor);
  for (l = drives; l != NULL; l = l->next)
    {
      char *drive_id;

      drive = G_DRIVE (l->data);
      drive_id = g_strdup_printf ("%p", drive);
      if (strcmp (drive_id, id) == 0)
        break;

      g_free (drive_id);
    }
  if (l == NULL)
    drive = NULL;

  if (drive == NULL)
    {
      DBusMessage *reply;
      reply = dbus_message_new_error (message,
                                      "org.gtk.Private.RemoteVolumeMonitor.NotFound",
                                      "The given drive was not found");
      dbus_connection_send (connection, reply, NULL);
      dbus_message_unref (reply);
      goto out;
    }

  g_drive_eject (drive,
                 unmount_flags,
                 NULL,
                 (GAsyncReadyCallback) drive_eject_cb,
                 dbus_message_ref (message));

 out:
  if (drives != NULL)
    {
      g_list_foreach (drives, (GFunc) g_object_unref, NULL);
      g_list_free (drives);
    }
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
drive_poll_for_media_cb (GDrive *drive, GAsyncResult *result, DBusMessage *message)
{
  GError *error;
  DBusMessage *reply;

  error = NULL;
  if (!g_drive_poll_for_media_finish (drive, result, &error))
    {
      reply = _dbus_message_new_from_gerror (message, error);
      g_error_free (error);
    }
  else
    {
      reply = dbus_message_new_method_return (message);
    }

  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (message);
  dbus_message_unref (reply);
}

static DBusHandlerResult
handle_drive_poll_for_media (DBusConnection *connection, DBusMessage *message)
{
  const char *id;
  DBusError dbus_error;
  GList *drives, *l;
  GDrive *drive;
  DBusHandlerResult ret;

  drive = NULL;
  ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  dbus_error_init (&dbus_error);
  if (!dbus_message_get_args (message, &dbus_error,
                              DBUS_TYPE_STRING, &id,
                              DBUS_TYPE_INVALID))
    {
      g_warning ("Error parsing args for DrivePollForMedia(): %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }

  ret = DBUS_HANDLER_RESULT_HANDLED;

  drive = NULL;
  drives = g_volume_monitor_get_connected_drives (monitor);
  for (l = drives; l != NULL; l = l->next)
    {
      char *drive_id;

      drive = G_DRIVE (l->data);
      drive_id = g_strdup_printf ("%p", drive);
      if (strcmp (drive_id, id) == 0)
        break;

      g_free (drive_id);
    }
  if (l == NULL)
    drive = NULL;

  if (drive == NULL)
    {
      DBusMessage *reply;
      reply = dbus_message_new_error (message,
                                      "org.gtk.Private.RemoteVolumeMonitor.NotFound",
                                      "The given drive was not found");
      dbus_connection_send (connection, reply, NULL);
      dbus_message_unref (reply);
      goto out;
    }

  g_drive_poll_for_media (drive,
                          NULL,
                          (GAsyncReadyCallback) drive_poll_for_media_cb,
                          dbus_message_ref (message));

 out:
  if (drives != NULL)
    {
      g_list_foreach (drives, (GFunc) g_object_unref, NULL);
      g_list_free (drives);
    }
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static DBusHandlerResult
handle_is_supported (DBusConnection *connection, DBusMessage *message)
{
  dbus_bool_t is_supported;
  DBusMessage *reply;
  DBusMessageIter iter;

  /* if monitor wasn't created on startup; try again */
  if (monitor == NULL)
    monitor_try_create ();

  is_supported = (monitor != NULL);

  reply = dbus_message_new_method_return (message);
  dbus_message_iter_init_append (reply, &iter);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &is_supported);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);

  return DBUS_HANDLER_RESULT_HANDLED;
}

/* ---------------------------------------------------------------------------------------------------- */

static DBusHandlerResult
filter_function (DBusConnection *connection, DBusMessage *message, void *user_data)
{
  DBusHandlerResult ret;

  ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "IsSupported") &&
      strcmp (dbus_message_get_path (message), "/") == 0)
    {
      ret = handle_is_supported (connection, message);
    }
  else
    {
      if (monitor != NULL)
        {
          if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "List") &&
              strcmp (dbus_message_get_path (message), "/") == 0)
            ret = handle_list (connection, message);

          else if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "MountUnmount") &&
                   strcmp (dbus_message_get_path (message), "/") == 0)
            ret = handle_mount_unmount (connection, message);

          else if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "VolumeMount") &&
                   strcmp (dbus_message_get_path (message), "/") == 0)
            ret = handle_volume_mount (connection, message);

          else if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "DriveEject") &&
                   strcmp (dbus_message_get_path (message), "/") == 0)
            ret = handle_drive_eject (connection, message);

          else if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "DrivePollForMedia") &&
                   strcmp (dbus_message_get_path (message), "/") == 0)
            ret = handle_drive_poll_for_media (connection, message);
        }
    }

  return ret;
}

typedef void (*AppendFunc) (void *object, DBusMessageIter *iter);

static void
emit_signal (DBusConnection *connection, const char *signal_name, void *object, AppendFunc func)
{
  char *id;
  DBusMessage *message;
  DBusMessageIter iter;

  id = g_strdup_printf ("%p", object);

  message = dbus_message_new_signal ("/", "org.gtk.Private.RemoteVolumeMonitor", signal_name);
  dbus_message_iter_init_append (message, &iter);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &id);

  func (object, &iter);

  dbus_connection_send (connection, message, NULL);
  dbus_message_unref (message);


  g_free (id);
}

static void
drive_changed (GVolumeMonitor *monitor, GDrive *drive, DBusConnection *connection)
{
  emit_signal (connection, "DriveChanged", drive, (AppendFunc) append_drive);
}

static void
drive_connected (GVolumeMonitor *monitor, GDrive *drive, DBusConnection *connection)
{
  emit_signal (connection, "DriveConnected", drive, (AppendFunc) append_drive);
}

static void
drive_disconnected (GVolumeMonitor *monitor, GDrive *drive, DBusConnection *connection)
{
  emit_signal (connection, "DriveDisconnected", drive, (AppendFunc) append_drive);
}

static void
drive_eject_button (GVolumeMonitor *monitor, GDrive *drive, DBusConnection *connection)
{
  g_warning ("drive eject button!");
  emit_signal (connection, "DriveEjectButton", drive, (AppendFunc) append_drive);
}

static void
volume_changed (GVolumeMonitor *monitor, GVolume *volume, DBusConnection *connection)
{
  emit_signal (connection, "VolumeChanged", volume, (AppendFunc) append_volume);
}

static void
volume_added (GVolumeMonitor *monitor, GVolume *volume, DBusConnection *connection)
{
  emit_signal (connection, "VolumeAdded", volume, (AppendFunc) append_volume);
}

static void
volume_removed (GVolumeMonitor *monitor, GVolume *volume, DBusConnection *connection)
{
  emit_signal (connection, "VolumeRemoved", volume, (AppendFunc) append_volume);
}

static void
mount_changed (GVolumeMonitor *monitor, GMount *mount, DBusConnection *connection)
{
  emit_signal (connection, "MountChanged", mount, (AppendFunc) append_mount);
}

static void
mount_added (GVolumeMonitor *monitor, GMount *mount, DBusConnection *connection)
{
  emit_signal (connection, "MountAdded", mount, (AppendFunc) append_mount);
}

static void
mount_pre_unmount (GVolumeMonitor *monitor, GMount *mount, DBusConnection *connection)
{
  emit_signal (connection, "MountPreUnmount", mount, (AppendFunc) append_mount);
}

static void
mount_removed (GVolumeMonitor *monitor, GMount *mount, DBusConnection *connection)
{
  emit_signal (connection, "MountRemoved", mount, (AppendFunc) append_mount);
}

void
g_vfs_proxy_volume_monitor_daemon_init (void)
{
  /* avoid loading the gio proxy module which will spawn ourselves
   *
   * see remote-volume-monitor-module.c
   */
  g_setenv ("GVFS_REMOTE_VOLUME_MONITOR_IGNORE", "1", TRUE);

  setlocale (LC_ALL, "");

  bindtextdomain (GETTEXT_PACKAGE, GVFS_LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  dbus_threads_init_default ();
  g_thread_init (NULL);
  g_type_init ();
}


static void
monitor_try_create (void)
{
  GVolumeMonitorClass *klass;

  monitor = NULL;
  klass = G_VOLUME_MONITOR_CLASS (g_type_class_ref (the_volume_monitor_type));
  if (klass == NULL)
    {
      g_warning ("Can't get class for type");
      goto fail;
    }

  if (klass->is_supported != NULL)
    {
      if (! (klass->is_supported ()))
        {
          g_warning ("monitor says it's not supported");
          goto fail;
        }
    }

  monitor = G_VOLUME_MONITOR (g_object_new (the_volume_monitor_type, NULL));
  if (monitor == NULL)
    {
      g_warning ("Cannot instantiate volume monitor");
      goto fail;
    }

 fail:
  if (klass != NULL)
    g_type_class_unref (klass);
}

int
g_vfs_proxy_volume_monitor_daemon_main (int argc,
                                        char *argv[],
                                        const char *dbus_name,
                                        GType volume_monitor_type)
{
  int rc;
  int ret;
  GMainLoop *loop;
  DBusError dbus_error;

  ret = 1;

  loop = g_main_loop_new (NULL, FALSE);

  /* need to start up regardless of whether we can instantiate a
   * volume monitor; this is because the proxy will need to be able to
   * call IsSupported() on our D-Bus interface.
   */

  the_volume_monitor_type = volume_monitor_type;

  /* try and create the monitor */
  monitor_try_create ();

  dbus_error_init (&dbus_error);
  connection = dbus_bus_get (DBUS_BUS_SESSION, &dbus_error);
  if (dbus_error_is_set (&dbus_error))
    {
      g_warning ("Cannot connect to session bus: %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }

  _g_dbus_connection_integrate_with_main (connection);

  rc = dbus_bus_request_name (connection, dbus_name, 0, &dbus_error);
  if (dbus_error_is_set (&dbus_error))
    {
      g_warning ("dbus_bus_request_name failed: %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }
  if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
    {
      g_warning ("Cannot become primary owner");
      goto out;
    }

  if (!dbus_connection_add_filter (connection, filter_function, NULL, NULL))
    {
      g_warning ("Cannot add filter function");
      goto out;
    }

  if (monitor != NULL)
    {
      g_signal_connect (monitor, "drive-changed", (GCallback) drive_changed, connection);
      g_signal_connect (monitor, "drive-connected", (GCallback) drive_connected, connection);
      g_signal_connect (monitor, "drive-disconnected", (GCallback) drive_disconnected, connection);
      g_signal_connect (monitor, "drive-eject-button", (GCallback) drive_eject_button, connection);

      g_signal_connect (monitor, "volume-changed", (GCallback) volume_changed, connection);
      g_signal_connect (monitor, "volume-added", (GCallback) volume_added, connection);
      g_signal_connect (monitor, "volume-removed", (GCallback) volume_removed, connection);

      g_signal_connect (monitor, "mount-changed", (GCallback) mount_changed, connection);
      g_signal_connect (monitor, "mount-added", (GCallback) mount_added, connection);
      g_signal_connect (monitor, "mount-pre-unmount", (GCallback) mount_pre_unmount, connection);
      g_signal_connect (monitor, "mount-removed", (GCallback) mount_removed, connection);
    }

  g_main_loop_run (loop);
  g_main_loop_unref (loop);

  ret = 0;

out:
  return ret;
}
