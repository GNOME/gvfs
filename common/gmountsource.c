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

#include <gmountsource.h>
#include <gio/gio.h>
#include <gvfsdbus.h>
#include "gvfsdaemonprotocol.h"

#include <string.h>

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

GVariant *
g_mount_source_to_dbus (GMountSource *source)
{
  g_assert (source->dbus_id != NULL);
  g_assert (source->obj_path != NULL);

  return g_variant_new ("(so)",
                        source->dbus_id,
                        source->obj_path);
}

GMountSource *
g_mount_source_from_dbus (GVariant *value)
{
  const gchar *obj_path, *dbus_id;

  g_variant_get (value, "(&s&o)",
                 &dbus_id,
                 &obj_path);
  
  return g_mount_source_new (dbus_id, obj_path);
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
  GMainContext *context;
  GMainLoop *loop;

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

static GVfsDBusMountOperation *
create_mount_operation_proxy_sync (GMountSource        *source,
                                   GError             **error)
{
  GVfsDBusMountOperation *proxy;
  GError *local_error;

  /* If no dbus id specified, reply that we weren't handled */
  if (source->dbus_id[0] == 0)
    { 
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Internal Error");
      return NULL;
    }

  local_error = NULL;
  /* Synchronously creating a proxy using unique/private d-bus name and not loading properties or connecting signals should not block */
  proxy = gvfs_dbus_mount_operation_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                            G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS | G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                            source->dbus_id,
                                                            source->obj_path,
                                                            NULL,
                                                            &local_error);
  if (proxy == NULL)
    {
      g_dbus_error_strip_remote_error (local_error);
      g_propagate_error (error, local_error);
    }

  return proxy;
}

/* the callback from dbus -> main thread */
static void
ask_password_reply (GVfsDBusMountOperation *proxy,
                    GAsyncResult *res,
                    gpointer user_data)
{
  GTask *task;
  AskPasswordData *data;
  gboolean handled, aborted, anonymous;
  guint32 password_save;
  gchar *password, *username, *domain;
  GError *error;

  task = G_TASK (user_data);
  handled = TRUE;

  error = NULL;
  if (!gvfs_dbus_mount_operation_call_ask_password_finish (proxy,
                                                           &handled,
                                                           &aborted,
                                                           &password,
                                                           &username,
                                                           &domain,
                                                           &anonymous,
                                                           &password_save,
                                                           res,
                                                           &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_task_return_error (task, error);
    }
  else if (handled == FALSE)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "Internal Error");
    }
  else
    {
      data = g_new0 (AskPasswordData, 1);
      data->aborted = aborted;

      if (!anonymous)
        {
          data->password = g_strdup (password);
          data->username = *username == 0 ? NULL : g_strdup (username);
          data->domain = *domain == 0 ? NULL : g_strdup (domain);
        }
      data->password_save = (GPasswordSave)password_save;
      data->anonymous = anonymous;

      g_task_return_pointer (task, data, ask_password_data_free);

      /* TODO: handle more args */
      g_free (password);
      g_free (username);
      g_free (domain);
    }

  g_object_unref (task);
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
  GTask *task;
  GVfsDBusMountOperation *proxy;
  GError *error = NULL;

  task = g_task_new (source, NULL, callback, user_data);
  g_task_set_source_tag (task, g_mount_source_ask_password_async);

  proxy = create_mount_operation_proxy_sync (source, &error);
  if (proxy == NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  /* 30 minute timeout */
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), G_VFS_DBUS_MOUNT_TIMEOUT_MSECS);

  gvfs_dbus_mount_operation_call_ask_password (proxy,
                                               message_string ? message_string : "",
                                               default_user ? default_user : "",
                                               default_domain ? default_domain : "",
                                               flags,
                                               NULL,
                                               (GAsyncReadyCallback) ask_password_reply,
                                               task);
  g_object_unref (proxy);
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

  g_return_val_if_fail (g_task_is_valid (result, source), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_mount_source_ask_password_async), FALSE);

  data = g_task_propagate_pointer (G_TASK (result), NULL);
  if (data == NULL)
    data = &def;

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

  g_main_loop_quit (data->loop);
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
  AskSyncData data;

  data.context = g_main_context_new ();
  data.loop = g_main_loop_new (data.context, FALSE);

  g_main_context_push_thread_default (data.context);

  g_mount_source_ask_password_async (source,
                                     message_string,
                                     default_user,
                                     default_domain,
                                     flags,
                                     ask_reply_sync,
                                     &data);

  g_main_loop_run (data.loop);

  handled = g_mount_source_ask_password_finish (source,
                                                data.result,
                                                aborted_out,
                                                password_out,
                                                user_out,
                                                domain_out,
						anonymous_out,
						password_save_out);

  g_main_context_pop_thread_default (data.context);
  g_main_context_unref (data.context);
  g_main_loop_unref (data.loop);
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
ask_question_reply (GVfsDBusMountOperation *proxy,
                    GAsyncResult *res,
                    gpointer user_data)
{
  GTask *task;
  AskQuestionData *data;
  gboolean handled, aborted;
  guint32 choice;
  GError *error;

  task = G_TASK (user_data);
  handled = TRUE;

  error = NULL;
  if (!gvfs_dbus_mount_operation_call_ask_question_finish (proxy,
                                                           &handled,
                                                           &aborted,
                                                           &choice,
                                                           res,
                                                           &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_task_return_error (task, error);
    }
  else if (handled == FALSE)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "Internal Error");
    }
  else
    {
      data = g_new0 (AskQuestionData, 1);
      data->aborted = aborted;
      data->choice = choice;

      g_task_return_pointer (task, data, g_free);
    }

  g_object_unref (task);
}

gboolean
g_mount_source_ask_question (GMountSource *source,
			     const char   *message,
			     const char  **choices,
			     gboolean     *aborted_out,
			     gint         *choice_out)
{
  gint choice;
  gboolean handled, aborted;
  AskSyncData data;

  data.context = g_main_context_new ();
  data.loop = g_main_loop_new (data.context, FALSE);

  g_main_context_push_thread_default (data.context);

  g_mount_source_ask_question_async (source,
                                     message,
				     choices,
                                     ask_reply_sync,
                                     &data);

  g_main_loop_run (data.loop);

  handled = g_mount_source_ask_question_finish (source,
                                                data.result,
                                                &aborted,
                                                &choice);

  g_main_context_pop_thread_default (data.context);
  g_main_context_unref (data.context);
  g_main_loop_unref (data.loop);
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
				   GAsyncReadyCallback callback,
				   gpointer            user_data)
{
  GTask *task;
  GVfsDBusMountOperation *proxy;
  GError *error = NULL;

  task = g_task_new (source, NULL, callback, user_data);
  g_task_set_source_tag (task, g_mount_source_ask_question_async);

  proxy = create_mount_operation_proxy_sync (source, &error);
  if (proxy == NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  /* 30 minute timeout */
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), G_VFS_DBUS_MOUNT_TIMEOUT_MSECS);

  gvfs_dbus_mount_operation_call_ask_question (proxy,
                                               message_string ? message_string : "",
                                               choices,   
                                               NULL,
                                               (GAsyncReadyCallback) ask_question_reply,
                                               task);
  g_object_unref (proxy);
}

gboolean
g_mount_source_ask_question_finish (GMountSource *source,
				    GAsyncResult *result,
				    gboolean     *aborted,
				    gint         *choice_out)
{
  AskQuestionData *data, def = { TRUE, };

  g_return_val_if_fail (g_task_is_valid (result, source), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_mount_source_ask_question_async), FALSE);

  data = g_task_propagate_pointer (G_TASK (result), NULL);
  if (data == NULL)
    data = &def;

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
show_processes_reply (GVfsDBusMountOperation *proxy,
                      GAsyncResult *res,
                      gpointer user_data)
{
  GTask *task;
  ShowProcessesData *data;
  gboolean handled, aborted;
  guint32 choice;
  GError *error;

  task = G_TASK (user_data);
  handled = TRUE;

  error = NULL;
  if (!gvfs_dbus_mount_operation_call_show_processes_finish (proxy,
                                                             &handled,
                                                             &aborted,
                                                             &choice,
                                                             res,
                                                             &error))
    {
      g_dbus_error_strip_remote_error (error);
      g_task_return_error (task, error);
    }
  else if (handled == FALSE)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "Internal Error");
    }
  else
    {
      data = g_new0 (ShowProcessesData, 1);
      data->aborted = aborted;
      data->choice = choice;

      g_task_return_pointer (task, data, g_free);
    }

  g_object_unref (task);
}

void
g_mount_source_show_processes_async (GMountSource        *source,
                                     const char          *message_string,
                                     GArray              *processes,
                                     const char         **choices,
                                     GAsyncReadyCallback  callback,
                                     gpointer             user_data)
{
  GTask *task;
  GVfsDBusMountOperation *proxy;
  GVariantBuilder builder;
  guint i;
  GError *error = NULL;

  task = g_task_new (source, NULL, callback, user_data);
  g_task_set_source_tag (task, g_mount_source_show_processes_async);

  proxy = create_mount_operation_proxy_sync (source, &error);
  if (proxy == NULL)
    {
      g_task_return_error (task, error);
      g_object_unref (task);
      return;
    }

  /* 30 minute timeout */
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), G_VFS_DBUS_MOUNT_TIMEOUT_MSECS);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("ai"));
  for (i = 0; i < processes->len; i++)
    g_variant_builder_add (&builder, "i", 
                           g_array_index (processes, gint32, i));
  
  gvfs_dbus_mount_operation_call_show_processes (proxy,
                                                 message_string ? message_string : "",
                                                 choices,
                                                 g_variant_builder_end (&builder),
                                                 NULL,
                                                 (GAsyncReadyCallback) show_processes_reply,
                                                 task);
  g_object_unref (proxy);
}

gboolean
g_mount_source_show_processes_finish (GMountSource *source,
                                      GAsyncResult *result,
                                      gboolean     *aborted,
                                      gint         *choice_out)
{
  ShowProcessesData *data, def = { TRUE, };

  g_return_val_if_fail (g_task_is_valid (result, source), FALSE);
  g_return_val_if_fail (g_async_result_is_tagged (result, g_mount_source_show_processes_async), FALSE);

  data = g_task_propagate_pointer (G_TASK (result), NULL);
  if (data == NULL)
    data = &def;

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
                               gboolean     *aborted_out,
                               gint         *choice_out)
{
  gint choice;
  gboolean handled, aborted;
  AskSyncData data;

  data.context = g_main_context_new ();
  data.loop = g_main_loop_new (data.context, FALSE);

  g_main_context_push_thread_default (data.context);

  g_mount_source_show_processes_async (source,
                                       message,
                                       processes,
                                       choices,
                                       ask_reply_sync,
                                       &data);

  g_main_loop_run (data.loop);

  handled = g_mount_source_show_processes_finish (source,
                                                  data.result,
                                                  &aborted,
                                                  &choice);

  g_main_context_pop_thread_default (data.context);
  g_main_context_unref (data.context);
  g_main_loop_unref (data.loop);
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
                                       op_show_processes_reply,
                                       g_object_ref (op));
  g_signal_stop_emission_by_name (op, "show_processes");
  return TRUE;
}

static void
show_unmount_progress_reply (GVfsDBusMountOperation *proxy,
                             GAsyncResult *res,
                             gpointer user_data)
{
  GError *error;

  error = NULL;
  if (!gvfs_dbus_mount_operation_call_show_unmount_progress_finish (proxy, res, &error))
    {
      g_warning ("ShowUnmountProgress request failed: %s", error->message);
      g_error_free (error);
    }
}

void
g_mount_source_show_unmount_progress (GMountSource *source,
                                      const char   *message_string,
                                      gint64      time_left,
                                      gint64      bytes_left)
{
  GVfsDBusMountOperation *proxy;

  /* If no dbus id specified, warn and return */
  if (source->dbus_id[0] == 0)
    {
      g_warning ("No dbus id specified in the mount source, "
                 "ignoring show-unmount-progress request");
      return;
    }
  
  proxy = create_mount_operation_proxy_sync (source, NULL);
  if (proxy == NULL)
    return;

  /* 30 minute timeout */
  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (proxy), G_VFS_DBUS_MOUNT_TIMEOUT_MSECS);

  gvfs_dbus_mount_operation_call_show_unmount_progress (proxy,
                                                        message_string ? message_string : "",
                                                        time_left,
                                                        bytes_left,
                                                        NULL,
                                                        (GAsyncReadyCallback) show_unmount_progress_reply,
                                                        NULL);
  g_object_unref (proxy);
}

static void
op_show_unmount_progress (GMountOperation *op,
                          const char      *message,
                          gint64           time_left,
                          gint64           bytes_left,
                          GMountSource    *mount_source)
{
  g_mount_source_show_unmount_progress (mount_source,
                                        message,
                                        time_left,
                                        bytes_left);
  g_signal_stop_emission_by_name (op, "show_unmount_progress");
}

static void
abort_reply (GVfsDBusMountOperation *proxy,
             GAsyncResult *res,
             gpointer user_data)
{
  gvfs_dbus_mount_operation_call_aborted_finish (proxy, res, NULL);
}

gboolean
g_mount_source_abort (GMountSource *source)
{
  GVfsDBusMountOperation *proxy;

  proxy = create_mount_operation_proxy_sync (source, NULL);
  if (proxy == NULL)
    return FALSE;
  
  gvfs_dbus_mount_operation_call_aborted (proxy, NULL, 
                                          (GAsyncReadyCallback) abort_reply, NULL);
  
  g_object_unref (proxy);
  return TRUE;
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
  g_signal_connect (op, "show_unmount_progress", (GCallback)op_show_unmount_progress, mount_source);
  g_signal_connect (op, "aborted", (GCallback)op_aborted, mount_source);

  return op;
}
