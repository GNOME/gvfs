#include <config.h>

#include <gmountsource.h>
#include <gdbusutils.h>
#include <gio/gioerror.h>
#include <gvfsdaemonprotocol.h>

struct _GMountSource
{
  GObject parent_instance;

  GMountSpec *mount_spec;
  char *dbus_id;
  char *obj_path;
  gboolean is_automount;
};

G_DEFINE_TYPE (GMountSource, g_mount_source, G_TYPE_OBJECT);

static void
g_mount_source_finalize (GObject *object)
{
  GMountSource *source;

  source = G_MOUNT_SOURCE (object);

  g_free (source->dbus_id);
  g_free (source->obj_path);
  if (source->mount_spec)
    g_mount_spec_unref (source->mount_spec);
  
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
  mount_source->is_automount = FALSE;
}

void
g_mount_source_set_is_automount (GMountSource *source,
				 gboolean is_automount)
{
  source->is_automount = is_automount;
}

gboolean
g_mount_source_get_is_automount (GMountSource *source)
{
  return source->is_automount;
}

GMountSource *
g_mount_source_new_dbus (const char                *dbus_id,
			 const char                *obj_path,
			 GMountSpec                *spec)
{
  GMountSource *source;

  source = g_object_new (G_TYPE_MOUNT_SOURCE, NULL);

  source->dbus_id = g_strdup (dbus_id);
  source->obj_path = g_strdup (obj_path);
  if (spec)
    source->mount_spec = g_mount_spec_ref (spec);
  
  return source;
}

void
g_mount_source_to_dbus (GMountSource *source,
			DBusMessage *message)
{
  dbus_bool_t automount;
  DBusMessageIter iter;
  
  g_assert (source->dbus_id != NULL);
  g_assert (source->obj_path != NULL);
  g_assert (source->mount_spec != NULL);

  automount = source->is_automount;
  if (!dbus_message_append_args (message,
				 DBUS_TYPE_STRING, &source->dbus_id,
				 DBUS_TYPE_OBJECT_PATH, &source->obj_path,
				 DBUS_TYPE_BOOLEAN, &automount,
				 0))
    _g_dbus_oom ();
  
  dbus_message_iter_init_append (message, &iter);
  g_mount_spec_to_dbus (&iter, source->mount_spec);
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

GMountSource *
g_mount_source_new_null (GMountSpec *spec)
{
  GMountSource *source;

  source = g_object_new (G_TYPE_MOUNT_SOURCE, NULL);
  
  source->mount_spec = g_mount_spec_ref (spec);
  
  return source;
}

static DBusMessage *
send_sync_and_unref (DBusMessage *message,
		     GError **error)
{
  DBusError derror;
  DBusConnection *connection;
  DBusMessage *reply;
  
  dbus_error_init (&derror);
  connection = dbus_bus_get (DBUS_BUS_SESSION, &derror);
  if (connection == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "Can't open dbus connection");
      dbus_message_unref (message);
      return NULL;
    }

  reply = dbus_connection_send_with_reply_and_block (connection, message, 4000, &derror);
  dbus_connection_unref (connection);
  dbus_message_unref (message);
  if (reply == NULL)
    {
      _g_error_from_dbus (&derror, error);      
      dbus_error_free (&derror);
      return NULL;
    }

  return reply;
}

static void
send_noreply_and_unref (DBusMessage *message)
{
  DBusError derror;
  DBusConnection *connection;

  dbus_message_set_no_reply (message, TRUE);
  
  dbus_error_init (&derror);
  connection = dbus_bus_get (DBUS_BUS_SESSION, &derror);
  if (connection == NULL)
    {
      g_warning ("Can't get dbus connection");
      return;
    }

  dbus_connection_send (connection, message, NULL);
  dbus_message_unref (message);
  dbus_connection_unref (connection);
}

void
g_mount_source_done (GMountSource *source)
{
  dbus_bool_t succeeded_dbus = TRUE;
  DBusMessage *message;

  /* Fail gracefully if no source specified */
  if (source == NULL)
    return;
  
  if (source->dbus_id == NULL)
    return;

  message = dbus_message_new_method_call (source->dbus_id,
					  source->obj_path,
					  G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
					  "done");
  _g_dbus_message_append_args (message,
			       DBUS_TYPE_BOOLEAN, &succeeded_dbus,
			       0);

  send_noreply_and_unref (message);
}

void
g_mount_source_failed (GMountSource *source,
		       GError *error)
{
  dbus_bool_t succeeded_dbus = FALSE;
  DBusMessage *message;
  const char *domain;
  guint32 code;

  /* Fail gracefully if no source specified */
  if (source == NULL)
    return;
  
  if (source->dbus_id == NULL)
    {
      g_print ("Error mounting: %s\n", error->message);
      return;
    }

  message = dbus_message_new_method_call (source->dbus_id,
					  source->obj_path,
					  G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
					  "done");

  domain = g_quark_to_string (error->domain);
  code = error->code;
  _g_dbus_message_append_args (message,
			       DBUS_TYPE_BOOLEAN, &succeeded_dbus,
			       DBUS_TYPE_STRING, &domain,
			       DBUS_TYPE_UINT32, &code,
			       DBUS_TYPE_STRING, &error->message,
			       0);

  send_noreply_and_unref (message);
}

GMountSpec *
g_mount_source_request_mount_spec (GMountSource *source,
				   GError **error)
{
  DBusMessage *message, *reply;
  DBusMessageIter iter;

  if (source->mount_spec)
    return source->mount_spec;

  g_assert (source->dbus_id != NULL || source->obj_path != NULL);
  
  message =
    dbus_message_new_method_call (source->dbus_id,
				  source->obj_path,
				  G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
				  "getMountSpec");

  reply = send_sync_and_unref (message, error);
  if (reply == NULL)
    return NULL;
  
  dbus_message_iter_init (reply, &iter);
  source->mount_spec = g_mount_spec_from_dbus (&iter);
  dbus_message_unref (reply);
  if (source->mount_spec == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   "No mount spec gotten from mount source");
      return NULL;
    }
  
  return source->mount_spec;
}

typedef struct {
  GMountSource *source;
  RequestMountSpecCallback callback;
  gpointer user_data;
} RequestMountSpecData;

static void
request_mount_spec_reply (DBusMessage *reply,
			  GError *error,
			  gpointer _data)
{
  DBusMessageIter iter;
  RequestMountSpecData *data = _data;

  if (reply == NULL)
    {
      data->callback (data->source, NULL, error, data->user_data);
      goto out;
    }
  
  dbus_message_iter_init (reply, &iter);
  data->source->mount_spec = g_mount_spec_from_dbus (&iter);
  
  if (data->source->mount_spec == NULL)
    {
      error = NULL;
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		   "No mount spec gotten from mount source");
      data->callback (data->source, NULL, error, data->user_data);
      g_error_free (error);
      goto out;
    }
  
  data->callback (data->source, data->source->mount_spec, NULL, data->user_data);

 out:
  g_object_unref (data->source);
  g_free (data);
}

void
g_mount_source_request_mount_spec_async (GMountSource *source,
					 RequestMountSpecCallback callback,
					 gpointer user_data)
{
  RequestMountSpecData *data;
  DBusMessage *message;
  
  if (source->mount_spec)
    {
      callback (source, source->mount_spec, NULL, user_data);
      return;
    }

  g_assert (source->dbus_id != NULL || source->obj_path != NULL);

  message = dbus_message_new_method_call (source->dbus_id,
					  source->obj_path,
					  G_VFS_DBUS_MOUNT_OPERATION_INTERFACE,
					  "getMountSpec");


  data = g_new (RequestMountSpecData, 1);
  data->source = g_object_ref (source);
  data->callback = callback;
  data->user_data = user_data;
  
  _g_dbus_connection_call_async (NULL, message, -1,
				 request_mount_spec_reply, data);
  dbus_message_unref (message);
}

typedef struct {
  GMutex *mutex;
  GCond *cond;
  gboolean handled;
  gboolean aborted;
  char *password;
  char *username;
  char *domain;
} AskPasswordData;

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
			     GPasswordFlags flags,
			     gboolean *aborted,
			     char **password_out,
			     char **user_out,
			     char **domain_out)
{
  DBusMessage *message;
  guint32 flags_as_int;
  AskPasswordData data = {0};

  *password_out = NULL;
  *user_out = NULL;
  *domain_out = NULL;
  
  if (source->dbus_id == NULL)
    return FALSE;

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
					  "askPassword");
  
  _g_dbus_message_append_args (message,
			       DBUS_TYPE_STRING, &message_string,
			       DBUS_TYPE_STRING, &default_user,
			       DBUS_TYPE_STRING, &default_domain,
			       DBUS_TYPE_UINT32, &flags_as_int,
			       0);

  data.mutex = g_mutex_new ();
  data.cond = g_cond_new ();

  g_mutex_lock (data.mutex);
  
  _g_dbus_connection_call_async (NULL, message, -1,
				 ask_password_reply, &data);
  dbus_message_unref (message);
  
  g_cond_wait(data.cond, data.mutex);
  g_mutex_unlock (data.mutex);

  g_cond_free (data.cond);
  g_mutex_free (data.mutex);

  *aborted = data.aborted;
  *password_out = data.password;
  *user_out = data.username;
  *domain_out = data.domain;
  
  return data.handled;
}

