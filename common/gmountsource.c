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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <gmountsource.h>
#include <gdbusutils.h>
#include <gio/gio.h>
#include <gvfsdaemonprotocol.h>

struct _GMountSource
{
  GObject parent_instance;

  char *dbus_id;
  char *obj_path;
};

G_DEFINE_TYPE (GMountSource, g_mount_source, G_TYPE_OBJECT);

static void
g_mount_source_finalize (GObject *object)
{
  GMountSource *source;

  source = G_MOUNT_SOURCE (object);

  g_free (source->dbus_id);
  g_free (source->obj_path);
  
  if (G_OBJECT_CLASS (g_mount_source_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_mount_source_parent_class)->finalize) (object);
}

static void
g_mount_source_class_init (GMountSourceClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_mount_source_finalize;
}

static void
g_mount_source_init (GMountSource *mount_source)
{
}

GMountSource *
g_mount_source_new (const char *dbus_id,
		    const char *obj_path)
{
  GMountSource *source;

  source = g_object_new (G_TYPE_MOUNT_SOURCE, NULL);

  source->dbus_id = g_strdup (dbus_id);
  source->obj_path = g_strdup (obj_path);
  
  return source;
}

GMountSource *
g_mount_source_new_dummy (void)
{
  GMountSource *source;

  source = g_object_new (G_TYPE_MOUNT_SOURCE, NULL);
  
  source->dbus_id = g_strdup ("");
  source->obj_path = g_strdup ("/");
  
  return source;
}


void
g_mount_source_to_dbus (GMountSource *source,
			DBusMessage *message)
{
  g_assert (source->dbus_id != NULL);
  g_assert (source->obj_path != NULL);

  if (!dbus_message_append_args (message,
				 DBUS_TYPE_STRING, &source->dbus_id,
				 DBUS_TYPE_OBJECT_PATH, &source->obj_path,
				 0))
    _g_dbus_oom ();
  
}

const char *
g_mount_source_get_dbus_id (GMountSource *mount_source)
{
  return mount_source->dbus_id;
}

const char *
g_mount_source_get_obj_path (GMountSource *mount_source)
{
  return mount_source->obj_path;
}

typedef struct AskPasswordData AskPasswordData;

struct AskPasswordData {
  void (*callback)(AskPasswordData *data);
  /* For sync calls */
  GMutex *mutex;
  GCond *cond;
  /* For async calls */
  GMountOperation *op;

  /* results: */
  gboolean handled;
  gboolean aborted;
  char *password;
  char *username;
  char *domain;
} ;

static void
ask_password_reply (DBusMessage *reply,
		    GError *error,
		    gpointer _data)
{
  AskPasswordData *data = _data;
  dbus_bool_t handled, aborted, anonymous;
  guint32 password_save;
  const char *password, *username, *domain;
  DBusMessageIter iter;

  data->handled = TRUE;
  if (reply == NULL)
    {
      data->aborted = TRUE;
    }
  else
    {
      dbus_message_iter_init (reply, &iter);
      if (!_g_dbus_message_iter_get_args (&iter, NULL,
					  DBUS_TYPE_BOOLEAN, &handled,
					  DBUS_TYPE_BOOLEAN, &aborted,
					  DBUS_TYPE_STRING, &password,
					  DBUS_TYPE_STRING, &username,
					  DBUS_TYPE_STRING, &domain,
					  DBUS_TYPE_BOOLEAN, &anonymous,
					  DBUS_TYPE_UINT32, &password_save,
					  0))
	data->aborted = TRUE;
      else
	{
	  data->handled = handled;
	  data->aborted = aborted;

	  data->password = g_strdup (password);
	  data->username = g_strdup (username);
	  data->domain = g_strdup (domain);

	  /* TODO: handle more args */
	}
    }

  data->callback (data);
}

static gboolean
password_non_handled_in_idle (gpointer _data)
{
  AskPasswordData *data = _data;

  data->callback (data);
  return FALSE;
}

static void
g_mount_source_ask_password_async (GMountSource *source,
				   const char *message_string,
				   const char *default_user,
				   const char *default_domain,
				   GAskPasswordFlags flags,
				   AskPasswordData *data)
{
  DBusMessage *message;
  guint32 flags_as_int;

  /* If no dbus id specified, reply that we weren't handled */
  if (source->dbus_id[0] == 0)
    {
      data->handled = FALSE;
      
      g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
		       password_non_handled_in_idle,
		       data,
		       NULL);
      return;
    }

  if (message_string == NULL)
    message_string = "";
  if (default_user == NULL)
    default_user = "";
  if (default_domain == NULL)
    default_domain = "";

  flags_as_int = flags;
  
  message = dbus_message_new_method_call (source->dbus_id,
					  source->obj_path,
					  G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
					  G_VFS_DBUS_MOUNT_OPERATION_OP_ASK_PASSWORD);
  
  _g_dbus_message_append_args (message,
			       DBUS_TYPE_STRING, &message_string,
			       DBUS_TYPE_STRING, &default_user,
			       DBUS_TYPE_STRING, &default_domain,
			       DBUS_TYPE_UINT32, &flags_as_int,
			       0);

  /* 30 minute timeout */
  _g_dbus_connection_call_async (NULL, message, 1000 * 60 * 30,
				 ask_password_reply, data);
  dbus_message_unref (message);
}


static void
ask_password_reply_sync (AskPasswordData *data)
{
  /* Wake up sync call thread */
  g_mutex_lock (data->mutex);
  g_cond_signal (data->cond);
  g_mutex_unlock (data->mutex);
}

gboolean
g_mount_source_ask_password (GMountSource *source,
			     const char *message_string,
			     const char *default_user,
			     const char *default_domain,
			     GAskPasswordFlags flags,
			     gboolean *aborted,
			     char **password_out,
			     char **user_out,
			     char **domain_out)
{
  AskPasswordData data = {NULL};

  if (password_out)
    *password_out = NULL;
  if (user_out)
    *user_out = NULL;
  if (domain_out)
    *domain_out = NULL;
  
  data.mutex = g_mutex_new ();
  data.cond = g_cond_new ();
  data.callback = ask_password_reply_sync;

  g_mutex_lock (data.mutex);


  g_mount_source_ask_password_async (source,
				     message_string,
				     default_user,
				     default_domain,
				     flags,
				     &data);
  
  g_cond_wait(data.cond, data.mutex);
  g_mutex_unlock (data.mutex);

  g_cond_free (data.cond);
  g_mutex_free (data.mutex);

  if (aborted)
    *aborted = data.aborted;
  if (password_out)
    *password_out = data.password;
  else
    g_free (data.password);
  if (user_out)
    *user_out = data.username;
  else
    g_free (data.username);
  if (domain_out)
    *domain_out = data.domain;
  else
    g_free (data.domain);
  
  return data.handled;
}

static void
ask_password_reply_async (AskPasswordData *data)
{
  GMountOperationResult result;

  if (!data->handled)
    result = G_MOUNT_OPERATION_UNHANDLED;
  else if (data->aborted)
    result = G_MOUNT_OPERATION_ABORTED;
  else
    {
      result = G_MOUNT_OPERATION_HANDLED;

      if (data->password)
	g_mount_operation_set_password (data->op,
					data->password);
      if (data->username)
	g_mount_operation_set_username (data->op,
					data->username);
      if (data->domain)
	g_mount_operation_set_domain (data->op,
				      data->domain);
    }
  
  g_mount_operation_reply (data->op, result);

  g_object_unref (data->op);
  g_free (data);
}

static gboolean
op_ask_password (GMountOperation *op,
		 const char      *message,
		 const char      *default_user,
		 const char      *default_domain,
		 GAskPasswordFlags flags,
		 GMountSource *mount_source)
{
  AskPasswordData *data;

  data = g_new0 (AskPasswordData, 1);
  data->callback = ask_password_reply_async;
  data->op = g_object_ref (op);
  
  g_mount_source_ask_password_async (mount_source,
				     message,
				     default_user,
				     default_domain,
				     flags,
				     data);
  return TRUE;
}


GMountOperation *
g_mount_source_get_operation (GMountSource *mount_source)
{
  GMountOperation *op;

  op = g_mount_operation_new ();
  g_object_set_data_full (G_OBJECT (op), "source",
			  g_object_ref (mount_source),
			  g_object_unref);


  g_signal_connect (op, "ask_password", (GCallback)op_ask_password, mount_source);

  return op;
}
