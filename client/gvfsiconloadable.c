/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2008 Red Hat, Inc.
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
#include <glib/gi18n-lib.h>

#include "gvfsicon.h"
#include "gvfsiconloadable.h"
#include "gmounttracker.h"
#include "gvfsdaemondbus.h"
#include "gdaemonvfs.h"
#include "gdaemonfileinputstream.h"
#include "gvfsdaemonprotocol.h"
#include "gvfsdbusutils.h"

/* see comment in common/giconvfs.c for why the GLoadableIcon interface is here */

static DBusMessage *
create_empty_message (GVfsIcon *vfs_icon,
		      const char *op,
		      GMountInfo **mount_info_out,
		      GError **error)
{
  DBusMessage *message;
  GMountInfo *mount_info;

  mount_info = _g_daemon_vfs_get_mount_info_sync (vfs_icon->mount_spec,
						  "/",
						  error);

  if (mount_info == NULL)
    return NULL;

  if (mount_info_out)
    *mount_info_out = g_mount_info_ref (mount_info);

  message =
    dbus_message_new_method_call (mount_info->dbus_id,
				  mount_info->object_path,
				  G_VFS_DBUS_MOUNT_INTERFACE,
				  op);

  _g_dbus_message_append_args (message, G_DBUS_TYPE_CSTRING, &(vfs_icon->icon_id), 0);

  g_mount_info_unref (mount_info);
  return message;
}

static DBusMessage *
do_sync_path_call (GVfsIcon *vfs_icon,
		   const char *op,
		   GMountInfo **mount_info_out,
		   DBusConnection **connection_out,
		   GCancellable *cancellable,
		   GError **error,
		   int first_arg_type,
		   ...)
{
  DBusMessage *message, *reply;
  va_list var_args;
  GError *my_error;

 retry:
  message = create_empty_message (vfs_icon, op, mount_info_out, error);
  if (!message)
    return NULL;

  va_start (var_args, first_arg_type);
  _g_dbus_message_append_args_valist (message,
				      first_arg_type,
				      var_args);
  va_end (var_args);


  my_error = NULL;
  reply = _g_vfs_daemon_call_sync (message,
				   connection_out,
				   NULL, NULL, NULL,
				   cancellable, &my_error);
  dbus_message_unref (message);

  if (reply == NULL)
    {
      if (g_error_matches (my_error, G_VFS_ERROR, G_VFS_ERROR_RETRY))
	{
	  g_error_free (my_error);
	  goto retry;
	}
      g_propagate_error (error, my_error);
    }

  return reply;
}

static GInputStream *
g_vfs_icon_load (GLoadableIcon  *icon,
                 int            size,
                 char          **type,
                 GCancellable   *cancellable,
                 GError        **error)
{
  DBusConnection *connection;
  int fd;
  DBusMessage *reply;
  guint32 fd_id;
  dbus_bool_t can_seek;

  reply = do_sync_path_call (G_VFS_ICON (icon),
			     G_VFS_DBUS_MOUNT_OP_OPEN_ICON_FOR_READ,
			     NULL,
                             &connection,
			     cancellable,
                             error,
			     0);
  if (reply == NULL)
    return NULL;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_INVALID))
    {
      dbus_message_unref (reply);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Invalid return value from %s"), "open_icon_for_read");
      return NULL;
    }

  dbus_message_unref (reply);

  fd = _g_dbus_connection_get_fd_sync (connection, fd_id);
  if (fd == -1)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			   _("Didn't get stream file descriptor"));
      return NULL;
    }

  return G_INPUT_STREAM (g_daemon_file_input_stream_new (fd, can_seek));
}


typedef void (*AsyncPathCallCallback) (DBusMessage *reply,
				       DBusConnection *connection,
				       GSimpleAsyncResult *result,
				       GCancellable *cancellable,
				       gpointer callback_data);


typedef struct {
  GSimpleAsyncResult *result;
  GVfsIcon *vfs_icon;
  char *op;
  GCancellable *cancellable;
  DBusMessage *args;
  AsyncPathCallCallback callback;
  gpointer callback_data;
  GDestroyNotify notify;
} AsyncPathCall;


static void
async_path_call_free (AsyncPathCall *data)
{
  if (data->notify)
    data->notify (data->callback_data);

  if (data->result)
    g_object_unref (data->result);
  g_object_unref (data->vfs_icon);
  g_free (data->op);
  if (data->cancellable)
    g_object_unref (data->cancellable);
  if (data->args)
    dbus_message_unref (data->args);
  g_free (data);
}

static void
async_path_call_done (DBusMessage *reply,
		      DBusConnection *connection,
		      GError *io_error,
		      gpointer _data)
{
  AsyncPathCall *data = _data;
  GSimpleAsyncResult *result;

  if (io_error != NULL)
    {
      g_simple_async_result_set_from_error (data->result, io_error);
      _g_simple_async_result_complete_with_cancellable (data->result, data->cancellable);
      async_path_call_free (data);
    }
  else
    {
      result = data->result;
      g_object_weak_ref (G_OBJECT (result), (GWeakNotify)async_path_call_free, data);
      data->result = NULL;
      
      data->callback (reply, connection,
		      result,
		      data->cancellable,
		      data->callback_data);

      /* Free data here, or later if callback ref:ed the result */
      g_object_unref (result);
    }
}

static void
do_async_path_call_callback (GMountInfo *mount_info,
			     gpointer _data,
			     GError *error)
{
  AsyncPathCall *data = _data;
  DBusMessage *message;
  DBusMessageIter arg_source, arg_dest;
  
  if (error != NULL)
    {
      g_simple_async_result_set_from_error (data->result, error);      
      _g_simple_async_result_complete_with_cancellable (data->result, data->cancellable);
      async_path_call_free (data);
      return;
    }

  message =
    dbus_message_new_method_call (mount_info->dbus_id,
				  mount_info->object_path,
				  G_VFS_DBUS_MOUNT_INTERFACE,
				  data->op);
  
  _g_dbus_message_append_args (message, G_DBUS_TYPE_CSTRING, &(data->vfs_icon->icon_id), 0);

  /* Append more args from data->args */

  if (data->args)
    {
      dbus_message_iter_init (data->args, &arg_source);
      dbus_message_iter_init_append (message, &arg_dest);

      _g_dbus_message_iter_copy (&arg_dest, &arg_source);
    }

  _g_vfs_daemon_call_async (message,
			    async_path_call_done, data,
			    data->cancellable);
  
  dbus_message_unref (message);
}


static void
do_async_path_call (GVfsIcon *vfs_icon,
		    const char *op,
		    GCancellable *cancellable,
		    GAsyncReadyCallback op_callback,
		    gpointer op_callback_data,
		    AsyncPathCallCallback callback,
		    gpointer callback_data,
		    GDestroyNotify notify,
		    int first_arg_type,
		    ...)
{
  va_list var_args;
  AsyncPathCall *data;

  data = g_new0 (AsyncPathCall, 1);

  data->result = g_simple_async_result_new (G_OBJECT (vfs_icon),
					    op_callback, op_callback_data,
					    NULL);

  data->vfs_icon = g_object_ref (vfs_icon);
  data->op = g_strdup (op);
  if (cancellable)
    data->cancellable = g_object_ref (cancellable);
  data->callback = callback;
  data->callback_data = callback_data;
  data->notify = notify;
  
  if (first_arg_type != 0)
    {
      data->args = dbus_message_new (DBUS_MESSAGE_TYPE_METHOD_CALL);
      if (data->args == NULL)
	_g_dbus_oom ();
      
      va_start (var_args, first_arg_type);
      _g_dbus_message_append_args_valist (data->args,
					  first_arg_type,
					  var_args);
      va_end (var_args);
    }
  
  
  _g_daemon_vfs_get_mount_info_async (vfs_icon->mount_spec,
                                      "/",
                                      do_async_path_call_callback,
                                      data);
}

typedef struct {
  GSimpleAsyncResult *result;
  GCancellable *cancellable;
  gboolean can_seek;
} GetFDData;

static void
load_async_get_fd_cb (int fd,
		      gpointer callback_data)
{
  GetFDData *data = callback_data;
  GFileInputStream *stream;

  if (fd == -1)
    {
      g_simple_async_result_set_error (data->result,
				       G_IO_ERROR, G_IO_ERROR_FAILED,
				       _("Couldn't get stream file descriptor"));
    }
  else
    {
      stream = g_daemon_file_input_stream_new (fd, data->can_seek);
      g_simple_async_result_set_op_res_gpointer (data->result, stream, g_object_unref);
    }

  _g_simple_async_result_complete_with_cancellable (data->result,
                                                    data->cancellable);

  g_object_unref (data->result);
  if (data->cancellable)
    g_object_unref (data->cancellable);
  g_free (data);
}

static void
load_async_cb (DBusMessage *reply,
	       DBusConnection *connection,
	       GSimpleAsyncResult *result,
	       GCancellable *cancellable,
	       gpointer callback_data)
{
  guint32 fd_id;
  dbus_bool_t can_seek;
  GetFDData *get_fd_data;

  if (!dbus_message_get_args (reply, NULL,
			      DBUS_TYPE_UINT32, &fd_id,
			      DBUS_TYPE_BOOLEAN, &can_seek,
			      DBUS_TYPE_INVALID))
    {
      g_simple_async_result_set_error (result,
				       G_IO_ERROR, G_IO_ERROR_FAILED,
				       _("Invalid return value from %s"), "open");
      _g_simple_async_result_complete_with_cancellable (result, cancellable);
      return;
    }

  get_fd_data = g_new0 (GetFDData, 1);
  get_fd_data->result = g_object_ref (result);
  if (cancellable)
    get_fd_data->cancellable = g_object_ref (cancellable);
  get_fd_data->can_seek = can_seek;

  _g_dbus_connection_get_fd_async (connection, fd_id,
				   load_async_get_fd_cb, get_fd_data);
}

static void
g_vfs_icon_load_async (GLoadableIcon       *icon,
                       int                  size,
                       GCancellable        *cancellable,
                       GAsyncReadyCallback  callback,
                       gpointer             user_data)
{
  do_async_path_call (G_VFS_ICON (icon),
		      G_VFS_DBUS_MOUNT_OP_OPEN_ICON_FOR_READ,
		      cancellable,
		      callback, user_data,
		      load_async_cb, NULL, NULL,
		      0);
}

static GInputStream *
g_vfs_icon_load_finish (GLoadableIcon  *icon,
                         GAsyncResult   *res,
                         char          **type,
                         GError        **error)
{
  GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (res);
  gpointer op;

  op = g_simple_async_result_get_op_res_gpointer (simple);
  if (op)
    return g_object_ref (op);

  return NULL;
}


static void
g_vfs_icon_loadable_icon_iface_init (GLoadableIconIface *iface)
{
  iface->load = g_vfs_icon_load;
  iface->load_async = g_vfs_icon_load_async;
  iface->load_finish = g_vfs_icon_load_finish;
}

void
_g_vfs_icon_add_loadable_interface (void)
{
  static const GInterfaceInfo g_implement_interface_info = {
    (GInterfaceInitFunc) g_vfs_icon_loadable_icon_iface_init
  };

  g_type_add_interface_static (G_VFS_TYPE_ICON, G_TYPE_LOADABLE_ICON, &g_implement_interface_info);
}
