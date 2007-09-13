#include <config.h>

#include <gmountsource.h>
#include <gdbusutils.h>
#include <gio/gvfserror.h>
#include <gvfsdaemonprotocol.h>

struct _GMountSource
{
  GObject parent_instance;

  GMountSpec *mount_spec;
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
g_mount_source_init (GMountSource *backend)
{
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
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_INTERNAL_ERROR,
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
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
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
      g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
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
  
  _g_dbus_connection_call_async (NULL, message,
				 request_mount_spec_reply, data);
  dbus_message_unref (message);
}
