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
#include <stdlib.h>
#include <glib/gprintf.h>

#include "gvfsdbusutils.h"
#include "gvfsproxyvolumemonitordaemon.h"

/* ---------------------------------------------------------------------------------------------------- */

static GVolumeMonitor *monitor = NULL;
static DBusConnection *connection = NULL;
static GType the_volume_monitor_type;
static const char *the_dbus_name = NULL;
static GList *outstanding_ops = NULL;
static GList *outstanding_mount_op_objects = NULL;
static GHashTable *unique_names_being_watched = NULL;

/* #define DEBUG_ENABLED */

#ifdef DEBUG_ENABLED
static void
print_debug (const gchar *format, ...)
{
  va_list      var_args;

  va_start (var_args, format);

  g_print ("### debug: ");
  g_vprintf (format, var_args);
  g_print ("\n");

  va_end (var_args);
}
#else
static void
print_debug (const gchar *format, ...)
{
}
#endif



/* ---------------------------------------------------------------------------------------------------- */

GType g_proxy_mount_operation_get_type (void) G_GNUC_CONST;

typedef struct
{
  GMountOperation parent_instance;
} GProxyMountOperation;

typedef struct
{
  GMountOperationClass parent_class;
} GProxyMountOperationClass;


static GMountOperation *
g_proxy_mount_operation_new (void)
{
  return G_MOUNT_OPERATION (g_object_new (g_proxy_mount_operation_get_type(), NULL));
}

G_DEFINE_TYPE (GProxyMountOperation, g_proxy_mount_operation, G_TYPE_MOUNT_OPERATION)

static void
g_proxy_mount_operation_init (GProxyMountOperation *mount_operation)
{
}

static void
g_proxy_mount_operation_ask_password (GMountOperation *op,
                                      const char      *message,
                                      const char      *default_user,
                                      const char      *default_domain,
                                      GAskPasswordFlags flags)
{
  /* do nothing */
}

static void
g_proxy_mount_operation_ask_question (GMountOperation *op,
                                      const char      *message,
                                      const char      *choices[])
{
  /* do nothing */
}

static void
g_proxy_mount_operation_show_processes (GMountOperation *op,
                                        const gchar          *message,
                                        GArray               *processes,
                                        const gchar          *choices[])
{
  /* do nothing */
}

static void
g_proxy_mount_operation_class_init (GProxyMountOperationClass *klass)
{
  GMountOperationClass *mount_op_class;

  mount_op_class = G_MOUNT_OPERATION_CLASS (klass);

  mount_op_class->ask_password   = g_proxy_mount_operation_ask_password;
  mount_op_class->ask_question   = g_proxy_mount_operation_ask_question;
  mount_op_class->show_processes = g_proxy_mount_operation_show_processes;
}

static void
ask_password_cb (GMountOperation  *mount_operation,
                 const gchar      *message_to_show,
                 const gchar      *default_user,
                 const gchar      *default_domain,
                 GAskPasswordFlags flags,
                 gpointer          user_data)
{
  DBusMessage *message;
  DBusMessageIter iter;
  const gchar *mount_op_id;
  const gchar *mount_op_owner;

  print_debug ("in ask_password_cb %s", message_to_show);

  mount_op_id = g_object_get_data (G_OBJECT (mount_operation), "mount_op_id");
  mount_op_owner = g_object_get_data (G_OBJECT (mount_operation), "mount_op_owner");

  message = dbus_message_new_method_call (mount_op_owner,
                                          "/org/gtk/Private/RemoteVolumeMonitor",
                                          "org.gtk.Private.RemoteVolumeMonitor",
                                          "MountOpAskPassword");
  dbus_message_iter_init_append (message, &iter);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &the_dbus_name);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &mount_op_id);

  if (message_to_show == NULL)
    message_to_show = "";

  if (default_user == NULL)
    default_user = "";

  if (default_domain == NULL)
    default_domain = "";

  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &message_to_show);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &default_user);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &default_domain);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_INT32, &flags);

  dbus_connection_send (connection, message, NULL);
  dbus_message_unref (message);
}

static void
ask_question_cb (GMountOperation  *mount_operation,
                 const gchar      *message_to_show,
                 gchar           **choices,
                 gpointer          user_data)
{
  DBusMessage *message;
  DBusMessageIter iter;
  DBusMessageIter iter_string_array;
  const gchar *mount_op_id;
  const gchar *mount_op_owner;
  guint n;

  print_debug ("in ask_question_cb %s", message_to_show);

  mount_op_id = g_object_get_data (G_OBJECT (mount_operation), "mount_op_id");
  mount_op_owner = g_object_get_data (G_OBJECT (mount_operation), "mount_op_owner");

  message = dbus_message_new_method_call (mount_op_owner,
                                          "/org/gtk/Private/RemoteVolumeMonitor",
                                          "org.gtk.Private.RemoteVolumeMonitor",
                                          "MountOpAskQuestion");
  dbus_message_iter_init_append (message, &iter);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &the_dbus_name);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &mount_op_id);

  if (message_to_show == NULL)
    message_to_show = "";

  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &message_to_show);

  dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &iter_string_array);
  for (n = 0; choices != NULL && choices[n] != NULL; n++)
    dbus_message_iter_append_basic (&iter_string_array, DBUS_TYPE_STRING, &(choices[n]));
  dbus_message_iter_close_container (&iter, &iter_string_array);

  dbus_connection_send (connection, message, NULL);
  dbus_message_unref (message);
}

static void
show_processes_cb (GMountOperation  *mount_operation,
                   const gchar      *message_to_show,
                   GArray           *processes,
                   gchar           **choices,
                   gpointer          user_data)
{
  DBusMessage *message;
  DBusMessageIter iter;
  DBusMessageIter iter_string_array;
  const gchar *mount_op_id;
  const gchar *mount_op_owner;
  guint n;

  print_debug ("in show_processes_cb %s", message_to_show);

  mount_op_id = g_object_get_data (G_OBJECT (mount_operation), "mount_op_id");
  mount_op_owner = g_object_get_data (G_OBJECT (mount_operation), "mount_op_owner");

  message = dbus_message_new_method_call (mount_op_owner,
                                          "/org/gtk/Private/RemoteVolumeMonitor",
                                          "org.gtk.Private.RemoteVolumeMonitor",
                                          "MountOpShowProcesses");
  dbus_message_iter_init_append (message, &iter);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &the_dbus_name);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &mount_op_id);

  if (message_to_show == NULL)
    message_to_show = "";

  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &message_to_show);

  dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_INT32_AS_STRING, &iter_string_array);
  for (n = 0; processes != NULL && n < processes->len; n++)
    {
      GPid pid;
      pid = g_array_index (processes, GPid, n);
      dbus_message_iter_append_basic (&iter_string_array, DBUS_TYPE_INT32, &pid);
    }
  dbus_message_iter_close_container (&iter, &iter_string_array);

  dbus_message_iter_open_container (&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &iter_string_array);
  for (n = 0; choices != NULL && choices[n] != NULL; n++)
    dbus_message_iter_append_basic (&iter_string_array, DBUS_TYPE_STRING, &(choices[n]));
  dbus_message_iter_close_container (&iter, &iter_string_array);

  dbus_connection_send (connection, message, NULL);
  dbus_message_unref (message);
}

static void
aborted_cb (GMountOperation  *mount_operation,
            gpointer          user_data)
{
  DBusMessage *message;
  DBusMessageIter iter;
  const gchar *mount_op_id;
  const gchar *mount_op_owner;

  print_debug ("in aborted_cb");

  mount_op_id = g_object_get_data (G_OBJECT (mount_operation), "mount_op_id");
  mount_op_owner = g_object_get_data (G_OBJECT (mount_operation), "mount_op_owner");

  message = dbus_message_new_method_call (mount_op_owner,
                                          "/org/gtk/Private/RemoteVolumeMonitor",
                                          "org.gtk.Private.RemoteVolumeMonitor",
                                          "MountOpAborted");
  dbus_message_iter_init_append (message, &iter);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &the_dbus_name);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &mount_op_id);

  dbus_connection_send (connection, message, NULL);
  dbus_message_unref (message);
}

static void
mount_op_destroyed_cb (gpointer user_data,
                       GObject *where_the_mount_op_was)
{
  outstanding_mount_op_objects = g_list_remove (outstanding_mount_op_objects, where_the_mount_op_was);
}

static GMountOperation *
wrap_mount_op (const gchar *mount_op_id,
               const gchar *mount_op_owner)
{
  GMountOperation *op;

  op = g_proxy_mount_operation_new ();
  //op = g_mount_operation_new ();
  g_signal_connect (op, "ask-password", G_CALLBACK (ask_password_cb), NULL);
  g_signal_connect (op, "ask-question", G_CALLBACK (ask_question_cb), NULL);
  g_signal_connect (op, "show-processes", G_CALLBACK (show_processes_cb), NULL);
  g_signal_connect (op, "aborted", G_CALLBACK (aborted_cb), NULL);
  g_object_set_data_full (G_OBJECT (op), "mount_op_id", g_strdup (mount_op_id), g_free);
  g_object_set_data_full (G_OBJECT (op), "mount_op_owner", g_strdup (mount_op_owner), g_free);

  outstanding_mount_op_objects = g_list_prepend (outstanding_mount_op_objects, op);
  g_object_weak_ref (G_OBJECT (op),
                     mount_op_destroyed_cb,
                     NULL);

  return op;
}

/* ---------------------------------------------------------------------------------------------------- */


static void
cancellable_destroyed_cb (gpointer user_data,
                          GObject *where_the_cancellable_was)
{
  outstanding_ops = g_list_remove (outstanding_ops, where_the_cancellable_was);
}

static void
remove_name_owned_changed_for_unique_name (const gchar *unique_name)
{
  const gchar *match_rule;
  DBusError dbus_error;

  match_rule = g_hash_table_lookup (unique_names_being_watched, unique_name);
  if (match_rule == NULL)
    {
      g_warning ("Was asked to remove match rule for unique_name %s but we don't have one", unique_name);
      goto out;
    }

  dbus_error_init (&dbus_error);
  dbus_bus_remove_match (connection,
                         match_rule,
                         &dbus_error);
  if (dbus_error_is_set (&dbus_error)) {
    g_warning ("cannot remove match rule '%s': %s: %s", match_rule, dbus_error.name, dbus_error.message);
    dbus_error_free (&dbus_error);
  }

  g_hash_table_remove (unique_names_being_watched, unique_name);

 out:
  ;
}

static void
ensure_name_owner_changed_for_unique_name (const gchar *unique_name)
{
  gchar *match_rule;
  DBusError dbus_error;

  if (g_hash_table_lookup (unique_names_being_watched, unique_name) != NULL)
    goto out;

  match_rule = g_strdup_printf ("type='signal',"
                                "interface='org.freedesktop.DBus',"
                                "member='NameOwnerChanged',"
                                "arg0='%s'",
                                unique_name);

  dbus_error_init (&dbus_error);
  dbus_bus_add_match (connection,
                      match_rule,
                      &dbus_error);
  if (dbus_error_is_set (&dbus_error))
    {
      g_warning ("cannot add match rule '%s': %s: %s", match_rule, dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      g_free (match_rule);
      goto out;
    }

  g_hash_table_insert (unique_names_being_watched, g_strdup (unique_name), match_rule);

 out:
  ;
}

static void monitor_try_create (void);

/* string               id
 * string               name
 * string               gicon_data
 * boolean              can-eject
 * boolean              can-poll-for-media
 * boolean              has-media
 * boolean              is-media-removable
 * boolean              is-media-check-automatic
 * boolean              can-start
 * boolean              can-start-degraded
 * boolean              can-stop
 * uint32               start-stop-type
 * array:string         volume-ids
 * dict:string->string  identifiers
 */
#define DRIVE_STRUCT_TYPE "(sssbbbbbbbbuasa{ss})"

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
  gboolean is_media_check_automatic;
  gboolean can_start;
  gboolean can_start_degraded;
  gboolean can_stop;
  GDriveStartStopType start_stop_type;
  GList *volumes, *l;
  char **identifiers;
  int n;

  dbus_message_iter_open_container (iter_array, DBUS_TYPE_STRUCT, NULL, &iter_struct);

  id = g_strdup_printf ("%p", drive);
  name = g_drive_get_name (drive);
  icon = g_drive_get_icon (drive);
  if (icon)
    icon_data = g_icon_to_string (icon);
  else
    icon_data = g_strdup ("");
  can_eject = g_drive_can_eject (drive);
  can_poll_for_media = g_drive_can_poll_for_media (drive);
  has_media = g_drive_has_media (drive);
  is_media_removable = g_drive_is_media_removable (drive);
  is_media_check_automatic = g_drive_is_media_check_automatic (drive);
  can_start = g_drive_can_start (drive);
  can_start_degraded = g_drive_can_start_degraded (drive);
  can_stop = g_drive_can_stop (drive);
  start_stop_type = g_drive_get_start_stop_type (drive);
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
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_BOOLEAN, &is_media_check_automatic);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_BOOLEAN, &can_start);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_BOOLEAN, &can_start_degraded);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_BOOLEAN, &can_stop);
  dbus_message_iter_append_basic (&iter_struct, DBUS_TYPE_UINT32, &start_stop_type);

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
  for (n = 0; identifiers != NULL && identifiers[n] != NULL; n++)
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
  if (icon)
    icon_data = g_icon_to_string (icon);
  else
    icon_data = g_strdup ("");
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
  for (n = 0; identifiers != NULL && identifiers[n] != NULL; n++)
    {
      DBusMessageIter iter_dict_entry;
      char *id_value;
      id_value = g_volume_get_identifier (volume, identifiers[n]);
      if (id_value == NULL)
        continue;
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
  char **x_content_types;
  int n;

  dbus_message_iter_open_container (iter_array, DBUS_TYPE_STRUCT, NULL, &iter_struct);

  id = g_strdup_printf ("%p", mount);
  name = g_mount_get_name (mount);
  icon = g_mount_get_icon (mount);
  if (icon)
    icon_data = g_icon_to_string (icon);
  else
    icon_data = g_strdup ("");
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
  x_content_types = (char **) g_object_get_data (G_OBJECT (mount), "x-content-types");
  if (x_content_types != NULL)
    {
      for (n = 0; x_content_types[n] != NULL; n++)
        dbus_message_iter_append_basic (&iter_x_content_types_array, DBUS_TYPE_STRING, &(x_content_types[n]));
    }
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

  print_debug ("in handle_list");

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

  print_debug ("in mount_unmount_cb");

  g_object_set_data (G_OBJECT (mount), "cancellable", NULL);
  g_object_set_data (G_OBJECT (mount), "mount_operation", NULL);

  error = NULL;
  if (!g_mount_unmount_with_operation_finish (mount, result, &error))
    {
      print_debug ("  error: %s", error->message);
      reply = _dbus_message_new_from_gerror (message, error);
      g_error_free (error);
    }
  else
    {
      print_debug (" success");
      reply = dbus_message_new_method_return (message);
    }

  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (message);
  dbus_message_unref (reply);

  g_object_unref (mount);
}

static DBusHandlerResult
handle_mount_unmount (DBusConnection *connection, DBusMessage *message)
{
  const char *id;
  const char *cancellation_id;
  const char *sender;
  const char *mount_op_id;
  GCancellable *cancellable;
  GMountOperation *mount_operation;
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
                              DBUS_TYPE_STRING, &cancellation_id,
                              DBUS_TYPE_UINT32, &unmount_flags,
                              DBUS_TYPE_STRING, &mount_op_id,
                              DBUS_TYPE_INVALID))
    {
      g_warning ("Error parsing args for MountUnmount(): %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }

  print_debug ("in handle_mount_unmount");

  ret = DBUS_HANDLER_RESULT_HANDLED;

  sender = dbus_message_get_sender (message);

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

  if (g_object_get_data (G_OBJECT (mount), "cancellable") != NULL)
    {
      DBusMessage *reply;
      reply = dbus_message_new_error (message,
                                      "org.gtk.Private.RemoteVolumeMonitor.Failed",
                                      "An operation is already pending");
      dbus_connection_send (connection, reply, NULL);
      dbus_message_unref (reply);
      goto out;
    }

  cancellable = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (mount), "cancellable", cancellable, g_object_unref);
  g_object_set_data_full (G_OBJECT (cancellable), "owner", g_strdup (sender), g_free);
  g_object_set_data_full (G_OBJECT (cancellable), "cancellation_id", g_strdup (cancellation_id), g_free);
  outstanding_ops = g_list_prepend (outstanding_ops, cancellable);
  g_object_weak_ref (G_OBJECT (cancellable),
                     cancellable_destroyed_cb,
                     NULL);

  mount_operation = NULL;
  if (mount_op_id != NULL && strlen (mount_op_id) > 0)
    {
      mount_operation = wrap_mount_op (mount_op_id, sender);
      g_object_set_data_full (G_OBJECT (mount), "mount_operation", mount_operation, g_object_unref);
    }

  g_object_ref (mount);
  g_mount_unmount_with_operation (mount,
                                  unmount_flags,
                                  mount_operation,
                                  cancellable,
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

static DBusHandlerResult
handle_mount_op_reply (DBusConnection *connection, DBusMessage *message)
{
  const char *mount_op_id;
  dbus_int32_t result;
  const char *user_name;
  const char *domain;
  const char *encoded_password;
  char *decoded_password;
  gsize decoded_password_len;
  dbus_int32_t password_save;
  dbus_int32_t choice;
  dbus_bool_t anonymous;
  DBusError dbus_error;
  DBusHandlerResult ret;
  GList *volumes, *l;
  DBusMessage *reply;
  GMountOperation *mount_operation;
  const gchar *sender;

  volumes = NULL;
  decoded_password = NULL;
  ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  dbus_error_init (&dbus_error);
  if (!dbus_message_get_args (message,
                              &dbus_error,
                              DBUS_TYPE_STRING, &mount_op_id,
                              DBUS_TYPE_INT32, &result,
                              DBUS_TYPE_STRING, &user_name,
                              DBUS_TYPE_STRING, &domain,
                              DBUS_TYPE_STRING, &encoded_password,
                              DBUS_TYPE_INT32, &password_save,
                              DBUS_TYPE_INT32, &choice,
                              DBUS_TYPE_BOOLEAN, &anonymous,
                              DBUS_TYPE_INVALID))
    {
      g_warning ("Error parsing args for MountOpReply(): %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }

  print_debug ("in handle_mount_op_reply");

  ret = DBUS_HANDLER_RESULT_HANDLED;

  sender = dbus_message_get_sender (message);

  /* Find the op */
  mount_operation = NULL;

  for (l = outstanding_mount_op_objects; l != NULL; l = l->next)
    {
      GMountOperation *op = G_MOUNT_OPERATION (l->data);
      const gchar *owner;
      const gchar *id;

      owner = g_object_get_data (G_OBJECT (op), "mount_op_owner");
      id = g_object_get_data (G_OBJECT (op), "mount_op_id");
      if (g_strcmp0 (owner, sender) == 0 && g_strcmp0 (id, mount_op_id) == 0)
        {
          print_debug ("found mount_op");
          mount_operation = op;
          break;
        }
    }

  if (mount_operation == NULL)
    {
      DBusMessage *reply;
      reply = dbus_message_new_error (message,
                                      "org.gtk.Private.RemoteVolumeMonitor.NotFound",
                                      "No outstanding mount operation");
      dbus_connection_send (connection, reply, NULL);
      dbus_message_unref (reply);
      goto out;
    }

  decoded_password = (gchar *) g_base64_decode (encoded_password, &decoded_password_len);

  g_mount_operation_set_username (mount_operation, user_name);
  g_mount_operation_set_domain (mount_operation, domain);
  g_mount_operation_set_password (mount_operation, decoded_password);
  g_mount_operation_set_password_save (mount_operation, password_save);
  g_mount_operation_set_choice (mount_operation, choice);
  g_mount_operation_set_anonymous (mount_operation, anonymous);

  g_mount_operation_reply (mount_operation, result);

  reply = dbus_message_new_method_return (message);
  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (reply);

 out:
  g_free (decoded_password);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
volume_mount_cb (GVolume *volume, GAsyncResult *result, DBusMessage *message)
{
  GError *error;
  DBusMessage *reply;

  print_debug ("in volume_mount_cb");

  g_object_set_data (G_OBJECT (volume), "mount_operation", NULL);
  g_object_set_data (G_OBJECT (volume), "cancellable", NULL);

  error = NULL;
  if (!g_volume_mount_finish (volume, result, &error))
    {
      print_debug ("  error: %s", error->message);
      reply = _dbus_message_new_from_gerror (message, error);
      g_error_free (error);
    }
  else
    {
      print_debug (" success");
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
  const char *cancellation_id;
  const char *sender;
  dbus_uint32_t mount_flags;
  const char *mount_op_id;
  DBusError dbus_error;
  GList *volumes, *l;
  GVolume *volume;
  DBusHandlerResult ret;
  GMountOperation *mount_operation;
  GCancellable *cancellable;

  volumes = NULL;
  ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  dbus_error_init (&dbus_error);
  if (!dbus_message_get_args (message, &dbus_error,
                              DBUS_TYPE_STRING, &id,
                              DBUS_TYPE_STRING, &cancellation_id,
                              DBUS_TYPE_UINT32, &mount_flags,
                              DBUS_TYPE_STRING, &mount_op_id,
                              DBUS_TYPE_INVALID))
    {
      g_warning ("Error parsing args for VolumeMount(): %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }

  print_debug ("in handle_volume_mount");

  ret = DBUS_HANDLER_RESULT_HANDLED;

  sender = dbus_message_get_sender (message);

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

  if (g_object_get_data (G_OBJECT (volume), "cancellable") != NULL)
    {
      DBusMessage *reply;
      reply = dbus_message_new_error (message,
                                      "org.gtk.Private.RemoteVolumeMonitor.Failed",
                                      "An operation is already pending");
      dbus_connection_send (connection, reply, NULL);
      dbus_message_unref (reply);
      goto out;
    }

  mount_operation = NULL;
  if (mount_op_id != NULL && strlen (mount_op_id) > 0)
    {
      mount_operation = wrap_mount_op (mount_op_id, sender);
      g_object_set_data_full (G_OBJECT (volume), "mount_operation", mount_operation, g_object_unref);
    }

  cancellable = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (volume), "cancellable", cancellable, g_object_unref);
  g_object_set_data_full (G_OBJECT (cancellable), "owner", g_strdup (sender), g_free);
  g_object_set_data_full (G_OBJECT (cancellable), "cancellation_id", g_strdup (cancellation_id), g_free);
  outstanding_ops = g_list_prepend (outstanding_ops, cancellable);
  g_object_weak_ref (G_OBJECT (cancellable),
                     cancellable_destroyed_cb,
                     NULL);

  g_volume_mount (volume,
                  mount_flags,
                  mount_operation,
                  cancellable,
                  (GAsyncReadyCallback) volume_mount_cb,
                  dbus_message_ref (message));

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

  print_debug ("in drive_eject_cb");

  g_object_set_data (G_OBJECT (drive), "cancellable", NULL);
  g_object_set_data (G_OBJECT (drive), "mount_operation", NULL);

  error = NULL;
  if (!g_drive_eject_with_operation_finish (drive, result, &error))
    {
      print_debug ("  error: %s", error->message);
      reply = _dbus_message_new_from_gerror (message, error);
      g_error_free (error);
    }
  else
    {
      print_debug (" success");
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
  const char *cancellation_id;
  const char *sender;
  const char *mount_op_id;
  GMountOperation *mount_operation;
  GCancellable *cancellable;
  dbus_uint32_t unmount_flags;
  DBusError dbus_error;
  GList *drives, *l;
  GDrive *drive;
  DBusHandlerResult ret;

  drive = NULL;
  drives = NULL;
  unmount_flags = 0;
  ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  dbus_error_init (&dbus_error);
  if (!dbus_message_get_args (message, &dbus_error,
                              DBUS_TYPE_STRING, &id,
                              DBUS_TYPE_STRING, &cancellation_id,
                              DBUS_TYPE_UINT32, &unmount_flags,
                              DBUS_TYPE_STRING, &mount_op_id,
                              DBUS_TYPE_INVALID))
    {
      g_warning ("Error parsing args for DriveEject(): %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }

  print_debug ("in handle_drive_eject");

  ret = DBUS_HANDLER_RESULT_HANDLED;

  sender = dbus_message_get_sender (message);

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

  if (g_object_get_data (G_OBJECT (drive), "cancellable") != NULL)
    {
      DBusMessage *reply;
      reply = dbus_message_new_error (message,
                                      "org.gtk.Private.RemoteVolumeMonitor.Failed",
                                      "An operation is already pending");
      dbus_connection_send (connection, reply, NULL);
      dbus_message_unref (reply);
      goto out;
    }

  cancellable = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (drive), "cancellable", cancellable, g_object_unref);
  g_object_set_data_full (G_OBJECT (cancellable), "owner", g_strdup (sender), g_free);
  g_object_set_data_full (G_OBJECT (cancellable), "cancellation_id", g_strdup (cancellation_id), g_free);
  outstanding_ops = g_list_prepend (outstanding_ops, cancellable);
  g_object_weak_ref (G_OBJECT (cancellable),
                     cancellable_destroyed_cb,
                     NULL);

  mount_operation = NULL;
  if (mount_op_id != NULL && strlen (mount_op_id) > 0)
    {
      mount_operation = wrap_mount_op (mount_op_id, sender);
      g_object_set_data_full (G_OBJECT (drive), "mount_operation", mount_operation, g_object_unref);
    }

  g_drive_eject_with_operation (drive,
                                unmount_flags,
                                mount_operation,
                                cancellable,
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
drive_stop_cb (GDrive *drive, GAsyncResult *result, DBusMessage *message)
{
  GError *error;
  DBusMessage *reply;

  print_debug ("in drive_stop_cb");

  g_object_set_data (G_OBJECT (drive), "cancellable", NULL);
  g_object_set_data (G_OBJECT (drive), "mount_operation", NULL);

  error = NULL;
  if (!g_drive_stop_finish (drive, result, &error))
    {
      print_debug ("  error: %s", error->message);
      reply = _dbus_message_new_from_gerror (message, error);
      g_error_free (error);
    }
  else
    {
      print_debug (" success");
      reply = dbus_message_new_method_return (message);
    }

  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (message);
  dbus_message_unref (reply);
}

static DBusHandlerResult
handle_drive_stop (DBusConnection *connection, DBusMessage *message)
{
  const char *id;
  const char *cancellation_id;
  const char *sender;
  const char *mount_op_id;
  GMountOperation *mount_operation;
  GCancellable *cancellable;
  dbus_uint32_t unmount_flags;
  DBusError dbus_error;
  GList *drives, *l;
  GDrive *drive;
  DBusHandlerResult ret;

  drive = NULL;
  drives = NULL;
  unmount_flags = 0;
  ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  dbus_error_init (&dbus_error);
  if (!dbus_message_get_args (message, &dbus_error,
                              DBUS_TYPE_STRING, &id,
                              DBUS_TYPE_STRING, &cancellation_id,
                              DBUS_TYPE_UINT32, &unmount_flags,
                              DBUS_TYPE_STRING, &mount_op_id,
                              DBUS_TYPE_INVALID))
    {
      g_warning ("Error parsing args for DriveStop(): %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }

  print_debug ("in handle_drive_stop");

  ret = DBUS_HANDLER_RESULT_HANDLED;

  sender = dbus_message_get_sender (message);

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

  if (g_object_get_data (G_OBJECT (drive), "cancellable") != NULL)
    {
      DBusMessage *reply;
      reply = dbus_message_new_error (message,
                                      "org.gtk.Private.RemoteVolumeMonitor.Failed",
                                      "An operation is already pending");
      dbus_connection_send (connection, reply, NULL);
      dbus_message_unref (reply);
      goto out;
    }

  cancellable = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (drive), "cancellable", cancellable, g_object_unref);
  g_object_set_data_full (G_OBJECT (cancellable), "owner", g_strdup (sender), g_free);
  g_object_set_data_full (G_OBJECT (cancellable), "cancellation_id", g_strdup (cancellation_id), g_free);
  outstanding_ops = g_list_prepend (outstanding_ops, cancellable);
  g_object_weak_ref (G_OBJECT (cancellable),
                     cancellable_destroyed_cb,
                     NULL);

  mount_operation = NULL;
  if (mount_op_id != NULL && strlen (mount_op_id) > 0)
    {
      mount_operation = wrap_mount_op (mount_op_id, sender);
      g_object_set_data_full (G_OBJECT (drive), "mount_operation", mount_operation, g_object_unref);
    }

  g_drive_stop (drive,
                unmount_flags,
                mount_operation,
                cancellable,
                (GAsyncReadyCallback) drive_stop_cb,
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
drive_start_cb (GDrive *drive, GAsyncResult *result, DBusMessage *message)
{
  GError *error;
  DBusMessage *reply;

  print_debug ("in drive_start_cb");

  g_object_set_data (G_OBJECT (drive), "mount_operation", NULL);
  g_object_set_data (G_OBJECT (drive), "cancellable", NULL);

  error = NULL;
  if (!g_drive_start_finish (drive, result, &error))
    {
      print_debug ("  error: %s", error->message);
      reply = _dbus_message_new_from_gerror (message, error);
      g_error_free (error);
    }
  else
    {
      print_debug (" success");
      reply = dbus_message_new_method_return (message);
    }

  dbus_connection_send (connection, reply, NULL);
  dbus_message_unref (message);
  dbus_message_unref (reply);
}

static DBusHandlerResult
handle_drive_start (DBusConnection *connection, DBusMessage *message)
{
  const char *id;
  const char *cancellation_id;
  const char *sender;
  const char *mount_op_id;
  GDriveStartFlags flags;
  DBusError dbus_error;
  GList *drives, *l;
  GDrive *drive;
  DBusHandlerResult ret;
  GMountOperation *mount_operation;
  GCancellable *cancellable;

  drives = NULL;
  ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  dbus_error_init (&dbus_error);
  if (!dbus_message_get_args (message, &dbus_error,
                              DBUS_TYPE_STRING, &id,
                              DBUS_TYPE_STRING, &cancellation_id,
                              DBUS_TYPE_UINT32, &flags,
                              DBUS_TYPE_STRING, &mount_op_id,
                              DBUS_TYPE_INVALID))
    {
      g_warning ("Error parsing args for DriveStart(): %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }

  print_debug ("in handle_drive_start");

  ret = DBUS_HANDLER_RESULT_HANDLED;

  sender = dbus_message_get_sender (message);

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

  if (g_object_get_data (G_OBJECT (drive), "cancellable") != NULL)
    {
      DBusMessage *reply;
      reply = dbus_message_new_error (message,
                                      "org.gtk.Private.RemoteVolumeMonitor.Failed",
                                      "An operation is already pending");
      dbus_connection_send (connection, reply, NULL);
      dbus_message_unref (reply);
      goto out;
    }

  mount_operation = NULL;
  if (mount_op_id != NULL && strlen (mount_op_id) > 0)
    {
      mount_operation = wrap_mount_op (mount_op_id, sender);
      g_object_set_data_full (G_OBJECT (drive), "mount_operation", mount_operation, g_object_unref);
    }

  cancellable = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (drive), "cancellable", cancellable, g_object_unref);
  g_object_set_data_full (G_OBJECT (cancellable), "owner", g_strdup (sender), g_free);
  g_object_set_data_full (G_OBJECT (cancellable), "cancellation_id", g_strdup (cancellation_id), g_free);
  outstanding_ops = g_list_prepend (outstanding_ops, cancellable);
  g_object_weak_ref (G_OBJECT (cancellable),
                     cancellable_destroyed_cb,
                     NULL);

  g_drive_start (drive,
                 flags,
                 mount_operation,
                 cancellable,
                 (GAsyncReadyCallback) drive_start_cb,
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

  print_debug ("in drive_poll_for_media_cb");

  g_object_set_data (G_OBJECT (drive), "cancellable", NULL);

  error = NULL;
  if (!g_drive_poll_for_media_finish (drive, result, &error))
    {
      print_debug ("  error: %s", error->message);
      reply = _dbus_message_new_from_gerror (message, error);
      g_error_free (error);
    }
  else
    {
      print_debug (" success");
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
  const char *cancellation_id;
  const char *sender;
  GCancellable *cancellable;
  DBusError dbus_error;
  GList *drives, *l;
  GDrive *drive;
  DBusHandlerResult ret;

  drive = NULL;
  drives = NULL;
  ret = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

  dbus_error_init (&dbus_error);
  if (!dbus_message_get_args (message, &dbus_error,
                              DBUS_TYPE_STRING, &id,
                              DBUS_TYPE_STRING, &cancellation_id,
                              DBUS_TYPE_INVALID))
    {
      g_warning ("Error parsing args for DrivePollForMedia(): %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }

  print_debug ("in handle_drive_poll_for_media");

  ret = DBUS_HANDLER_RESULT_HANDLED;

  sender = dbus_message_get_sender (message);

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

  if (g_object_get_data (G_OBJECT (drive), "cancellable") != NULL)
    {
      DBusMessage *reply;
      reply = dbus_message_new_error (message,
                                      "org.gtk.Private.RemoteVolumeMonitor.Failed",
                                      "An operation is already pending");
      dbus_connection_send (connection, reply, NULL);
      dbus_message_unref (reply);
      goto out;
    }

  cancellable = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (drive), "cancellable", cancellable, g_object_unref);
  g_object_set_data_full (G_OBJECT (cancellable), "owner", g_strdup (sender), g_free);
  g_object_set_data_full (G_OBJECT (cancellable), "cancellation_id", g_strdup (cancellation_id), g_free);
  outstanding_ops = g_list_prepend (outstanding_ops, cancellable);
  g_object_weak_ref (G_OBJECT (cancellable),
                     cancellable_destroyed_cb,
                     NULL);

  g_drive_poll_for_media (drive,
                          cancellable,
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

  print_debug ("in handle_supported");

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
handle_cancel_operation (DBusConnection *connection, DBusMessage *message)
{
  DBusMessage *reply;
  DBusMessageIter iter;
  DBusError dbus_error;
  dbus_bool_t was_cancelled;
  const char *sender;
  const char *cancellation_id;
  GList *l;

  was_cancelled = FALSE;

  sender = dbus_message_get_sender (message);

  dbus_error_init (&dbus_error);
  if (!dbus_message_get_args (message, &dbus_error,
                              DBUS_TYPE_STRING, &cancellation_id,
                              DBUS_TYPE_INVALID))
    {
      g_warning ("Error parsing args for CancelOperation(): %s: %s", dbus_error.name, dbus_error.message);
      dbus_error_free (&dbus_error);
      goto out;
    }

  print_debug ("in handle_cancel_operation");

  /* Find GCancellable to cancel */
  for (l = outstanding_ops; l != NULL; l = l->next)
    {
      GCancellable *cancellable = G_CANCELLABLE (l->data);
      const gchar *owner;
      const gchar *id;

      owner = g_object_get_data (G_OBJECT (cancellable), "owner");
      id = g_object_get_data (G_OBJECT (cancellable), "cancellation_id");
      if (g_strcmp0 (owner, sender) == 0 && g_strcmp0 (id, cancellation_id) == 0)
        {
          print_debug ("found op to cancel");
          g_cancellable_cancel (cancellable);

          was_cancelled = TRUE;
          break;
        }
    }

  if (!was_cancelled)
    g_warning ("didn't find op to cancel");

 out:
  reply = dbus_message_new_method_return (message);
  dbus_message_iter_init_append (reply, &iter);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_BOOLEAN, &was_cancelled);
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

  if (dbus_message_is_signal (message, "org.freedesktop.DBus", "NameLost"))
    {
      /* means that someone has claimed our name (we allow replacement) */
      g_warning ("Got NameLost, some other instance replaced us");
      exit (0);
    }
  else if (dbus_message_is_signal (message, "org.freedesktop.DBus", "NameOwnerChanged"))
    {
      DBusMessageIter iter;
      const gchar *name;
      const gchar *old_owner;
      const gchar *new_owner;

      dbus_message_iter_init (message, &iter);
      dbus_message_iter_get_basic (&iter, &name);
      dbus_message_iter_next (&iter);
      dbus_message_iter_get_basic (&iter, &old_owner);
      dbus_message_iter_next (&iter);
      dbus_message_iter_get_basic (&iter, &new_owner);
      dbus_message_iter_next (&iter);

      print_debug ("NameOwnerChanged: '%s' '%s' '%s'", name, old_owner, new_owner);

      if (strlen (new_owner) == 0)
        {
          GList *l;

          /* if @name has outstanding mount operation objects; abort them */
          for (l = outstanding_mount_op_objects; l != NULL; l = l->next)
            {
              GMountOperation *op = G_MOUNT_OPERATION (l->data);
              const gchar *owner;
              const gchar *id;

              owner = g_object_get_data (G_OBJECT (op), "mount_op_owner");
              id = g_object_get_data (G_OBJECT (op), "mount_op_id");
              if (g_strcmp0 (owner, name) == 0)
                {
                  print_debug ("****** name `%s' has an outstanding mount operation object, aborting it",
                               name);
                  g_mount_operation_reply (op, G_MOUNT_OPERATION_ABORTED);
                }
            }

          /* see if @name has outstanding ops; if so, cancel them */
          for (l = outstanding_ops; l != NULL; l = l->next)
            {
              GCancellable *cancellable = G_CANCELLABLE (l->data);
              const gchar *owner;

              owner = g_object_get_data (G_OBJECT (cancellable), "owner");
              print_debug ("looking at op for %s", owner);
              if (g_strcmp0 (owner, name) == 0)
                {
                  print_debug ("****** name `%s' has an outstanding op, cancelling it",
                               name);
                  g_cancellable_cancel (cancellable);
                }
            }

          remove_name_owned_changed_for_unique_name (name);
        }

    }
  else if (g_strcmp0 (dbus_message_get_interface (message), "org.gtk.Private.RemoteVolumeMonitor") == 0 &&
           g_strcmp0 (dbus_message_get_path (message), "/org/gtk/Private/RemoteVolumeMonitor") == 0)
    {
      /* If someone is calling into this object and interface, start watching their name so
       * we can cancel operations initiated by them when they disconnect
       */
      ensure_name_owner_changed_for_unique_name (dbus_message_get_sender (message));

      if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "IsSupported"))
        {
          ret = handle_is_supported (connection, message);
        }
      else
        {
          if (monitor != NULL)
            {
              if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "List"))
                ret = handle_list (connection, message);

              else if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "CancelOperation"))
                ret = handle_cancel_operation (connection, message);

              else if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "MountUnmount"))
                ret = handle_mount_unmount (connection, message);

              else if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "VolumeMount"))
                ret = handle_volume_mount (connection, message);

              else if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "DriveEject"))
                ret = handle_drive_eject (connection, message);

              else if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "DrivePollForMedia"))
                ret = handle_drive_poll_for_media (connection, message);

              else if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "DriveStart"))
                ret = handle_drive_start (connection, message);

              else if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "DriveStop"))
                ret = handle_drive_stop (connection, message);

              else if (dbus_message_is_method_call (message, "org.gtk.Private.RemoteVolumeMonitor", "MountOpReply"))
                ret = handle_mount_op_reply (connection, message);
            }
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

  message = dbus_message_new_signal ("/org/gtk/Private/RemoteVolumeMonitor",
                                     "org.gtk.Private.RemoteVolumeMonitor",
                                     signal_name);
  dbus_message_iter_init_append (message, &iter);
  dbus_message_iter_append_basic (&iter, DBUS_TYPE_STRING, &the_dbus_name);
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
  emit_signal (connection, "DriveEjectButton", drive, (AppendFunc) append_drive);
}

static void
drive_stop_button (GVolumeMonitor *monitor, GDrive *drive, DBusConnection *connection)
{
  emit_signal (connection, "DriveStopButton", drive, (AppendFunc) append_drive);
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
mount_sniff_x_content_type (GMount *mount)
{
  char **x_content_types;
  x_content_types = g_mount_guess_content_type_sync (mount, TRUE, NULL, NULL);
  g_object_set_data_full (G_OBJECT (mount), "x-content-types", x_content_types, (GDestroyNotify) g_strfreev);
}

static void
mount_added (GVolumeMonitor *monitor, GMount *mount, DBusConnection *connection)
{
  mount_sniff_x_content_type (mount);
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
  GList *mounts;
  GList *l;

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

  mounts = g_volume_monitor_get_mounts (monitor);
  for (l = mounts; l != NULL; l = l->next)
    mount_sniff_x_content_type (G_MOUNT (l->data));
  g_list_foreach (mounts, (GFunc) g_object_unref, NULL);
  g_list_free (mounts);

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
  the_dbus_name = dbus_name;
  unique_names_being_watched = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

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

  rc = dbus_bus_request_name (connection,
                              dbus_name,
                              DBUS_NAME_FLAG_ALLOW_REPLACEMENT |
                              DBUS_NAME_FLAG_DO_NOT_QUEUE |
                              DBUS_NAME_FLAG_REPLACE_EXISTING,
                              &dbus_error);
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
      g_signal_connect (monitor, "drive-stop-button", (GCallback) drive_stop_button, connection);

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
