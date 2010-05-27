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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <gvfsdbusutils.h>

#include "gproxymountoperation.h"

/* for protecting the id_to_op and id_count */
G_LOCK_DEFINE_STATIC(proxy_op);

/* map from id to GMountOperation */
static GHashTable *id_to_op = NULL;

static guint id_count = 1;

typedef struct
{
  gchar *id;
  GMountOperation *op;
  GProxyVolumeMonitor *monitor;
  gulong reply_handler_id;
} ProxyMountOpData;

static void
proxy_mount_op_data_free (ProxyMountOpData *data)
{
  g_free (data->id);
  if (data->reply_handler_id > 0)
    g_signal_handler_disconnect (data->op, data->reply_handler_id);
  g_object_unref (data->op);
  g_object_unref (data->monitor);
  g_free (data);
}

/* must be called with lock held */
static ProxyMountOpData *
proxy_mount_op_data_new (GMountOperation *op,
                         GProxyVolumeMonitor *monitor)
{
  ProxyMountOpData *data;

  data = g_new0 (ProxyMountOpData, 1);
  data->id = g_strdup_printf ("%d:%d", getpid (), id_count++);
  data->op = g_object_ref (op);
  data->monitor = g_object_ref (monitor);
  return data;
}

/* must be called with lock held */
static void
ensure_hash (void)
{
  if (id_to_op == NULL)
    id_to_op = g_hash_table_new_full (g_str_hash,
                                      g_str_equal,
                                      NULL,
                                      (GDestroyNotify) proxy_mount_op_data_free);
}

const gchar *
g_proxy_mount_operation_wrap (GMountOperation *op,
                              GProxyVolumeMonitor *monitor)
{
  ProxyMountOpData *data;

  if (op == NULL)
    return "";

  G_LOCK (proxy_op);

  ensure_hash ();

  data = proxy_mount_op_data_new (op, monitor);
  g_hash_table_insert (id_to_op,
                       data->id,
                       data);

  G_UNLOCK (proxy_op);

  return data->id;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
mount_op_reply_cb (DBusMessage *reply,
                   GError      *error,
                   gpointer     user_data)
{
  if (error != NULL)
    {
      g_warning ("Error from MountOpReply(): %s", error->message);
    }
}

static void
mount_operation_reply (GMountOperation        *mount_operation,
                       GMountOperationResult  result,
                       gpointer               user_data)
{
  ProxyMountOpData *data = user_data;
  DBusConnection *connection;
  const gchar *name;
  DBusMessage *message;
  const gchar *user_name;
  const gchar *domain;
  const gchar *password;
  gchar *encoded_password;
  dbus_uint32_t password_save;
  dbus_uint32_t choice;
  dbus_bool_t anonymous;

  connection = g_proxy_volume_monitor_get_dbus_connection (data->monitor);
  name = g_proxy_volume_monitor_get_dbus_name (data->monitor);

  user_name     = g_mount_operation_get_username (mount_operation);
  domain        = g_mount_operation_get_domain (mount_operation);
  password      = g_mount_operation_get_password (mount_operation);
  password_save = g_mount_operation_get_password_save (mount_operation);
  choice        = g_mount_operation_get_choice (mount_operation);
  anonymous     = g_mount_operation_get_anonymous (mount_operation);

  if (user_name == NULL)
    user_name = "";
  if (domain == NULL)
    domain = "";
  if (password == NULL)
    password = "";

  /* NOTE: this is not to add "security", it's merely to prevent accidental exposure
   *       of passwords when running dbus-monitor
   */
  encoded_password = g_base64_encode ((const guchar *) password, (gsize) (strlen (password) + 1));

  message = dbus_message_new_method_call (name,
                                          "/org/gtk/Private/RemoteVolumeMonitor",
                                          "org.gtk.Private.RemoteVolumeMonitor",
                                          "MountOpReply");
  dbus_message_append_args (message,
                            DBUS_TYPE_STRING,
                            &(data->id),
                            DBUS_TYPE_INT32,
                            &result,
                            DBUS_TYPE_STRING,
                            &user_name,
                            DBUS_TYPE_STRING,
                            &domain,
                            DBUS_TYPE_STRING,
                            &encoded_password,
                            DBUS_TYPE_INT32,
                            &password_save,
                            DBUS_TYPE_INT32,
                            &choice,
                            DBUS_TYPE_BOOLEAN,
                            &anonymous,
                            DBUS_TYPE_INVALID);

  _g_dbus_connection_call_async (connection,
                                 message,
                                 -1,
                                 (GAsyncDBusCallback) mount_op_reply_cb,
                                 data);

  g_free (encoded_password);
  dbus_message_unref (message);
  dbus_connection_unref (connection);
}

/* ---------------------------------------------------------------------------------------------------- */

void
g_proxy_mount_operation_handle_ask_password (const gchar      *wrapped_id,
                                             DBusMessageIter  *iter)
{
  ProxyMountOpData *data;
  const gchar *message;
  const gchar *default_user;
  const gchar *default_domain;
  dbus_int32_t flags;

  g_return_if_fail (wrapped_id != NULL);
  g_return_if_fail (iter != NULL);

  G_LOCK (proxy_op);
  data = g_hash_table_lookup (id_to_op, wrapped_id);
  G_UNLOCK (proxy_op);

  if (data == NULL)
    {
      g_warning ("%s: No GMountOperation for id `%s'",
                 G_STRFUNC,
                 wrapped_id);
      goto out;
    }

  dbus_message_iter_get_basic (iter, &message);
  dbus_message_iter_next (iter);

  dbus_message_iter_get_basic (iter, &default_user);
  dbus_message_iter_next (iter);

  dbus_message_iter_get_basic (iter, &default_domain);
  dbus_message_iter_next (iter);

  dbus_message_iter_get_basic (iter, &flags);
  dbus_message_iter_next (iter);

  if (data->reply_handler_id == 0)
    {
      data->reply_handler_id = g_signal_connect (data->op,
                                                 "reply",
                                                 G_CALLBACK (mount_operation_reply),
                                                 data);
    }

  g_signal_emit_by_name (data->op,
                         "ask-password",
                         message,
                         default_user,
                         default_domain,
                         flags);

 out:
  ;
}

/* ---------------------------------------------------------------------------------------------------- */

void
g_proxy_mount_operation_handle_ask_question (const gchar      *wrapped_id,
                                             DBusMessageIter  *iter)
{
  ProxyMountOpData *data;
  const gchar *message;
  GPtrArray *choices;
  DBusMessageIter iter_array;

  g_return_if_fail (wrapped_id != NULL);
  g_return_if_fail (iter != NULL);

  choices = NULL;

  G_LOCK (proxy_op);
  data = g_hash_table_lookup (id_to_op, wrapped_id);
  G_UNLOCK (proxy_op);

  if (data == NULL)
    {
      g_warning ("%s: No GMountOperation for id `%s'",
                 G_STRFUNC,
                 wrapped_id);
      goto out;
    }

  dbus_message_iter_get_basic (iter, &message);
  dbus_message_iter_next (iter);

  choices = g_ptr_array_new ();
  dbus_message_iter_recurse (iter, &iter_array);
  while (dbus_message_iter_get_arg_type (&iter_array) != DBUS_TYPE_INVALID)
    {
      const gchar *choice;
      dbus_message_iter_get_basic (&iter_array, &choice);
      dbus_message_iter_next (&iter_array);

      g_ptr_array_add (choices, g_strdup (choice));
    }
  g_ptr_array_add (choices, NULL);

  if (data->reply_handler_id == 0)
    {
      data->reply_handler_id = g_signal_connect (data->op,
                                                 "reply",
                                                 G_CALLBACK (mount_operation_reply),
                                                 data);
    }

  g_signal_emit_by_name (data->op,
                         "ask-question",
                         message,
                         choices->pdata);

 out:
  g_ptr_array_free (choices, TRUE);
}

/* ---------------------------------------------------------------------------------------------------- */

void
g_proxy_mount_operation_handle_show_processes (const gchar      *wrapped_id,
                                               DBusMessageIter  *iter)
{
  ProxyMountOpData *data;
  const gchar *message;
  GPtrArray *choices;
  GArray *processes;
  DBusMessageIter iter_array;

  g_return_if_fail (wrapped_id != NULL);
  g_return_if_fail (iter != NULL);

  choices = NULL;
  processes = NULL;

  G_LOCK (proxy_op);
  data = g_hash_table_lookup (id_to_op, wrapped_id);
  G_UNLOCK (proxy_op);

  if (data == NULL)
    {
      g_warning ("%s: No GMountOperation for id `%s'",
                 G_STRFUNC,
                 wrapped_id);
      goto out;
    }

  dbus_message_iter_get_basic (iter, &message);
  dbus_message_iter_next (iter);

  processes = g_array_new (FALSE, FALSE, sizeof (GPid));
  dbus_message_iter_recurse (iter, &iter_array);
  while (dbus_message_iter_get_arg_type (&iter_array) != DBUS_TYPE_INVALID)
    {
      GPid pid;

      dbus_message_iter_get_basic (&iter_array, &pid);
      dbus_message_iter_next (&iter_array);
      g_array_append_val (processes, pid);
    }

  dbus_message_iter_next (iter);

  choices = g_ptr_array_new ();
  dbus_message_iter_recurse (iter, &iter_array);
  while (dbus_message_iter_get_arg_type (&iter_array) != DBUS_TYPE_INVALID)
    {
      const gchar *choice;
      dbus_message_iter_get_basic (&iter_array, &choice);
      dbus_message_iter_next (&iter_array);

      g_ptr_array_add (choices, g_strdup (choice));
    }
  g_ptr_array_add (choices, NULL);

  if (data->reply_handler_id == 0)
    {
      data->reply_handler_id = g_signal_connect (data->op,
                                                 "reply",
                                                 G_CALLBACK (mount_operation_reply),
                                                 data);
    }

  g_signal_emit_by_name (data->op,
                         "show-processes",
                         message,
                         processes,
                         choices->pdata);

 out:
  if (choices)
    g_ptr_array_free (choices, TRUE);
  if (processes)
    g_array_unref (processes);
}

/* ---------------------------------------------------------------------------------------------------- */

void
g_proxy_mount_operation_handle_aborted (const gchar      *wrapped_id,
                                        DBusMessageIter  *iter)
{
  ProxyMountOpData *data;

  g_return_if_fail (wrapped_id != NULL);
  g_return_if_fail (iter != NULL);

  G_LOCK (proxy_op);
  data = g_hash_table_lookup (id_to_op, wrapped_id);
  G_UNLOCK (proxy_op);

  if (data == NULL)
    {
      g_warning ("%s: No GMountOperation for id `%s'",
                 G_STRFUNC,
                 wrapped_id);
      goto out;
    }

  g_signal_emit_by_name (data->op, "aborted");

 out:
  ;
}

/* ---------------------------------------------------------------------------------------------------- */

void
g_proxy_mount_operation_destroy (const gchar *wrapped_id)
{
  g_return_if_fail (wrapped_id != NULL);

  if (strlen (wrapped_id) == 0)
    return;

  G_LOCK (proxy_op);
  if (!g_hash_table_remove (id_to_op, wrapped_id))
    {
      g_warning ("%s: No GMountOperation for id `%s'",
                 G_STRFUNC,
                 wrapped_id);
    }
  G_UNLOCK (proxy_op);
}

/* ---------------------------------------------------------------------------------------------------- */
