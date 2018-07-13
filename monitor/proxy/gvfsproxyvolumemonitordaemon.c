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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>
#include <string.h>
#include <locale.h>
#include <gio/gio.h>
#include <glib/gi18n.h>
#include <stdlib.h>
#include <glib/gprintf.h>

#include "gvfsproxyvolumemonitordaemon.h"
#include "gvfsvolumemonitordbus.h"

/* ---------------------------------------------------------------------------------------------------- */

static GMainLoop *loop = NULL;
static GVolumeMonitor *monitor = NULL;
static GType the_volume_monitor_type;
static const char *the_dbus_name = NULL;
static GList *outstanding_ops = NULL;
static GList *outstanding_mount_op_objects = NULL;
static GHashTable *unique_names_being_watched = NULL;
static gboolean always_call_mount = FALSE;

static GVfsRemoteVolumeMonitor *monitor_daemon = NULL;

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
g_proxy_mount_operation_show_unmount_progress (GMountOperation *op,
                                               const gchar     *message,
                                               gint64           time_left,
                                               gint64           bytes_left)
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
  mount_op_class->show_unmount_progress = g_proxy_mount_operation_show_unmount_progress;
}

static void
ask_password_cb (GMountOperation          *mount_operation,
                 const gchar              *message_to_show,
                 const gchar              *default_user,
                 const gchar              *default_domain,
                 GAskPasswordFlags         flags,
                 GVfsRemoteVolumeMonitor  *monitor)
{
  const gchar *mount_op_id;
  const gchar *mount_op_owner;
  GDBusConnection *connection;
  GError *error;

  print_debug ("in ask_password_cb %s", message_to_show);

  mount_op_id = g_object_get_data (G_OBJECT (mount_operation), "mount_op_id");
  mount_op_owner = g_object_get_data (G_OBJECT (mount_operation), "mount_op_owner");

  if (message_to_show == NULL)
    message_to_show = "";

  if (default_user == NULL)
    default_user = "";

  if (default_domain == NULL)
    default_domain = "";

  connection = g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (monitor));
  g_assert (connection != NULL);
  
  error = NULL;
  if (!g_dbus_connection_emit_signal (connection,
                                      mount_op_owner,
                                      "/org/gtk/Private/RemoteVolumeMonitor",
                                      "org.gtk.Private.RemoteVolumeMonitor",
                                      "MountOpAskPassword",
                                      g_variant_new ("(sssssu)",
                                                     the_dbus_name,
                                                     mount_op_id,
                                                     message_to_show,
                                                     default_user,
                                                     default_domain,
                                                     flags),
                                      &error))
    {
      g_printerr ("Error emitting signal: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }
}

static void
ask_question_cb (GMountOperation          *mount_operation,
                 const gchar              *message_to_show,
                 gchar                   **choices,
                 GVfsRemoteVolumeMonitor  *monitor)
{
  const gchar *mount_op_id;
  const gchar *mount_op_owner;
  guint n;
  GVariantBuilder *choices_array;
  GDBusConnection *connection;
  GError *error;

  print_debug ("in ask_question_cb %s", message_to_show);

  mount_op_id = g_object_get_data (G_OBJECT (mount_operation), "mount_op_id");
  mount_op_owner = g_object_get_data (G_OBJECT (mount_operation), "mount_op_owner");

  if (message_to_show == NULL)
    message_to_show = "";

  choices_array = g_variant_builder_new (G_VARIANT_TYPE_STRING_ARRAY);
  for (n = 0; choices != NULL && choices[n] != NULL; n++)
    {
      g_variant_builder_add (choices_array, "s", choices[n]);
    }

  connection = g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (monitor));
  g_assert (connection != NULL);
  
  error = NULL;
  if (!g_dbus_connection_emit_signal (connection,
                                      mount_op_owner,
                                      "/org/gtk/Private/RemoteVolumeMonitor",
                                      "org.gtk.Private.RemoteVolumeMonitor",
                                      "MountOpAskQuestion",
                                      g_variant_new ("(sssas)",
                                                     the_dbus_name,
                                                     mount_op_id,
                                                     message_to_show,
                                                     choices_array),
                                      &error))
    {
      g_printerr ("Error emitting signal: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }

  g_variant_builder_unref (choices_array);
}

static void
show_processes_cb (GMountOperation          *mount_operation,
                   const gchar              *message_to_show,
                   GArray                   *processes,
                   gchar                   **choices,
                   GVfsRemoteVolumeMonitor  *monitor)
{
  const gchar *mount_op_id;
  const gchar *mount_op_owner;
  guint n;
  GVariantBuilder *pids;
  GVariantBuilder *choices_array;
  GDBusConnection *connection;
  GError *error;

  print_debug ("in show_processes_cb %s", message_to_show);

  mount_op_id = g_object_get_data (G_OBJECT (mount_operation), "mount_op_id");
  mount_op_owner = g_object_get_data (G_OBJECT (mount_operation), "mount_op_owner");

  print_debug ("  owner =  '%s'", mount_op_owner);

  if (message_to_show == NULL)
    message_to_show = "";

  pids = g_variant_builder_new (G_VARIANT_TYPE ("ai"));
  for (n = 0; processes != NULL && n < processes->len; n++)
    {
      GPid pid;
      pid = g_array_index (processes, GPid, n);
      g_variant_builder_add (pids, "i", pid);
    }

  choices_array = g_variant_builder_new (G_VARIANT_TYPE_STRING_ARRAY);
  for (n = 0; choices != NULL && choices[n] != NULL; n++)
    {
      g_variant_builder_add (choices_array, "s", choices[n]);
    }

  connection = g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (monitor));
  g_assert (connection != NULL);
  
  error = NULL;
  if (!g_dbus_connection_emit_signal (connection,
                                      mount_op_owner,
                                      "/org/gtk/Private/RemoteVolumeMonitor",
                                      "org.gtk.Private.RemoteVolumeMonitor",
                                      "MountOpShowProcesses",
                                      g_variant_new ("(sssaias)",
                                                     the_dbus_name,
                                                     mount_op_id,
                                                     message_to_show,
                                                     pids,
                                                     choices_array),
                                      &error))
    {
      g_printerr ("Error emitting signal: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }

  g_variant_builder_unref (pids);
  g_variant_builder_unref (choices_array);
}

static void
show_unmount_progress_cb (GMountOperation *mount_operation,
                          const gchar *message_to_show,
                          gint64 time_left,
                          gint64 bytes_left,
                          GVfsRemoteVolumeMonitor *monitor)
{
  const gchar *mount_op_id;
  const gchar *mount_op_owner;
  GDBusConnection *connection;
  GError *error;

  print_debug ("in show_unmount_progress_cb %s", message_to_show);

  mount_op_id = g_object_get_data (G_OBJECT (mount_operation), "mount_op_id");
  mount_op_owner = g_object_get_data (G_OBJECT (mount_operation), "mount_op_owner");

  print_debug ("  owner =  '%s'", mount_op_owner);

  if (message_to_show == NULL)
    message_to_show = "";

  connection = g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (monitor));
  g_assert (connection != NULL);
  
  error = NULL;
  if (!g_dbus_connection_emit_signal (connection,
                                      mount_op_owner,
                                      "/org/gtk/Private/RemoteVolumeMonitor",
                                      "org.gtk.Private.RemoteVolumeMonitor",
                                      "MountOpShowUnmountProgress",
                                      g_variant_new ("(sssxx)",
                                                     the_dbus_name,
                                                     mount_op_id,
                                                     message_to_show,
                                                     time_left,
                                                     bytes_left),
                                      &error))
    {
      g_printerr ("Error emitting signal: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }
}

static void
aborted_cb (GMountOperation         *mount_operation,
            GVfsRemoteVolumeMonitor *monitor)
{
  const gchar *mount_op_id;
  const gchar *mount_op_owner;
  GDBusConnection *connection;
  GError *error;

  print_debug ("in aborted_cb");

  mount_op_id = g_object_get_data (G_OBJECT (mount_operation), "mount_op_id");
  mount_op_owner = g_object_get_data (G_OBJECT (mount_operation), "mount_op_owner");

  connection = g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (monitor));
  g_assert (connection != NULL);
  
  error = NULL;
  if (!g_dbus_connection_emit_signal (connection,
                                      mount_op_owner,
                                      "/org/gtk/Private/RemoteVolumeMonitor",
                                      "org.gtk.Private.RemoteVolumeMonitor",
                                      "MountOpAborted",
                                      g_variant_new ("(ss)",
                                                     the_dbus_name,
                                                     mount_op_id),
                                      &error))
    {
      g_printerr ("Error emitting signal: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }
}

static void
mount_op_destroyed_cb (gpointer user_data,
                       GObject *where_the_mount_op_was)
{
  outstanding_mount_op_objects = g_list_remove (outstanding_mount_op_objects, where_the_mount_op_was);
}

static GMountOperation *
wrap_mount_op (const gchar *mount_op_id,
               const gchar *mount_op_owner,
               GVfsRemoteVolumeMonitor *monitor)
{
  GMountOperation *op;

  op = g_proxy_mount_operation_new ();
  //op = g_mount_operation_new ();
  g_signal_connect (op, "ask-password", G_CALLBACK (ask_password_cb), monitor);
  g_signal_connect (op, "ask-question", G_CALLBACK (ask_question_cb), monitor);
  g_signal_connect (op, "show-processes", G_CALLBACK (show_processes_cb), monitor);
  g_signal_connect (op, "show-unmount-progress", G_CALLBACK (show_unmount_progress_cb), monitor);
  g_signal_connect (op, "aborted", G_CALLBACK (aborted_cb), monitor);
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
on_name_owner_vanished (GDBusConnection *connection,
                        const gchar     *name,
                        gpointer         user_data)
{
  GList *l;
  guint name_watcher_id;
  
  print_debug ("Name owner '%s' vanished", name);

  /* if @name has outstanding mount operation objects; abort them */
  for (l = outstanding_mount_op_objects; l != NULL; l = l->next)
    {
      GMountOperation *op = G_MOUNT_OPERATION (l->data);
      const gchar *owner;

      owner = g_object_get_data (G_OBJECT (op), "mount_op_owner");
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

  /* unwatch the name */
  name_watcher_id = GPOINTER_TO_UINT (g_hash_table_lookup (unique_names_being_watched, name));
  g_hash_table_remove (unique_names_being_watched, name);
  if (name_watcher_id == 0)
    {
      g_warning ("Was asked to remove match rule for unique_name %s but we don't have one", name);
    }
  else
    {
      /* Note that calling g_bus_unwatch_name () makes @name invalid */
      g_bus_unwatch_name (name_watcher_id);
    }
}

static void
ensure_name_owner_changed_for_unique_name (GDBusMethodInvocation *invocation)
{
  guint name_watcher_id;
  const gchar *unique_name;
  
  unique_name = g_dbus_method_invocation_get_sender (invocation);

  if (g_hash_table_lookup (unique_names_being_watched, unique_name) != NULL)
    return;
  
  name_watcher_id = g_bus_watch_name_on_connection (g_dbus_method_invocation_get_connection (invocation),
                                                    unique_name,
                                                    G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                    NULL, /* name_appeared_handler */
                                                    on_name_owner_vanished,
                                                    NULL,
                                                    NULL);
  g_hash_table_insert (unique_names_being_watched, g_strdup (unique_name), GUINT_TO_POINTER (name_watcher_id));
}

static void monitor_try_create (void);

/* string               id
 * string               name
 * string               gicon_data
 * string               symbolic_gicon_data
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
 * string               sort_key
 * a{sv}                expansion
 *      boolean              is-removable
 */
#define DRIVE_STRUCT_TYPE "(ssssbbbbbbbbuasa{ss}sa{sv})"

static GVariant *
drive_to_dbus (GDrive *drive)
{
  char *id;
  char *name;
  GIcon *icon;
  GIcon *symbolic_icon;
  char *icon_data;
  char *symbolic_icon_data;
  gboolean can_eject;
  gboolean can_poll_for_media;
  gboolean has_media;
  gboolean is_removable;
  gboolean is_media_removable;
  gboolean is_media_check_automatic;
  gboolean can_start;
  gboolean can_start_degraded;
  gboolean can_stop;
  GDriveStartStopType start_stop_type;
  GList *volumes, *l;
  char **identifiers;
  int n;
  const gchar *sort_key;
  GVariant *result;
  GVariantBuilder *volume_array_builder;
  GVariantBuilder *identifiers_builder;
  GVariantBuilder *expansion_builder;


  id = g_strdup_printf ("%p", drive);
  name = g_drive_get_name (drive);
  icon = g_drive_get_icon (drive);
  if (icon)
    icon_data = g_icon_to_string (icon);
  else
    icon_data = g_strdup ("");
  symbolic_icon = g_drive_get_symbolic_icon (drive);
  if (symbolic_icon)
    symbolic_icon_data = g_icon_to_string (symbolic_icon);
  else
    symbolic_icon_data = g_strdup ("");
  can_eject = g_drive_can_eject (drive);
  can_poll_for_media = g_drive_can_poll_for_media (drive);
  has_media = g_drive_has_media (drive);
  is_removable = g_drive_is_removable (drive);
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

  sort_key = g_drive_get_sort_key (drive);
  if (sort_key == NULL)
    sort_key = "";

  volume_array_builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
  for (l = volumes; l != NULL; l = l->next)
    {
      GVolume *volume = G_VOLUME (l->data);
      char *volume_id;
      volume_id = g_strdup_printf ("%p", volume);
      g_variant_builder_add (volume_array_builder, "s", volume_id);
      g_free (volume_id);
    }

  identifiers_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{ss}"));
  for (n = 0; identifiers != NULL && identifiers[n] != NULL; n++)
    {
      char *id_value;
      id_value = g_drive_get_identifier (drive, identifiers[n]);

      g_variant_builder_add (identifiers_builder, "{ss}",
                             identifiers[n],
                             id_value);
      g_free (id_value);
    }

  expansion_builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (expansion_builder, "{sv}", "is-removable", g_variant_new_boolean (is_removable));
  /* left for future expansion without ABI breaks */

  for (n = 0; identifiers != NULL && identifiers[n] != NULL; n++)
    {
      char *id_value;
      id_value = g_drive_get_identifier (drive, identifiers[n]);

      g_variant_builder_add (identifiers_builder, "{ss}",
                             identifiers[n],
                             id_value);
      g_free (id_value);
    }

  result = g_variant_new (DRIVE_STRUCT_TYPE,
                          id,
                          name,
                          icon_data,
                          symbolic_icon_data,
                          can_eject,
                          can_poll_for_media,
                          has_media,
                          is_media_removable,
                          is_media_check_automatic,
                          can_start,
                          can_start_degraded,
                          can_stop,
                          start_stop_type,
                          volume_array_builder,
                          identifiers_builder,
                          sort_key,
                          expansion_builder);

  g_variant_builder_unref (volume_array_builder);
  g_variant_builder_unref (identifiers_builder);
  g_variant_builder_unref (expansion_builder);

  g_strfreev (identifiers);
  g_list_free_full (volumes, g_object_unref);
  g_free (icon_data);
  g_object_unref (icon);
  g_free (symbolic_icon_data);
  g_object_unref (symbolic_icon);
  g_free (name);
  g_free (id);

  return result;
}

/* string               id
 * string               name
 * string               gicon_data
 * string               symbolic_gicon_data
 * string               uuid
 * string               activation_uri
 * boolean              can-mount
 * boolean              should-automount
 * string               drive-id
 * string               mount-id
 * dict:string->string  identifiers
 * string               sort_key
 * a{sv}                expansion
 */
#define VOLUME_STRUCT_TYPE "(ssssssbbssa{ss}sa{sv})"

static GVariant *
volume_to_dbus (GVolume *volume)
{
  char *id;
  char *name;
  GIcon *icon;
  char *icon_data;
  GIcon *symbolic_icon;
  char *symbolic_icon_data;
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
  const gchar *sort_key;
  GVariant *result;
  GVariantBuilder *identifiers_builder;
  GVariantBuilder *expansion_builder;


  id = g_strdup_printf ("%p", volume);
  name = g_volume_get_name (volume);
  icon = g_volume_get_icon (volume);
  if (icon)
    icon_data = g_icon_to_string (icon);
  else
    icon_data = g_strdup ("");
  symbolic_icon = g_volume_get_symbolic_icon (volume);
  if (symbolic_icon)
    symbolic_icon_data = g_icon_to_string (symbolic_icon);
  else
    symbolic_icon_data = g_strdup ("");
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

  sort_key = g_volume_get_sort_key (volume);
  if (sort_key == NULL)
    sort_key = "";

  identifiers_builder = g_variant_builder_new (G_VARIANT_TYPE ("a{ss}"));
  for (n = 0; identifiers != NULL && identifiers[n] != NULL; n++)
    {
      char *id_value;
      id_value = g_volume_get_identifier (volume, identifiers[n]);
      if (id_value == NULL)
        continue;

      g_variant_builder_add (identifiers_builder, "{ss}",
                             identifiers[n],
                             id_value);
      g_free (id_value);
    }

  /* left for future expansion without ABI breaks */
  expansion_builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);

  if (always_call_mount)
    g_variant_builder_add (expansion_builder, "{sv}",
                           "always-call-mount", g_variant_new_boolean (TRUE));

  result = g_variant_new (VOLUME_STRUCT_TYPE,
                          id,
                          name,
                          icon_data,
                          symbolic_icon_data,
                          uuid,
                          activation_uri,
                          can_mount,
                          should_automount,
                          drive_id,
                          mount_id,
                          identifiers_builder,
                          sort_key,
                          expansion_builder);

  g_variant_builder_unref (identifiers_builder);
  g_variant_builder_unref (expansion_builder);

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
  g_free (symbolic_icon_data);
  g_object_unref (symbolic_icon);
  g_free (name);
  g_free (id);

  return result;
}

/* string               id
 * string               name
 * string               gicon_data
 * string               symbolic_gicon_data
 * string               uuid
 * string               root_uri
 * boolean              can-unmount
 * string               volume-id
 * array:string         x-content-types
 * string               sort_key
 * a{sv}                expansion
 */
#define MOUNT_STRUCT_TYPE "(ssssssbsassa{sv})"

static GVariant *
mount_to_dbus (GMount *mount)
{
  char *id;
  char *name;
  GIcon *icon;
  char *icon_data;
  GIcon *symbolic_icon;
  char *symbolic_icon_data;
  char *uuid;
  GFile *root;
  char *root_uri;
  gboolean can_unmount;
  GVolume *volume;
  char *volume_id;
  char **x_content_types;
  int n;
  const gchar *sort_key;
  GVariant *result;
  GVariantBuilder *x_content_types_array_builder;
  GVariantBuilder *expansion_builder;

  id = g_strdup_printf ("%p", mount);
  name = g_mount_get_name (mount);
  icon = g_mount_get_icon (mount);
  if (icon)
    icon_data = g_icon_to_string (icon);
  else
    icon_data = g_strdup ("");
  symbolic_icon = g_mount_get_symbolic_icon (mount);
  if (symbolic_icon)
    symbolic_icon_data = g_icon_to_string (symbolic_icon);
  else
    symbolic_icon_data = g_strdup ("");
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

  sort_key = g_mount_get_sort_key (mount);
  if (sort_key == NULL)
    sort_key = "";

  x_content_types_array_builder = g_variant_builder_new (G_VARIANT_TYPE ("as"));
  x_content_types = (char **) g_object_get_data (G_OBJECT (mount), "x-content-types");
  if (x_content_types != NULL)
    {
      for (n = 0; x_content_types[n] != NULL; n++)
        g_variant_builder_add (x_content_types_array_builder, "s", x_content_types[n]);
    }

  expansion_builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);
  /* left for future expansion without ABI breaks */

  result = g_variant_new (MOUNT_STRUCT_TYPE,
                          id,
                          name,
                          icon_data,
                          symbolic_icon_data,
                          uuid,
                          root_uri,
                          can_unmount,
                          volume_id,
                          x_content_types_array_builder,
                          sort_key,
                          expansion_builder);

  g_variant_builder_unref (x_content_types_array_builder);
  g_variant_builder_unref (expansion_builder);

  g_free (volume_id);
  if (volume != NULL)
    g_object_unref (volume);
  g_free (root_uri);
  g_object_unref (root);
  g_free (uuid);
  g_free (icon_data);
  g_object_unref (icon);
  g_free (symbolic_icon_data);
  g_object_unref (symbolic_icon);
  g_free (name);
  g_free (id);

  return result;
}

static gboolean
handle_list (GVfsRemoteVolumeMonitor *object,
             GDBusMethodInvocation *invocation,
             gpointer user_data)
{
  GList *drives;
  GList *volumes;
  GList *mounts;
  GList *l;
  GVariantBuilder *drives_array;
  GVariantBuilder *volumes_array;
  GVariantBuilder *mounts_array;

  print_debug ("in handle_list");

  drives = g_volume_monitor_get_connected_drives (monitor);
  volumes = g_volume_monitor_get_volumes (monitor);
  mounts = g_volume_monitor_get_mounts (monitor);

  drives_array = g_variant_builder_new (G_VARIANT_TYPE ("a" DRIVE_STRUCT_TYPE));
  for (l = drives; l; l = l->next)
    g_variant_builder_add_value (drives_array, drive_to_dbus (l->data));
  volumes_array = g_variant_builder_new (G_VARIANT_TYPE ("a" VOLUME_STRUCT_TYPE));
  for (l = volumes; l; l = l->next)
    g_variant_builder_add_value (volumes_array, volume_to_dbus (l->data));
  mounts_array = g_variant_builder_new (G_VARIANT_TYPE ("a" MOUNT_STRUCT_TYPE));
  for (l = mounts; l; l = l->next)
    g_variant_builder_add_value (mounts_array, mount_to_dbus (l->data));

  g_list_free_full (drives, g_object_unref);
  g_list_free_full (volumes, g_object_unref);
  g_list_free_full (mounts, g_object_unref);
  
  gvfs_remote_volume_monitor_complete_list (object, invocation,
                                            g_variant_builder_end (drives_array),
                                            g_variant_builder_end (volumes_array),
                                            g_variant_builder_end (mounts_array));
  
  g_variant_builder_unref (drives_array);
  g_variant_builder_unref (volumes_array);
  g_variant_builder_unref (mounts_array);
  
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
mount_unmount_cb (GMount *mount, GAsyncResult *result, GDBusMethodInvocation *invocation)
{
  GError *error;

  print_debug ("in mount_unmount_cb");

  g_object_set_data (G_OBJECT (mount), "cancellable", NULL);
  g_object_set_data (G_OBJECT (mount), "mount_operation", NULL);

  error = NULL;
  if (!g_mount_unmount_with_operation_finish (mount, result, &error))
    {
      print_debug ("  error: %s", error->message);
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
    }
  else
    {
      print_debug (" success");
      gvfs_remote_volume_monitor_complete_mount_unmount (NULL, invocation);
    }

  g_object_unref (mount);
  g_object_unref (invocation);
}

static gboolean
handle_mount_unmount (GVfsRemoteVolumeMonitor *object,
                      GDBusMethodInvocation *invocation,
                      const gchar *arg_id,
                      const gchar *arg_cancellation_id,
                      guint arg_unmount_flags,
                      const gchar *arg_mount_op_id,
                      gpointer user_data)
{
  const char *sender;
  GCancellable *cancellable;
  GMountOperation *mount_operation;
  GList *mounts, *l;
  GMount *mount;

  print_debug ("in handle_mount_unmount");

  sender = g_dbus_method_invocation_get_sender (invocation);

  mount = NULL;
  mounts = g_volume_monitor_get_mounts (monitor);
  for (l = mounts; l != NULL; l = l->next)
    {
      char *mount_id;

      mount = G_MOUNT (l->data);
      mount_id = g_strdup_printf ("%p", mount);
      if (strcmp (mount_id, arg_id) == 0)
        break;

      g_free (mount_id);
    }
  if (l == NULL)
    mount = NULL;

  if (mount == NULL)
    {
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.gtk.Private.RemoteVolumeMonitor.NotFound",
                                                  _("The given mount was not found"));
      goto out;
    }

  if (g_object_get_data (G_OBJECT (mount), "cancellable") != NULL)
    {
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.gtk.Private.RemoteVolumeMonitor.Failed",
                                                  _("An operation is already pending"));
      goto out;
    }

  cancellable = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (mount), "cancellable", cancellable, g_object_unref);
  g_object_set_data_full (G_OBJECT (cancellable), "owner", g_strdup (sender), g_free);
  g_object_set_data_full (G_OBJECT (cancellable), "cancellation_id", g_strdup (arg_cancellation_id), g_free);
  outstanding_ops = g_list_prepend (outstanding_ops, cancellable);
  g_object_weak_ref (G_OBJECT (cancellable),
                     cancellable_destroyed_cb,
                     NULL);

  mount_operation = NULL;
  if (arg_mount_op_id != NULL && strlen (arg_mount_op_id) > 0)
    {
      mount_operation = wrap_mount_op (arg_mount_op_id, sender, object);
      g_object_set_data_full (G_OBJECT (mount), "mount_operation", mount_operation, g_object_unref);
    }

  g_object_ref (mount);
  g_mount_unmount_with_operation (mount,
                                  arg_unmount_flags,
                                  mount_operation,
                                  cancellable,
                                  (GAsyncReadyCallback) mount_unmount_cb,
                                  g_object_ref (invocation));

 out:
  if (mounts != NULL)
    g_list_free_full (mounts, g_object_unref);
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_mount_op_reply2 (GVfsRemoteVolumeMonitor *object,
                        GDBusMethodInvocation *invocation,
                        const gchar *arg_mount_op_id,
                        gint arg_result,
                        const gchar *arg_user_name,
                        const gchar *arg_domain,
                        const gchar *arg_encoded_password,
                        gint arg_password_save,
                        gint arg_choice,
                        gboolean arg_anonymous,
                        GVariant *expansion,
                        gpointer user_data)
{
  char *decoded_password;
  gsize decoded_password_len;
  GList *l;
  GMountOperation *mount_operation;
  const gchar *sender;
  GVariantIter *iter_expansion;
  GVariant *value;
  const gchar *key;

  print_debug ("in handle_mount_op_reply2");

  decoded_password = NULL;
  sender = g_dbus_method_invocation_get_sender (invocation);

  /* Find the op */
  mount_operation = NULL;

  for (l = outstanding_mount_op_objects; l != NULL; l = l->next)
    {
      GMountOperation *op = G_MOUNT_OPERATION (l->data);
      const gchar *owner;
      const gchar *id;

      owner = g_object_get_data (G_OBJECT (op), "mount_op_owner");
      id = g_object_get_data (G_OBJECT (op), "mount_op_id");
      if (g_strcmp0 (owner, sender) == 0 && g_strcmp0 (id, arg_mount_op_id) == 0)
        {
          print_debug ("found mount_op");
          mount_operation = op;
          break;
        }
    }

  if (mount_operation == NULL)
    {
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.gtk.Private.RemoteVolumeMonitor.NotFound",
                                                  _("No outstanding mount operation"));
      goto out;
    }

  decoded_password = (gchar *) g_base64_decode (arg_encoded_password, &decoded_password_len);

  g_mount_operation_set_username (mount_operation, arg_user_name);
  g_mount_operation_set_domain (mount_operation, arg_domain);
  g_mount_operation_set_password (mount_operation, decoded_password);
  g_mount_operation_set_password_save (mount_operation, arg_password_save);
  g_mount_operation_set_choice (mount_operation, arg_choice);
  g_mount_operation_set_anonymous (mount_operation, arg_anonymous);

  g_variant_get (expansion, "a{sv}", &iter_expansion);
  while (g_variant_iter_loop (iter_expansion, "{sv}", &key, &value))
    {
      if (g_str_equal (key, "hidden-volume"))
        g_mount_operation_set_is_tcrypt_hidden_volume (mount_operation, g_variant_get_boolean (value));
      else if (g_str_equal (key, "system-volume"))
        g_mount_operation_set_is_tcrypt_system_volume (mount_operation, g_variant_get_boolean (value));
      else if (g_str_equal (key, "pim"))
        g_mount_operation_set_pim (mount_operation, g_variant_get_uint32 (value));
      else
        g_warning ("Unsupported GMountOperation option: %s\n", key);
    }
  g_variant_iter_free (iter_expansion);

  g_mount_operation_reply (mount_operation, arg_result);

  /* gvfs_remote_volume_monitor_complete_mount_op_reply2 should be
   * identical to gvfs_remote_volume_monitor_complete_mount_op_reply,
   * so it should be ok that we call this from handle_mount_op_reply too.
   */
  gvfs_remote_volume_monitor_complete_mount_op_reply2 (object, invocation);

 out:
  g_free (decoded_password);
  return TRUE;
}

static gboolean
handle_mount_op_reply (GVfsRemoteVolumeMonitor *object,
                       GDBusMethodInvocation *invocation,
                       const gchar *arg_mount_op_id,
                       gint arg_result,
                       const gchar *arg_user_name,
                       const gchar *arg_domain,
                       const gchar *arg_encoded_password,
                       gint arg_password_save,
                       gint arg_choice,
                       gboolean arg_anonymous,
                       gpointer user_data)
{
  GVariantBuilder *expansion_builder;
  gboolean ret;

  expansion_builder = g_variant_builder_new (G_VARIANT_TYPE_VARDICT);

  ret = handle_mount_op_reply2 (object,
                                invocation,
                                arg_mount_op_id,
                                arg_result,
                                arg_user_name,
                                arg_domain,
                                arg_encoded_password,
                                arg_password_save,
                                arg_choice,
                                arg_anonymous,
                                g_variant_new ("a{sv}", expansion_builder),
                                user_data);

  g_variant_builder_unref (expansion_builder);

  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
volume_mount_cb (GVolume *volume, GAsyncResult *result, GDBusMethodInvocation *invocation)
{
  GError *error;

  print_debug ("in volume_mount_cb");

  g_object_set_data (G_OBJECT (volume), "mount_operation", NULL);
  g_object_set_data (G_OBJECT (volume), "cancellable", NULL);

  error = NULL;
  if (!g_volume_mount_finish (volume, result, &error))
    {
      print_debug ("  error: %s", error->message);
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
    }
  else
    {
      print_debug (" success");
      gvfs_remote_volume_monitor_complete_volume_mount (NULL, invocation);      
    }
  g_object_unref (invocation);
}

static gboolean
handle_volume_mount (GVfsRemoteVolumeMonitor *object,
                     GDBusMethodInvocation *invocation,
                     const gchar *arg_id,
                     const gchar *arg_cancellation_id,
                     guint arg_mount_flags,
                     const gchar *arg_mount_op_id,
                     gpointer user_data)
{
  const char *sender;
  GList *volumes, *l;
  GVolume *volume;
  GMountOperation *mount_operation;
  GCancellable *cancellable;

  print_debug ("in handle_volume_mount");

  sender = g_dbus_method_invocation_get_sender (invocation);

  volume = NULL;
  volumes = g_volume_monitor_get_volumes (monitor);
  for (l = volumes; l != NULL; l = l->next)
    {
      char *volume_id;

      volume = G_VOLUME (l->data);
      volume_id = g_strdup_printf ("%p", volume);
      if (strcmp (volume_id, arg_id) == 0)
        break;

      g_free (volume_id);
    }
  if (l == NULL)
    volume = NULL;

  if (volume == NULL)
    {
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.gtk.Private.RemoteVolumeMonitor.NotFound",
                                                  _("The given volume was not found"));
      goto out;
    }

  if (g_object_get_data (G_OBJECT (volume), "cancellable") != NULL)
    {
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.gtk.Private.RemoteVolumeMonitor.Failed",
                                                  _("An operation is already pending"));
      goto out;
    }

  mount_operation = NULL;
  if (arg_mount_op_id != NULL && strlen (arg_mount_op_id) > 0)
    {
      mount_operation = wrap_mount_op (arg_mount_op_id, sender, object);
      g_object_set_data_full (G_OBJECT (volume), "mount_operation", mount_operation, g_object_unref);
    }

  cancellable = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (volume), "cancellable", cancellable, g_object_unref);
  g_object_set_data_full (G_OBJECT (cancellable), "owner", g_strdup (sender), g_free);
  g_object_set_data_full (G_OBJECT (cancellable), "cancellation_id", g_strdup (arg_cancellation_id), g_free);
  outstanding_ops = g_list_prepend (outstanding_ops, cancellable);
  g_object_weak_ref (G_OBJECT (cancellable),
                     cancellable_destroyed_cb,
                     NULL);

  g_volume_mount (volume,
                  arg_mount_flags,
                  mount_operation,
                  cancellable,
                  (GAsyncReadyCallback) volume_mount_cb,
                  g_object_ref (invocation));

 out:
  if (volumes != NULL)
    g_list_free_full (volumes, g_object_unref);
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
drive_eject_cb (GDrive *drive, GAsyncResult *result, GDBusMethodInvocation *invocation)
{
  GError *error;

  print_debug ("in drive_eject_cb");

  g_object_set_data (G_OBJECT (drive), "cancellable", NULL);
  g_object_set_data (G_OBJECT (drive), "mount_operation", NULL);

  error = NULL;
  if (!g_drive_eject_with_operation_finish (drive, result, &error))
    {
      print_debug ("  error: %s", error->message);
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
    }
  else
    {
      print_debug (" success");
      gvfs_remote_volume_monitor_complete_drive_eject (NULL, invocation);
    }
  g_object_unref (invocation);
}

static gboolean
handle_drive_eject (GVfsRemoteVolumeMonitor *object,
                    GDBusMethodInvocation *invocation,
                    const gchar *arg_id,
                    const gchar *arg_cancellation_id,
                    guint arg_unmount_flags,
                    const gchar *arg_mount_op_id,
                    gpointer user_data)
{
  const char *sender;
  GMountOperation *mount_operation;
  GCancellable *cancellable;
  GList *drives, *l;
  GDrive *drive;

  print_debug ("in handle_drive_eject");

  sender = g_dbus_method_invocation_get_sender (invocation);

  drive = NULL;
  drives = g_volume_monitor_get_connected_drives (monitor);
  for (l = drives; l != NULL; l = l->next)
    {
      char *drive_id;

      drive = G_DRIVE (l->data);
      drive_id = g_strdup_printf ("%p", drive);
      if (strcmp (drive_id, arg_id) == 0)
        break;

      g_free (drive_id);
    }
  if (l == NULL)
    drive = NULL;

  if (drive == NULL)
    {
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.gtk.Private.RemoteVolumeMonitor.NotFound",
                                                  _("The given drive was not found"));
      goto out;
    }

  if (g_object_get_data (G_OBJECT (drive), "cancellable") != NULL)
    {
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.gtk.Private.RemoteVolumeMonitor.Failed",
                                                  _("An operation is already pending"));
      goto out;
    }

  cancellable = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (drive), "cancellable", cancellable, g_object_unref);
  g_object_set_data_full (G_OBJECT (cancellable), "owner", g_strdup (sender), g_free);
  g_object_set_data_full (G_OBJECT (cancellable), "cancellation_id", g_strdup (arg_cancellation_id), g_free);
  outstanding_ops = g_list_prepend (outstanding_ops, cancellable);
  g_object_weak_ref (G_OBJECT (cancellable),
                     cancellable_destroyed_cb,
                     NULL);

  mount_operation = NULL;
  if (arg_mount_op_id != NULL && strlen (arg_mount_op_id) > 0)
    {
      mount_operation = wrap_mount_op (arg_mount_op_id, sender, object);
      g_object_set_data_full (G_OBJECT (drive), "mount_operation", mount_operation, g_object_unref);
    }

  g_drive_eject_with_operation (drive,
                                arg_unmount_flags,
                                mount_operation,
                                cancellable,
                                (GAsyncReadyCallback) drive_eject_cb,
                                g_object_ref (invocation));

 out:
  if (drives != NULL)
    g_list_free_full (drives, g_object_unref);
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
drive_stop_cb (GDrive *drive, GAsyncResult *result, GDBusMethodInvocation *invocation)
{
  GError *error;

  print_debug ("in drive_stop_cb");

  g_object_set_data (G_OBJECT (drive), "cancellable", NULL);
  g_object_set_data (G_OBJECT (drive), "mount_operation", NULL);

  error = NULL;
  if (!g_drive_stop_finish (drive, result, &error))
    {
      print_debug ("  error: %s", error->message);
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
    }
  else
    {
      print_debug (" success");
      gvfs_remote_volume_monitor_complete_drive_stop (NULL, invocation);
    }
  g_object_unref (invocation);
}

static gboolean
handle_drive_stop (GVfsRemoteVolumeMonitor *object,
                   GDBusMethodInvocation *invocation,
                   const gchar *arg_id,
                   const gchar *arg_cancellation_id,
                   guint arg_unmount_flags,
                   const gchar *arg_mount_op_id,
                   gpointer user_data)
{
  const char *sender;
  GMountOperation *mount_operation;
  GCancellable *cancellable;
  GList *drives, *l;
  GDrive *drive;

  print_debug ("in handle_drive_stop");

  sender = g_dbus_method_invocation_get_sender (invocation);

  drive = NULL;
  drives = g_volume_monitor_get_connected_drives (monitor);
  for (l = drives; l != NULL; l = l->next)
    {
      char *drive_id;

      drive = G_DRIVE (l->data);
      drive_id = g_strdup_printf ("%p", drive);
      if (strcmp (drive_id, arg_id) == 0)
        break;

      g_free (drive_id);
    }
  if (l == NULL)
    drive = NULL;

  if (drive == NULL)
    {
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.gtk.Private.RemoteVolumeMonitor.NotFound",
                                                  _("The given drive was not found"));
      goto out;
    }

  if (g_object_get_data (G_OBJECT (drive), "cancellable") != NULL)
    {
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.gtk.Private.RemoteVolumeMonitor.Failed",
                                                  _("An operation is already pending"));
      goto out;
    }

  cancellable = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (drive), "cancellable", cancellable, g_object_unref);
  g_object_set_data_full (G_OBJECT (cancellable), "owner", g_strdup (sender), g_free);
  g_object_set_data_full (G_OBJECT (cancellable), "cancellation_id", g_strdup (arg_cancellation_id), g_free);
  outstanding_ops = g_list_prepend (outstanding_ops, cancellable);
  g_object_weak_ref (G_OBJECT (cancellable),
                     cancellable_destroyed_cb,
                     NULL);

  mount_operation = NULL;
  if (arg_mount_op_id != NULL && strlen (arg_mount_op_id) > 0)
    {
      mount_operation = wrap_mount_op (arg_mount_op_id, sender, object);
      g_object_set_data_full (G_OBJECT (drive), "mount_operation", mount_operation, g_object_unref);
    }

  g_drive_stop (drive,
                arg_unmount_flags,
                mount_operation,
                cancellable,
                (GAsyncReadyCallback) drive_stop_cb,
                g_object_ref (invocation));

 out:
  if (drives != NULL)
    g_list_free_full (drives, g_object_unref);
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
drive_start_cb (GDrive *drive, GAsyncResult *result, GDBusMethodInvocation *invocation)
{
  GError *error;

  print_debug ("in drive_start_cb");

  g_object_set_data (G_OBJECT (drive), "mount_operation", NULL);
  g_object_set_data (G_OBJECT (drive), "cancellable", NULL);

  error = NULL;
  if (!g_drive_start_finish (drive, result, &error))
    {
      print_debug ("  error: %s", error->message);
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
    }
  else
    {
      print_debug (" success");
      gvfs_remote_volume_monitor_complete_drive_start (NULL, invocation);
    }
  g_object_unref (invocation);
}

static gboolean
handle_drive_start (GVfsRemoteVolumeMonitor *object,
                    GDBusMethodInvocation *invocation,
                    const gchar *arg_id,
                    const gchar *arg_cancellation_id,
                    guint arg_flags,
                    const gchar *arg_mount_op_id,
                    gpointer user_data)
{
  const char *sender;
  GList *drives, *l;
  GDrive *drive;
  GMountOperation *mount_operation;
  GCancellable *cancellable;

  print_debug ("in handle_drive_start");

  sender = g_dbus_method_invocation_get_sender (invocation);

  drive = NULL;
  drives = g_volume_monitor_get_connected_drives (monitor);
  for (l = drives; l != NULL; l = l->next)
    {
      char *drive_id;

      drive = G_DRIVE (l->data);
      drive_id = g_strdup_printf ("%p", drive);
      if (strcmp (drive_id, arg_id) == 0)
        break;

      g_free (drive_id);
    }
  if (l == NULL)
    drive = NULL;

  if (drive == NULL)
    {
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.gtk.Private.RemoteVolumeMonitor.NotFound",
                                                  _("The given drive was not found"));
      goto out;
    }

  if (g_object_get_data (G_OBJECT (drive), "cancellable") != NULL)
    {
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.gtk.Private.RemoteVolumeMonitor.Failed",
                                                  _("An operation is already pending"));
      goto out;
    }

  mount_operation = NULL;
  if (arg_mount_op_id != NULL && strlen (arg_mount_op_id) > 0)
    {
      mount_operation = wrap_mount_op (arg_mount_op_id, sender, object);
      g_object_set_data_full (G_OBJECT (drive), "mount_operation", mount_operation, g_object_unref);
    }

  cancellable = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (drive), "cancellable", cancellable, g_object_unref);
  g_object_set_data_full (G_OBJECT (cancellable), "owner", g_strdup (sender), g_free);
  g_object_set_data_full (G_OBJECT (cancellable), "cancellation_id", g_strdup (arg_cancellation_id), g_free);
  outstanding_ops = g_list_prepend (outstanding_ops, cancellable);
  g_object_weak_ref (G_OBJECT (cancellable),
                     cancellable_destroyed_cb,
                     NULL);

  g_drive_start (drive,
                 arg_flags,
                 mount_operation,
                 cancellable,
                 (GAsyncReadyCallback) drive_start_cb,
                 g_object_ref (invocation));

 out:
  if (drives != NULL)
    g_list_free_full (drives, g_object_unref);
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
drive_poll_for_media_cb (GDrive *drive, GAsyncResult *result, GDBusMethodInvocation *invocation)
{
  GError *error;

  print_debug ("in drive_poll_for_media_cb");

  g_object_set_data (G_OBJECT (drive), "cancellable", NULL);

  error = NULL;
  if (!g_drive_poll_for_media_finish (drive, result, &error))
    {
      print_debug ("  error: %s", error->message);
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
    }
  else
    {
      print_debug (" success");
      gvfs_remote_volume_monitor_complete_drive_poll_for_media (NULL, invocation);
    }
  g_object_unref (invocation);
}

static gboolean
handle_drive_poll_for_media (GVfsRemoteVolumeMonitor *object,
                             GDBusMethodInvocation *invocation,
                             const gchar *arg_id,
                             const gchar *arg_cancellation_id,
                             gpointer user_data)
{
  const char *sender;
  GCancellable *cancellable;
  GList *drives, *l;
  GDrive *drive;

  print_debug ("in handle_drive_poll_for_media");

  sender = g_dbus_method_invocation_get_sender (invocation);

  drive = NULL;
  drives = g_volume_monitor_get_connected_drives (monitor);
  for (l = drives; l != NULL; l = l->next)
    {
      char *drive_id;

      drive = G_DRIVE (l->data);
      drive_id = g_strdup_printf ("%p", drive);
      if (strcmp (drive_id, arg_id) == 0)
        break;

      g_free (drive_id);
    }
  if (l == NULL)
    drive = NULL;

  if (drive == NULL)
    {
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.gtk.Private.RemoteVolumeMonitor.NotFound",
                                                  _("The given drive was not found"));
      goto out;
    }

  if (g_object_get_data (G_OBJECT (drive), "cancellable") != NULL)
    {
      g_dbus_method_invocation_return_dbus_error (invocation,
                                                  "org.gtk.Private.RemoteVolumeMonitor.Failed",
                                                  _("An operation is already pending"));
      goto out;
    }

  cancellable = g_cancellable_new ();
  g_object_set_data_full (G_OBJECT (drive), "cancellable", cancellable, g_object_unref);
  g_object_set_data_full (G_OBJECT (cancellable), "owner", g_strdup (sender), g_free);
  g_object_set_data_full (G_OBJECT (cancellable), "cancellation_id", g_strdup (arg_cancellation_id), g_free);
  outstanding_ops = g_list_prepend (outstanding_ops, cancellable);
  g_object_weak_ref (G_OBJECT (cancellable),
                     cancellable_destroyed_cb,
                     NULL);

  g_drive_poll_for_media (drive,
                          cancellable,
                          (GAsyncReadyCallback) drive_poll_for_media_cb,
                          g_object_ref (invocation));
  
 out:
  if (drives != NULL)
    g_list_free_full (drives, g_object_unref);
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_is_supported (GVfsRemoteVolumeMonitor *object,
                     GDBusMethodInvocation *invocation,
                     gpointer user_data)
{
  print_debug ("in handle_supported");

  /* if monitor wasn't created on startup; try again */
  if (monitor == NULL)
    monitor_try_create ();

  if (monitor != NULL)
    {
      /* If someone is calling into this object and interface, start watching their name so
       * we can cancel operations initiated by them when they disconnect
       */
      ensure_name_owner_changed_for_unique_name (invocation);
    }
  
  gvfs_remote_volume_monitor_complete_is_supported (object, invocation,
                                                    monitor != NULL);
  
  return TRUE; /* invocation was handled */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_cancel_operation (GVfsRemoteVolumeMonitor *object,
                         GDBusMethodInvocation *invocation,
                         const gchar *arg_cancellation_id,
                         gpointer user_data)
{
  gboolean was_cancelled;
  const char *sender;
  GList *l;

  was_cancelled = FALSE;

  sender = g_dbus_method_invocation_get_sender (invocation);

  print_debug ("in handle_cancel_operation");

  /* Find GCancellable to cancel */
  for (l = outstanding_ops; l != NULL; l = l->next)
    {
      GCancellable *cancellable = G_CANCELLABLE (l->data);
      const gchar *owner;
      const gchar *id;

      owner = g_object_get_data (G_OBJECT (cancellable), "owner");
      id = g_object_get_data (G_OBJECT (cancellable), "cancellation_id");
      if (g_strcmp0 (owner, sender) == 0 && g_strcmp0 (id, arg_cancellation_id) == 0)
        {
          print_debug ("found op to cancel");
          g_cancellable_cancel (cancellable);

          was_cancelled = TRUE;
          break;
        }
    }

  if (!was_cancelled)
    g_warning ("didn't find op to cancel");

  gvfs_remote_volume_monitor_complete_cancel_operation (object, invocation, was_cancelled);

  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef GVariant * (*MonitorDBusFunc) (void *object);
typedef void (*MonitorSignalFunc) (GVfsRemoteVolumeMonitor *object, 
                                   const gchar *arg_dbus_name,
                                   const gchar *arg_id,
                                   GVariant *val);

static void
emit_signal (GVfsRemoteVolumeMonitor *instance, MonitorSignalFunc signal_func, void *object, MonitorDBusFunc func)
{
  char *id;
  GVariant *val;
  
  print_debug ("emit_signal: %p", object);
  
  id = g_strdup_printf ("%p", object);
  val = func (object);

  signal_func (instance, the_dbus_name, id, val);

  g_free (id);
}

static void
drive_changed (GVolumeMonitor *monitor, GDrive *drive, GVfsRemoteVolumeMonitor *instance)
{
  emit_signal (instance, (MonitorSignalFunc) gvfs_remote_volume_monitor_emit_drive_changed, drive, (MonitorDBusFunc) drive_to_dbus);
}

static void
drive_connected (GVolumeMonitor *monitor, GDrive *drive, GVfsRemoteVolumeMonitor *instance)
{
  emit_signal (instance, (MonitorSignalFunc) gvfs_remote_volume_monitor_emit_drive_connected, drive, (MonitorDBusFunc) drive_to_dbus);
}

static void
drive_disconnected (GVolumeMonitor *monitor, GDrive *drive, GVfsRemoteVolumeMonitor *instance)
{
  emit_signal (instance, (MonitorSignalFunc) gvfs_remote_volume_monitor_emit_drive_disconnected, drive, (MonitorDBusFunc) drive_to_dbus);
}

static void
drive_eject_button (GVolumeMonitor *monitor, GDrive *drive, GVfsRemoteVolumeMonitor *instance)
{
  emit_signal (instance, (MonitorSignalFunc) gvfs_remote_volume_monitor_emit_drive_eject_button, drive, (MonitorDBusFunc) drive_to_dbus);
}

static void
drive_stop_button (GVolumeMonitor *monitor, GDrive *drive, GVfsRemoteVolumeMonitor *instance)
{
  emit_signal (instance, (MonitorSignalFunc) gvfs_remote_volume_monitor_emit_drive_stop_button, drive, (MonitorDBusFunc) drive_to_dbus);
}

static void
volume_changed (GVolumeMonitor *monitor, GVolume *volume, GVfsRemoteVolumeMonitor *instance)
{
  emit_signal (instance, (MonitorSignalFunc) gvfs_remote_volume_monitor_emit_volume_changed, volume, (MonitorDBusFunc) volume_to_dbus);
}

static void
volume_added (GVolumeMonitor *monitor, GVolume *volume, GVfsRemoteVolumeMonitor *instance)
{
  emit_signal (instance, (MonitorSignalFunc) gvfs_remote_volume_monitor_emit_volume_added, volume, (MonitorDBusFunc) volume_to_dbus);
}

static void
volume_removed (GVolumeMonitor *monitor, GVolume *volume, GVfsRemoteVolumeMonitor *instance)
{
  emit_signal (instance, (MonitorSignalFunc) gvfs_remote_volume_monitor_emit_volume_removed, volume, (MonitorDBusFunc) volume_to_dbus);
}

static void
mount_changed (GVolumeMonitor *monitor, GMount *mount, GVfsRemoteVolumeMonitor *instance)
{
  emit_signal (instance, (MonitorSignalFunc) gvfs_remote_volume_monitor_emit_mount_changed, mount, (MonitorDBusFunc) mount_to_dbus);
}

static void
mount_sniff_x_content_type (GMount *mount)
{
  char **x_content_types;
  x_content_types = g_mount_guess_content_type_sync (mount, TRUE, NULL, NULL);
  g_object_set_data_full (G_OBJECT (mount), "x-content-types", x_content_types, (GDestroyNotify) g_strfreev);
}

static void
mount_added (GVolumeMonitor *monitor, GMount *mount, GVfsRemoteVolumeMonitor *instance)
{
  mount_sniff_x_content_type (mount);
  emit_signal (instance, (MonitorSignalFunc) gvfs_remote_volume_monitor_emit_mount_added, mount, (MonitorDBusFunc) mount_to_dbus);
}

static void
mount_pre_unmount (GVolumeMonitor *monitor, GMount *mount, GVfsRemoteVolumeMonitor *instance)
{
  emit_signal (instance, (MonitorSignalFunc) gvfs_remote_volume_monitor_emit_mount_pre_unmount, mount, (MonitorDBusFunc) mount_to_dbus);
}

static void
mount_removed (GVolumeMonitor *monitor, GMount *mount, GVfsRemoteVolumeMonitor *instance)
{
  emit_signal (instance, (MonitorSignalFunc) gvfs_remote_volume_monitor_emit_mount_removed, mount, (MonitorDBusFunc) mount_to_dbus);
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
  g_list_free_full (mounts, g_object_unref);

 fail:
  if (klass != NULL)
    g_type_class_unref (klass);
}

static void
bus_acquired_handler_cb (GDBusConnection *conn,
                         const gchar *name,
                         gpointer user_data)
{
  GError *error;
  
  if (! conn)
    return;

  monitor_daemon = gvfs_remote_volume_monitor_skeleton_new ();
  
  g_signal_connect (monitor_daemon, "handle-is-supported", G_CALLBACK (handle_is_supported), NULL);
  if (monitor != NULL)
    {
      g_signal_connect (monitor_daemon, "handle-list", G_CALLBACK (handle_list), NULL);
      g_signal_connect (monitor_daemon, "handle-cancel-operation", G_CALLBACK (handle_cancel_operation), NULL);
      g_signal_connect (monitor_daemon, "handle-drive-eject", G_CALLBACK (handle_drive_eject), NULL);
      g_signal_connect (monitor_daemon, "handle-drive-poll-for-media", G_CALLBACK (handle_drive_poll_for_media), NULL);
      g_signal_connect (monitor_daemon, "handle-drive-start", G_CALLBACK (handle_drive_start), NULL);
      g_signal_connect (monitor_daemon, "handle-drive-stop", G_CALLBACK (handle_drive_stop), NULL);
      g_signal_connect (monitor_daemon, "handle-mount-op-reply", G_CALLBACK (handle_mount_op_reply), NULL);
      g_signal_connect (monitor_daemon, "handle-mount-op-reply2", G_CALLBACK (handle_mount_op_reply2), NULL);
      g_signal_connect (monitor_daemon, "handle-mount-unmount", G_CALLBACK (handle_mount_unmount), NULL);
      g_signal_connect (monitor_daemon, "handle-volume-mount", G_CALLBACK (handle_volume_mount), NULL);
    }

  /*  This way we open our d-bus API to public, though there's the "Private" path element  */
  error = NULL;
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (monitor_daemon), conn,
                                         "/org/gtk/Private/RemoteVolumeMonitor", &error))
    {
      g_printerr ("Error exporting volume monitor: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
    }


  if (monitor != NULL)
    {
      g_signal_connect (monitor, "drive-changed", (GCallback) drive_changed, monitor_daemon);
      g_signal_connect (monitor, "drive-connected", (GCallback) drive_connected, monitor_daemon);
      g_signal_connect (monitor, "drive-disconnected", (GCallback) drive_disconnected, monitor_daemon);
      g_signal_connect (monitor, "drive-eject-button", (GCallback) drive_eject_button, monitor_daemon);
      g_signal_connect (monitor, "drive-stop-button", (GCallback) drive_stop_button, monitor_daemon);

      g_signal_connect (monitor, "volume-changed", (GCallback) volume_changed, monitor_daemon);
      g_signal_connect (monitor, "volume-added", (GCallback) volume_added, monitor_daemon);
      g_signal_connect (monitor, "volume-removed", (GCallback) volume_removed, monitor_daemon);

      g_signal_connect (monitor, "mount-changed", (GCallback) mount_changed, monitor_daemon);
      g_signal_connect (monitor, "mount-added", (GCallback) mount_added, monitor_daemon);
      g_signal_connect (monitor, "mount-pre-unmount", (GCallback) mount_pre_unmount, monitor_daemon);
      g_signal_connect (monitor, "mount-removed", (GCallback) mount_removed, monitor_daemon);
    }
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  /* means that someone has claimed our name (we allow replacement) */
  g_main_loop_quit (loop);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  /* acquired the name %s on the session message bus */
}

int
g_vfs_proxy_volume_monitor_daemon_main (int argc,
                                        char *argv[],
                                        const char *dbus_name,
                                        GType volume_monitor_type)
{
  guint name_owner_id;

  name_owner_id = 0;

  loop = g_main_loop_new (NULL, FALSE);

  /* need to start up regardless of whether we can instantiate a
   * volume monitor; this is because the proxy will need to be able to
   * call IsSupported() on our D-Bus interface.
   */

  the_volume_monitor_type = volume_monitor_type;
  the_dbus_name = dbus_name;
  unique_names_being_watched = g_hash_table_new_full (g_str_hash, g_int_equal, g_free, NULL);

  /* try and create the monitor */
  monitor_try_create ();

  name_owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                  dbus_name,
                                  G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
                                  G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                  bus_acquired_handler_cb,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);
  g_main_loop_run (loop);

  if (name_owner_id != 0)
    g_bus_unown_name (name_owner_id);
  if (loop != NULL)
    g_main_loop_unref (loop);
  if (unique_names_being_watched)
    g_hash_table_unref (unique_names_being_watched);

  return 0;
}

void
g_vfs_proxy_volume_monitor_daemon_set_always_call_mount (gboolean value)
{
  always_call_mount = value;
}
