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
#include <gvfsdbusutils.h>
#include <gio/gio.h>
#include <gvfsdaemonprotocol.h>

struct _GMountSource
{
  GObject parent_instance;

  char *dbus_id;
  char *obj_path;
};

G_DEFINE_TYPE (GMountSource, g_mount_source, G_TYPE_OBJECT)

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

  /* results: */
  gboolean       aborted;
  char          *password;
  char          *username;
  char          *domain;
  GPasswordSave  password_save;
  gboolean	 anonymous;
};

typedef struct AskSyncData AskSyncData;

struct AskSyncData {

  /* For sync calls */
  GMutex *mutex;
  GCond *cond;

  /* results: */
  GAsyncResult *result;
};

static void
ask_password_data_free (gpointer _data)
{
  AskPasswordData *data = (AskPasswordData *) _data;
  g_free (data->password);
  g_free (data->username);
  g_free (data->domain);
  g_free (data);
}

/* the callback from dbus -> main thread */
static void
ask_password_reply (DBusMessage *reply,
		    GError      *error,
		    gpointer     _data)
{
  GSimpleAsyncResult *result;
  AskPasswordData *data;
  dbus_bool_t handled, aborted, anonymous;
  guint32 password_save;
  const char *password, *username, *domain;
  DBusMessageIter iter;

  result = G_SIMPLE_ASYNC_RESULT (_data);
  handled = TRUE;
  
  data = g_new0 (AskPasswordData, 1);
  g_simple_async_result_set_op_res_gpointer (result, data, ask_password_data_free);

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
	  data->aborted = aborted;

	  if (!anonymous)
	    {
	      data->password = g_strdup (password);
	      data->username = *username == 0 ? NULL : g_strdup (username);
	      data->domain = *domain == 0 ? NULL : g_strdup (domain);
	    }
	  data->password_save = (GPasswordSave)password_save;
	  data->anonymous = anonymous;

	  /* TODO: handle more args */
	}
    }

  if (handled == FALSE)
    {
      g_simple_async_result_set_error (result, G_IO_ERROR, G_IO_ERROR_FAILED, "Internal Error");
    }

  g_simple_async_result_complete (result);
}

void
g_mount_source_ask_password_async (GMountSource              *source,
                                   const char                *message_string,
                                   const char                *default_user,
                                   const char                *default_domain,
                                   GAskPasswordFlags          flags,
                                   GAsyncReadyCallback        callback,
                                   gpointer                   user_data)
{
  GSimpleAsyncResult *result;
  DBusMessage *message;
  guint32 flags_as_int;
 

  /* If no dbus id specified, reply that we weren't handled */
  if (source->dbus_id[0] == 0)
    { 
      g_simple_async_report_error_in_idle (G_OBJECT (source),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_FAILED, 
					   "Internal Error"); 
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

  result = g_simple_async_result_new (G_OBJECT (source), callback, user_data, 
                                      g_mount_source_ask_password_async);
  /* 30 minute timeout */
  _g_dbus_connection_call_async (NULL, message, 1000 * 60 * 30,
				 ask_password_reply, result);
  dbus_message_unref (message);

}

/**
 * g_mount_source_ask_password_finish:
 * @source: the source to query
 * @result: the async result
 * @aborted: set to %TRUE if the password dialog was aborted by the user
 * @password_out: the to the password set by the user or to %NULL if none
 * @user_out: set to the username set by the user or to %NULL if none
 * @domain_out: set to the domain set by the user or to %NULL if none
 * @anonymous_out: set to %TRUE if the user selected anonymous login. This
 *                 should only happen if G_ASK_PASSWORD_ANONYMOUS_SUPPORTED
 *                 was supplied whe querying the password.
 * @password_save_out: set to the save flags to use when saving the password
 *                     in the keyring.
 *
 * Requests the reply parameters from a g_mount_source_ask_password_async() 
 * request. All out parameters can be set to %NULL to ignore them.
 * <note><para>Please be aware that out parameters other than the password
 * are set to %NULL if the user don't specify them so make sure to
 * check them.</para></note>
 *
 * Returns: %FALSE if the async reply contained an error.
 **/
gboolean
g_mount_source_ask_password_finish (GMountSource  *source,
                                    GAsyncResult  *result,
                                    gboolean      *aborted,
                                    char         **password_out,
                                    char         **user_out,
                                    char         **domain_out,
				    gboolean	  *anonymous_out,
				    GPasswordSave *password_save_out)
{
  AskPasswordData *data, def = { TRUE, };
  GSimpleAsyncResult *simple;

  simple = G_SIMPLE_ASYNC_RESULT (result);

  if (g_simple_async_result_propagate_error (simple, NULL))
    data = &def;
  else
    data = (AskPasswordData *) g_simple_async_result_get_op_res_gpointer (simple);

  if (aborted)
    *aborted = data->aborted;

  if (password_out)
    {
      *password_out = data->password;
      data->password = NULL;
    }

  if (user_out)
    {
      *user_out = data->username;
      data->username = NULL;
    }

  if (domain_out)
    {
      *domain_out = data->domain;
      data->domain = NULL;
    }

  if (anonymous_out)
    *anonymous_out = data->anonymous;

  if (password_save_out)
    *password_save_out = data->password_save;  
  
  return data != &def;
}


static void
ask_reply_sync  (GObject *source_object,
		 GAsyncResult *res,
		 gpointer user_data)
{
  AskSyncData *data;

  data = (AskSyncData *) user_data;

  data->result = g_object_ref (res);

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
			     gboolean *aborted_out,
			     char **password_out,
			     char **user_out,
			     char **domain_out,
			     gboolean *anonymous_out,
			     GPasswordSave *password_save_out)
{
  gboolean handled;
  AskSyncData data = {NULL};
  
  data.mutex = g_mutex_new ();
  data.cond = g_cond_new ();

  g_mutex_lock (data.mutex);


  g_mount_source_ask_password_async (source,
                                     message_string,
                                     default_user,
                                     default_domain,
                                     flags,
                                     ask_reply_sync,
                                     &data);
  
  g_cond_wait(data.cond, data.mutex);
  g_mutex_unlock (data.mutex);

  g_cond_free (data.cond);
  g_mutex_free (data.mutex);


  handled = g_mount_source_ask_password_finish (source,
                                                data.result,
                                                aborted_out,
                                                password_out,
                                                user_out,
                                                domain_out,
						anonymous_out,
						password_save_out);
  g_object_unref (data.result);

  return handled;
}

static void
op_ask_password_reply (GObject *source_object,
                       GAsyncResult *res,
                       gpointer user_data)
{
  GMountOperationResult result;
  GMountOperation *op;
  GMountSource *source;
  gboolean handled, aborted;
  char *username;
  char *password;
  char *domain;
  GPasswordSave password_save;

  source = G_MOUNT_SOURCE (source_object);
  op = G_MOUNT_OPERATION (user_data);
  username = NULL;
  password = NULL;
  domain = NULL;

  handled = g_mount_source_ask_password_finish (source,
                                                res,
                                                &aborted,
                                                &password,
                                                &username,
                                                &domain,
						NULL,
						&password_save);

  if (!handled)
    result = G_MOUNT_OPERATION_UNHANDLED;
  else if (aborted)
    result = G_MOUNT_OPERATION_ABORTED;
  else
    {
      result = G_MOUNT_OPERATION_HANDLED;

      if (password)
	g_mount_operation_set_password (op, password);
      if (username)
	g_mount_operation_set_username (op, username);
      if (domain)
	g_mount_operation_set_domain (op, domain);
      g_mount_operation_set_password_save (op, password_save);
    }
  
  g_mount_operation_reply (op, result);
  g_object_unref (op);
}

static gboolean
op_ask_password (GMountOperation *op,
		 const char      *message,
		 const char      *default_user,
		 const char      *default_domain,
		 GAskPasswordFlags flags,
		 GMountSource *mount_source)
{
  g_mount_source_ask_password_async (mount_source,
				     message,
				     default_user,
				     default_domain,
                                     flags,
				     op_ask_password_reply,
				     g_object_ref (op));
  g_signal_stop_emission_by_name (op, "ask_password");
  return TRUE;
}

typedef struct AskQuestionData AskQuestionData;

struct AskQuestionData {

  /* results: */
  gboolean aborted;
  guint32  choice;
};

/* the callback from dbus -> main thread */
static void
ask_question_reply (DBusMessage *reply,
		    GError      *error,
		    gpointer     _data)
{
  GSimpleAsyncResult *result;
  AskQuestionData *data;
  dbus_bool_t handled, aborted;
  guint32 choice;
  DBusMessageIter iter;

  result = G_SIMPLE_ASYNC_RESULT (_data);
  handled = TRUE;
  
  data = g_new0 (AskQuestionData, 1);
  g_simple_async_result_set_op_res_gpointer (result, data, g_free);

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
					  DBUS_TYPE_UINT32, &choice,
					  0))
	data->aborted = TRUE;
      else
	{
	  data->aborted = aborted;
	  data->choice = choice;
	}
    }

  if (handled == FALSE)
    {
      g_simple_async_result_set_error (result, G_IO_ERROR, G_IO_ERROR_FAILED, "Internal Error");
    }

  g_simple_async_result_complete (result);
}

gboolean
g_mount_source_ask_question (GMountSource *source,
			     const char   *message,
			     const char  **choices,
			     gint          n_choices,
			     gboolean     *aborted_out,
			     gint         *choice_out)
{
  gint choice;
  gboolean handled, aborted;
  AskSyncData data = {NULL};
  
  data.mutex = g_mutex_new ();
  data.cond = g_cond_new ();

  g_mutex_lock (data.mutex);

  g_mount_source_ask_question_async (source,
                                     message,
				     choices,
				     n_choices,
                                     ask_reply_sync,
                                     &data);
  
  g_cond_wait(data.cond, data.mutex);
  g_mutex_unlock (data.mutex);

  g_cond_free (data.cond);
  g_mutex_free (data.mutex);

  handled = g_mount_source_ask_question_finish (source,
                                                data.result,
                                                &aborted,
                                                &choice);
  
  g_object_unref (data.result);

  if (aborted_out)
    *aborted_out = aborted;

  if (choice_out)
    *choice_out = choice;
  
  return handled;	
}

void
g_mount_source_ask_question_async (GMountSource       *source,
				   const char         *message_string,
				   const char        **choices,
				   gint                n_choices,
				   GAsyncReadyCallback callback,
				   gpointer            user_data)
{
  GSimpleAsyncResult *result;
  DBusMessage *message;

  /* If no dbus id specified, reply that we weren't handled */
  if (source->dbus_id[0] == 0)
    { 
      g_simple_async_report_error_in_idle (G_OBJECT (source),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_FAILED, 
					   "Internal Error"); 
      return;
    }

  if (message_string == NULL)
    message_string = "";
  
  message = dbus_message_new_method_call (source->dbus_id,
					  source->obj_path,
					  G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
					  G_VFS_DBUS_MOUNT_OPERATION_OP_ASK_QUESTION);

  _g_dbus_message_append_args (message,
			       DBUS_TYPE_STRING, &message_string,
			       DBUS_TYPE_ARRAY, DBUS_TYPE_STRING,
			       &choices, n_choices,
			       0);

  result = g_simple_async_result_new (G_OBJECT (source), callback, user_data, 
                                      g_mount_source_ask_question_async);
  /* 30 minute timeout */
  _g_dbus_connection_call_async (NULL, message, 1000 * 60 * 30,
				 ask_question_reply, result);
  dbus_message_unref (message);
	
}

gboolean
g_mount_source_ask_question_finish (GMountSource *source,
				    GAsyncResult *result,
				    gboolean     *aborted,
				    gint         *choice_out)
{
  AskQuestionData *data, def= { FALSE, };
  GSimpleAsyncResult *simple;

  simple = G_SIMPLE_ASYNC_RESULT (result);

  if (g_simple_async_result_propagate_error (simple, NULL))
    data = &def;
  else
    data = (AskQuestionData *) g_simple_async_result_get_op_res_gpointer (simple);

  if (aborted)
    *aborted = data->aborted;

  if (choice_out)
    *choice_out = data->choice;

  return data != &def;	
}

static void
op_ask_question_reply (GObject      *source_object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
  GMountOperationResult result;
  GMountOperation *op;
  GMountSource *source;
  gboolean handled, aborted;
  gint choice;

  source = G_MOUNT_SOURCE (source_object);
  op = G_MOUNT_OPERATION (user_data);

  handled = g_mount_source_ask_question_finish (source,
                                                res,
                                                &aborted,
						&choice);

  if (!handled)
    result = G_MOUNT_OPERATION_UNHANDLED;
  else if (aborted)
    result = G_MOUNT_OPERATION_ABORTED;
  else
    {
      result = G_MOUNT_OPERATION_HANDLED;
      g_mount_operation_set_choice (op, choice);
    }
  
  g_mount_operation_reply (op, result);
  g_object_unref (op);
}

static gboolean
op_ask_question (GMountOperation *op,
		 const char      *message,
		 const char     **choices,
		 GMountSource    *mount_source)
{
  g_mount_source_ask_question_async (mount_source,
				     message,
				     choices,
				     g_strv_length ((gchar **) choices),
				     op_ask_question_reply,
				     g_object_ref (op));
  g_signal_stop_emission_by_name (op, "ask_question");
  return TRUE;
}

typedef struct ShowProcessesData ShowProcessesData;

struct ShowProcessesData {

  /* results: */
  gboolean aborted;
  guint32  choice;
};

/* the callback from dbus -> main thread */
static void
show_processes_reply (DBusMessage *reply,
                      GError      *error,
                      gpointer     _data)
{
  GSimpleAsyncResult *result;
  ShowProcessesData *data;
  dbus_bool_t handled, aborted;
  guint32 choice;
  DBusMessageIter iter;

  result = G_SIMPLE_ASYNC_RESULT (_data);
  handled = TRUE;

  data = g_new0 (ShowProcessesData, 1);
  g_simple_async_result_set_op_res_gpointer (result, data, g_free);

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
					  DBUS_TYPE_UINT32, &choice,
					  0))
	data->aborted = TRUE;
      else
	{
	  data->aborted = aborted;
	  data->choice = choice;
	}
    }

  if (handled == FALSE)
    {
      g_simple_async_result_set_error (result, G_IO_ERROR, G_IO_ERROR_FAILED, "Internal Error");
    }

  g_simple_async_result_complete (result);
}

void
g_mount_source_show_processes_async (GMountSource        *source,
                                     const char          *message_string,
                                     GArray              *processes,
                                     const char         **choices,
                                     gint                 n_choices,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  GSimpleAsyncResult *result;
  DBusMessage *message;

  /* If no dbus id specified, reply that we weren't handled */
  if (source->dbus_id[0] == 0)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (source),
					   callback,
					   user_data,
					   G_IO_ERROR, G_IO_ERROR_FAILED,
					   "Internal Error");
      return;
    }

  result = g_simple_async_result_new (G_OBJECT (source), callback, user_data,
                                      g_mount_source_show_processes_async);

  if (message_string == NULL)
    message_string = "";

  message = dbus_message_new_method_call (source->dbus_id,
					  source->obj_path,
					  G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
					  G_VFS_DBUS_MOUNT_OPERATION_OP_SHOW_PROCESSES);

  _g_dbus_message_append_args (message,
			       DBUS_TYPE_STRING, &message_string,
			       DBUS_TYPE_ARRAY, DBUS_TYPE_STRING,
			       &choices, n_choices,
			       DBUS_TYPE_ARRAY, DBUS_TYPE_INT32,
			       &processes->data, processes->len,
			       0);

  /* 30 minute timeout */
  _g_dbus_connection_call_async (NULL, message, 1000 * 60 * 30,
				 show_processes_reply, result);
  dbus_message_unref (message);
}

gboolean
g_mount_source_show_processes_finish (GMountSource *source,
                                      GAsyncResult *result,
                                      gboolean     *aborted,
                                      gint         *choice_out)
{
  ShowProcessesData *data, def= { FALSE, };
  GSimpleAsyncResult *simple;

  simple = G_SIMPLE_ASYNC_RESULT (result);

  if (g_simple_async_result_propagate_error (simple, NULL))
    data = &def;
  else
    data = (ShowProcessesData *) g_simple_async_result_get_op_res_gpointer (simple);

  if (aborted)
    *aborted = data->aborted;

  if (choice_out)
    *choice_out = data->choice;

  return data != &def;
}

gboolean
g_mount_source_show_processes (GMountSource *source,
                               const char   *message,
                               GArray       *processes,
                               const char  **choices,
                               gint          n_choices,
                               gboolean     *aborted_out,
                               gint         *choice_out)
{
  gint choice;
  gboolean handled, aborted;
  AskSyncData data = {NULL};

  data.mutex = g_mutex_new ();
  data.cond = g_cond_new ();

  g_mutex_lock (data.mutex);

  g_mount_source_show_processes_async (source,
                                       message,
                                       processes,
                                       choices,
                                       n_choices,
                                       ask_reply_sync,
                                       &data);

  g_cond_wait (data.cond, data.mutex);
  g_mutex_unlock (data.mutex);

  g_cond_free (data.cond);
  g_mutex_free (data.mutex);

  handled = g_mount_source_show_processes_finish (source,
                                                  data.result,
                                                  &aborted,
                                                  &choice);

  g_object_unref (data.result);

  if (aborted_out)
    *aborted_out = aborted;

  if (choice_out)
    *choice_out = choice;

  return handled;
}

static void
op_show_processes_reply (GObject      *source_object,
                         GAsyncResult *res,
                         gpointer      user_data)
{
  GMountOperationResult result;
  GMountOperation *op;
  GMountSource *source;
  gboolean handled, aborted;
  gint choice;

  source = G_MOUNT_SOURCE (source_object);
  op = G_MOUNT_OPERATION (user_data);

  handled = g_mount_source_show_processes_finish (source,
                                                  res,
                                                  &aborted,
                                                  &choice);

  if (!handled)
    result = G_MOUNT_OPERATION_UNHANDLED;
  else if (aborted)
    result = G_MOUNT_OPERATION_ABORTED;
  else
    {
      result = G_MOUNT_OPERATION_HANDLED;
      g_mount_operation_set_choice (op, choice);
    }

  g_mount_operation_reply (op, result);
  g_object_unref (op);
}

static gboolean
op_show_processes (GMountOperation *op,
                   const char      *message,
                   GArray          *processes,
                   const char     **choices,
                   GMountSource    *mount_source)
{
  g_mount_source_show_processes_async (mount_source,
                                       message,
                                       processes,
                                       choices,
                                       g_strv_length ((gchar **) choices),
                                       op_show_processes_reply,
                                       g_object_ref (op));
  g_signal_stop_emission_by_name (op, "show_processes");
  return TRUE;
}

gboolean
g_mount_source_abort (GMountSource *source)
{
  DBusMessage *message;
  DBusConnection *connection;
  gboolean ret;

  ret = FALSE;

  /* If no dbus id specified, reply that we weren't handled */
  if (source->dbus_id[0] == 0)
    goto out;

  connection = dbus_bus_get (DBUS_BUS_SESSION, NULL);
  if (connection == NULL)
    goto out;

  message = dbus_message_new_method_call (source->dbus_id,
					  source->obj_path,
					  G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
					  G_VFS_DBUS_MOUNT_OPERATION_OP_ABORTED);

  if (message)
    {
      dbus_connection_send (connection, message, NULL);
      dbus_message_unref (message);
    }

  ret = TRUE;

 out:
  return ret;
}

static void
op_aborted (GMountOperation *op,
	    GMountSource    *source)
{
  g_mount_source_abort (source);
}

gboolean
g_mount_source_is_dummy (GMountSource *source)
{
  g_return_val_if_fail (G_IS_MOUNT_SOURCE (source), TRUE);
  return source->dbus_id[0] == 0;
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
  g_signal_connect (op, "ask_question", (GCallback)op_ask_question, mount_source);
  g_signal_connect (op, "show_processes", (GCallback)op_show_processes, mount_source);
  g_signal_connect (op, "aborted", (GCallback)op_aborted, mount_source);

  return op;
}
