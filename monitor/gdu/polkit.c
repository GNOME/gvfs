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

#include <sys/types.h>
#include <unistd.h>

#include <gdbusutils.h>

#include "polkit.h"

static void
_obtain_authz_cb (DBusMessage *reply,
                  GError      *error,
                  gpointer     user_data)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (user_data);
  gboolean gained_authz;
  DBusError derror;

  if (error != NULL) {
    g_simple_async_result_set_from_error (simple, error);
    goto out;
  }

  dbus_error_init (&derror);
  if (!dbus_message_get_args (reply,
                              &derror,
                              DBUS_TYPE_BOOLEAN, &gained_authz,
                              DBUS_TYPE_INVALID))
    {
      /* no need to translate; this only happens if the auth agent is buggy */
      g_simple_async_result_set_error (simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_FAILED,
                                       "Error parsing reply for ObtainAuthorization(): %s: %s",
                                       derror.name, derror.message);
      dbus_error_free (&derror);
      goto out;
    }

  if (!gained_authz && error == NULL)
    {
      /* no need to translate, is never shown */
      g_simple_async_result_set_error (simple,
                                       G_IO_ERROR,
                                       G_IO_ERROR_FAILED_HANDLED,
                                       "Didn't obtain authorization (bug in libgio user, it shouldn't display this error)");
    }

 out:
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
}


gboolean
_obtain_authz_finish (GAsyncResult *res,
                      GError       **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);

  g_warn_if_fail (g_simple_async_result_get_source_tag (simple) == _obtain_authz);

  if (g_simple_async_result_propagate_error (simple, error))
    return FALSE;
  else
    return TRUE;
}

void
_obtain_authz (const gchar *action_id,
               GCancellable *cancellable,
               GAsyncReadyCallback callback,
               gpointer user_data)
{
  DBusConnection *connection;
  DBusMessage *message;
  GSimpleAsyncResult *simple;
  guint xid;
  guint pid;
  DBusError derror;

  dbus_error_init (&derror);

  /* this connection is already integrated and guaranteed to exist, see gvfsproxyvolumemonitordaemon.c */
  connection = dbus_bus_get (DBUS_BUS_SESSION, &derror);

  simple = g_simple_async_result_new (NULL,
                                      callback,
                                      user_data,
                                      _obtain_authz);

  message = dbus_message_new_method_call ("org.freedesktop.PolicyKit.AuthenticationAgent", /* bus name */
                                          "/",                                             /* object */
                                          "org.freedesktop.PolicyKit.AuthenticationAgent", /* interface */
                                          "ObtainAuthorization");

  xid = 0;
  pid = getpid ();

  dbus_message_append_args (message,
                            DBUS_TYPE_STRING,
                            &(action_id),
                            DBUS_TYPE_UINT32,
                            &(xid),
                            DBUS_TYPE_UINT32,
                            &(pid),
                            DBUS_TYPE_INVALID);

  _g_dbus_connection_call_async (connection,
                                 message,
                                 -1,
                                 (GAsyncDBusCallback) _obtain_authz_cb,
                                 simple);
  dbus_message_unref (message);
  dbus_connection_unref (connection);
}
