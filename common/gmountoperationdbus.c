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

#include <gio/gio.h>
#include <glib/gi18n-lib.h>
#include "gmountoperationdbus.h"
#include "gvfsdbus.h"

typedef struct 
{
  GMountOperation *op;
  char *obj_path;
  char *dbus_id;
  GDBusConnection *connection;
  GVfsDBusMountOperation *mount_op_skeleton;

  GVfsDBusMountOperation *object;
  GDBusMethodInvocation *invocation;
} GMountOperationDBus;

static void
mount_op_got_reply (GMountOperationDBus *op_dbus)
{
  g_signal_handlers_disconnect_matched (op_dbus->op,
					G_SIGNAL_MATCH_ID | G_SIGNAL_MATCH_DATA,
					g_signal_lookup ("reply", G_TYPE_MOUNT_OPERATION),
					0,
					NULL,
					NULL,
					op_dbus);
}

static void
ask_password_reply (GMountOperation *op,
		    GMountOperationResult result,
		    gpointer data)
{
  GMountOperationDBus *op_dbus = data;
  const char *username, *password, *domain;
  gboolean anonymous;
  guint32 password_save;
  gboolean handled, abort_dbus;

  handled = (result != G_MOUNT_OPERATION_UNHANDLED);
  abort_dbus = (result == G_MOUNT_OPERATION_ABORTED);
  
  password = g_mount_operation_get_password (op);
  if (password == NULL)
    password = "";
  username = g_mount_operation_get_username (op);
  if (username == NULL)
    username = "";
  domain = g_mount_operation_get_domain (op);
  if (domain == NULL)
    domain = "";
  anonymous = g_mount_operation_get_anonymous (op);
  password_save = g_mount_operation_get_password_save (op);

  gvfs_dbus_mount_operation_complete_ask_password (op_dbus->object,
                                                   op_dbus->invocation,
                                                   handled,
                                                   abort_dbus,
                                                   password,
                                                   username,
                                                   domain,
                                                   anonymous,
                                                   password_save);

  mount_op_got_reply (op_dbus);
}

static gboolean
handle_ask_password (GVfsDBusMountOperation *object,
                     GDBusMethodInvocation *invocation,
                     const gchar *arg_message_string,
                     const gchar *arg_default_user,
                     const gchar *arg_default_domain,
                     guint arg_flags_as_int,
                     gpointer data)
{
  GMountOperationDBus *op_dbus = data;

  op_dbus->object = object;
  op_dbus->invocation = invocation;
  g_signal_connect (op_dbus->op, "reply",
                    G_CALLBACK (ask_password_reply),
                    op_dbus);
  
  g_signal_emit_by_name (op_dbus->op, "ask_password",
                         arg_message_string,
                         arg_default_user,
                         arg_default_domain,
                         arg_flags_as_int);
  return TRUE;
}

static void
ask_question_reply (GMountOperation *op,
		    GMountOperationResult result,
		    gpointer data)
{
  GMountOperationDBus *op_dbus = data;
  guint32 choice;
  gboolean handled, abort_dbus;

  op_dbus = g_object_get_data (G_OBJECT (op), "dbus-op");

  handled = (result != G_MOUNT_OPERATION_UNHANDLED);
  abort_dbus = (result == G_MOUNT_OPERATION_ABORTED);
  
  choice = g_mount_operation_get_choice (op);

  gvfs_dbus_mount_operation_complete_ask_question (op_dbus->object,
                                                   op_dbus->invocation,
                                                   handled,
                                                   abort_dbus,
                                                   choice);

  mount_op_got_reply (op_dbus);
}

static gboolean
handle_ask_question (GVfsDBusMountOperation *object,
                     GDBusMethodInvocation *invocation,
                     const gchar *arg_message_string,
                     const gchar *const *arg_choices,
                     gpointer data)
{
  GMountOperationDBus *op_dbus = data;

  op_dbus->object = object;
  op_dbus->invocation = invocation;
  g_signal_connect (op_dbus->op,
                    "reply",
                    G_CALLBACK (ask_question_reply),
                    op_dbus);

  g_signal_emit_by_name (op_dbus->op, "ask_question",
                         arg_message_string,
                         arg_choices);
  return TRUE;
}

static void
show_processes_reply (GMountOperation *op,
                      GMountOperationResult result,
                      gpointer data)
{
  GMountOperationDBus *op_dbus = data;
  guint32 choice;
  gboolean handled, abort_dbus;

  op_dbus = g_object_get_data (G_OBJECT (op), "dbus-op");

  handled = (result != G_MOUNT_OPERATION_UNHANDLED);
  abort_dbus = (result == G_MOUNT_OPERATION_ABORTED);

  choice = g_mount_operation_get_choice (op);
  
  gvfs_dbus_mount_operation_complete_show_processes (op_dbus->object,
                                                     op_dbus->invocation,
                                                     handled,
                                                     abort_dbus,
                                                     choice);

  mount_op_got_reply (op_dbus);
}

static gboolean
handle_show_processes (GVfsDBusMountOperation *object,
                       GDBusMethodInvocation *invocation,
                       const gchar *arg_message_string,
                       const gchar *const *arg_choices,
                       GVariant *arg_processes,
                       gpointer data)
{
  GMountOperationDBus *op_dbus = data;
  GArray *processes;
  GPid pid;
  GVariantIter iter;

  processes = g_array_new (FALSE, FALSE, sizeof (GPid));
  g_variant_iter_init (&iter, arg_processes);
  while (g_variant_iter_loop (&iter, "i", &pid))
    g_array_append_val (processes, pid);

  op_dbus->object = object;
  op_dbus->invocation = invocation;
  g_signal_connect (op_dbus->op,
                    "reply",
                    G_CALLBACK (show_processes_reply),
                    op_dbus);

  g_signal_emit_by_name (op_dbus->op, "show_processes",
                         arg_message_string,
                         processes,
                         arg_choices);

  g_array_unref (processes);
  
  return TRUE;
}

static gboolean
handle_show_unmount_progress (GVfsDBusMountOperation *object,
                              GDBusMethodInvocation *invocation,
                              const gchar *arg_message_string,
                              gint64 arg_time_left,
                              gint64 arg_bytes_left,
                              gpointer data)
{
  GMountOperationDBus *op_dbus = data;

  g_signal_emit_by_name (op_dbus->op, "show-unmount-progress",
                         arg_message_string,
                         arg_time_left,
                         arg_bytes_left);

  gvfs_dbus_mount_operation_complete_show_unmount_progress (object, invocation);
  
  return TRUE;
}

static gboolean
handle_aborted (GVfsDBusMountOperation *object,
                GDBusMethodInvocation *invocation,
                gpointer data)
{
  GMountOperationDBus *op_dbus = data;
  
  /* also emit reply to make the all DBus ops return */
  g_mount_operation_reply (op_dbus->op, G_MOUNT_OPERATION_UNHANDLED);
  g_signal_emit_by_name (op_dbus->op, "aborted");
  gvfs_dbus_mount_operation_complete_aborted (object, invocation);
  
  return TRUE;
}


static void
g_mount_operation_dbus_free (GMountOperationDBus *op_dbus)
{
  if (op_dbus->connection)
    {
      if (op_dbus->mount_op_skeleton != NULL)
        {
          g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (op_dbus->mount_op_skeleton));
          g_object_unref (op_dbus->mount_op_skeleton);
        }
      g_object_unref (op_dbus->connection);
    }
  g_free (op_dbus->dbus_id);
  g_free (op_dbus->obj_path);
  g_free (op_dbus);
}

GMountSource *
g_mount_operation_dbus_wrap (GMountOperation *op,
                             GDBusConnection *connection)
{
  GMountOperationDBus *op_dbus;
  static int mount_id = 0;
  GError *error; 

  if (op == NULL)
    return g_mount_source_new_dummy ();
  
  op_dbus = g_new0 (GMountOperationDBus, 1);
  
  op_dbus->op = op;
  op_dbus->connection = g_object_ref (connection);
  op_dbus->obj_path = g_strdup_printf ("/org/gtk/gvfs/mountop/%d", mount_id++);
  if (op_dbus->connection)
    {
      op_dbus->dbus_id = g_strdup (g_dbus_connection_get_unique_name (op_dbus->connection));
      op_dbus->mount_op_skeleton = gvfs_dbus_mount_operation_skeleton_new ();
      
      g_signal_connect (op_dbus->mount_op_skeleton, "handle-ask-password", G_CALLBACK (handle_ask_password), op_dbus);
      g_signal_connect (op_dbus->mount_op_skeleton, "handle-ask-question", G_CALLBACK (handle_ask_question), op_dbus);
      g_signal_connect (op_dbus->mount_op_skeleton, "handle-show-processes", G_CALLBACK (handle_show_processes), op_dbus);
      g_signal_connect (op_dbus->mount_op_skeleton, "handle-show-unmount-progress", G_CALLBACK (handle_show_unmount_progress), op_dbus);
      g_signal_connect (op_dbus->mount_op_skeleton, "handle-aborted", G_CALLBACK (handle_aborted), op_dbus);

      error = NULL;
      if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (op_dbus->mount_op_skeleton),
                                             op_dbus->connection,
                                             op_dbus->obj_path, 
                                             &error))
        {
          g_warning ("Error exporting GMountOperationDBus: %s (%s, %d)\n",
                      error->message, g_quark_to_string (error->domain), error->code);
          g_error_free (error);
        }
    }

  g_object_set_data_full (G_OBJECT (op), "dbus-op",
                          op_dbus, (GDestroyNotify)g_mount_operation_dbus_free);
  
  return g_mount_source_new (op_dbus->dbus_id, op_dbus->obj_path);
}
