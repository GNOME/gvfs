#include <config.h>

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsbackend.h"
#include "gvfsjobsource.h"
#include "gvfsdaemonprotocol.h"
#include <gvfsjobopenforread.h>
#include <gvfsjobopenforwrite.h>
#include <gvfsjobgetinfo.h>
#include <gvfsjobgetfsinfo.h>
#include <gvfsjobsetdisplayname.h>
#include <gvfsjobenumerate.h>
#include <gvfsjobmountmountable.h>
#include <gdbusutils.h>

enum {
  PROP_0,
  PROP_OBJECT_PATH,
  PROP_DAEMON
};

struct _GVfsBackendPrivate
{
  GVfsDaemon *daemon;
  char *object_path;
  
  char *display_name;
  char *icon;
  GMountSpec *mount_spec;
};


/* TODO: Real P_() */
#define P_(_x) (_x)

static void              g_vfs_backend_job_source_iface_init (GVfsJobSourceIface    *iface);
static void              g_vfs_backend_get_property          (GObject               *object,
							      guint                  prop_id,
							      GValue                *value,
							      GParamSpec            *pspec);
static void              g_vfs_backend_set_property          (GObject               *object,
							      guint                  prop_id,
							      const GValue          *value,
							      GParamSpec            *pspec);
static GObject*          g_vfs_backend_constructor           (GType                  type,
							      guint                  n_construct_properties,
							      GObjectConstructParam *construct_params);
static DBusHandlerResult backend_dbus_handler                (DBusConnection        *connection,
							      DBusMessage           *message,
							      void                  *user_data);


G_DEFINE_TYPE_WITH_CODE (GVfsBackend, g_vfs_backend, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_VFS_TYPE_JOB_SOURCE,
						g_vfs_backend_job_source_iface_init))


static GHashTable *registered_backends = NULL;

void
g_vfs_register_backend (GType backend_type,
			const char *type)
{
  if (registered_backends == NULL)
    registered_backends = g_hash_table_new_full (g_str_hash, g_str_equal,
						 g_free, NULL);

  g_hash_table_insert (registered_backends,
		       g_strdup (type), (void *)backend_type);
}

GType
g_vfs_lookup_backend (const char *type)
{
  gpointer res;
  
  if (registered_backends != NULL)
    {
      res = g_hash_table_lookup (registered_backends, type);
      if (res != NULL)
	return (GType)res;
    }
  
  return G_TYPE_INVALID;
}
  
static void
g_vfs_backend_finalize (GObject *object)
{
  GVfsBackend *backend;

  backend = G_VFS_BACKEND (object);

  g_vfs_daemon_unregister_path (backend->priv->daemon, backend->priv->object_path);
  g_object_unref (backend->priv->daemon);
  g_free (backend->priv->object_path);
  
  g_free (backend->priv->display_name);
  if (backend->priv->mount_spec)
    g_mount_spec_unref (backend->priv->mount_spec);
  
  if (G_OBJECT_CLASS (g_vfs_backend_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_parent_class)->finalize) (object);
}

static void
g_vfs_backend_job_source_iface_init (GVfsJobSourceIface *iface)
{
}

static void
g_vfs_backend_class_init (GVfsBackendClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GVfsBackendPrivate));
  
  gobject_class->constructor = g_vfs_backend_constructor;
  gobject_class->finalize = g_vfs_backend_finalize;
  gobject_class->set_property = g_vfs_backend_set_property;
  gobject_class->get_property = g_vfs_backend_get_property;

  g_object_class_install_property (gobject_class,
				   PROP_OBJECT_PATH,
				   g_param_spec_string ("object-path",
							P_("Backend object path"),
							P_("The dbus object path for the backend object."),
							"",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));
  g_object_class_install_property (gobject_class,
				   PROP_DAEMON,
				   g_param_spec_object ("daemon",
							P_("Daemon"),
							P_("The daemon this backend is handled by."),
							G_VFS_TYPE_DAEMON,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));
}

static void
g_vfs_backend_init (GVfsBackend *backend)
{
  backend->priv = G_TYPE_INSTANCE_GET_PRIVATE (backend, G_VFS_TYPE_BACKEND, GVfsBackendPrivate);
  backend->priv->icon = g_strdup ("");
  backend->priv->display_name = g_strdup ("");
}

static void
g_vfs_backend_set_property (GObject         *object,
			    guint            prop_id,
			    const GValue    *value,
			    GParamSpec      *pspec)
{
  GVfsBackend *backend = G_VFS_BACKEND (object);
  
  switch (prop_id)
    {
    case PROP_OBJECT_PATH:
      backend->priv->object_path = g_value_dup_string (value);
      break;
    case PROP_DAEMON:
      backend->priv->daemon = G_VFS_DAEMON (g_value_dup_object (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_vfs_backend_get_property (GObject    *object,
			    guint       prop_id,
			    GValue     *value,
			    GParamSpec *pspec)
{
  GVfsBackend *backend = G_VFS_BACKEND (object);
  
  switch (prop_id)
    {
    case PROP_OBJECT_PATH:
      g_value_set_string (value, backend->priv->object_path);
      break;
    case PROP_DAEMON:
      g_value_set_object (value, backend->priv->daemon);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static GObject*
g_vfs_backend_constructor (GType                  type,
			   guint                  n_construct_properties,
			   GObjectConstructParam *construct_params)
{
  GObject *object;
  GVfsBackend *backend;

  object = (* G_OBJECT_CLASS (g_vfs_backend_parent_class)->constructor) (type,
									 n_construct_properties,
									 construct_params);
  backend = G_VFS_BACKEND (object);
  
  g_vfs_daemon_register_path (backend->priv->daemon,
			      backend->priv->object_path, 
			      backend_dbus_handler,
			      backend);
  
  return object;
}

void
g_vfs_backend_set_display_name (GVfsBackend *backend,
				const char *display_name)
{
  g_free (backend->priv->display_name);
  backend->priv->display_name = g_strdup (display_name);
}

void
g_vfs_backend_set_icon (GVfsBackend *backend,
			const char *icon)
{
  g_free (backend->priv->icon);
  backend->priv->display_name = g_strdup (icon);
}

void
g_vfs_backend_set_mount_spec (GVfsBackend *backend,
			      GMountSpec *mount_spec)
{
  if (backend->priv->mount_spec)
    g_mount_spec_unref (backend->priv->mount_spec);
  backend->priv->mount_spec = g_mount_spec_ref (mount_spec);
}

static DBusHandlerResult
backend_dbus_handler (DBusConnection  *connection,
		      DBusMessage     *message,
		      void            *user_data)
{
  GVfsBackend *backend = user_data;
  GVfsJob *job;

  job = NULL;
  
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_MOUNT_INTERFACE,
				   G_VFS_DBUS_MOUNT_OP_OPEN_FOR_READ))
    job = g_vfs_job_open_for_read_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_GET_INFO))
    job = g_vfs_job_get_info_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_GET_FILESYSTEM_INFO))
    job = g_vfs_job_get_fs_info_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_ENUMERATE))
    job = g_vfs_job_enumerate_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_OPEN_FOR_WRITE))
    job = g_vfs_job_open_for_write_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_MOUNT_MOUNTABLE))
    job = g_vfs_job_mount_mountable_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_SET_DISPLAY_NAME))
    job = g_vfs_job_set_display_name_new (connection, message, backend);

  if (job)
    {
      g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), job);
      g_object_unref (job);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
      
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void
g_vfs_backend_register_mount (GVfsBackend *backend,
			      GAsyncDBusCallback callback,
			      gpointer user_data)
{
  DBusMessage *message;
  DBusMessageIter iter;
  
  message = dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
					  G_VFS_DBUS_MOUNTTRACKER_PATH,
					  G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					  G_VFS_DBUS_MOUNTTRACKER_OP_REGISTER_MOUNT);
  if (message == NULL)
    _g_dbus_oom ();

  if (!dbus_message_append_args (message,
				 DBUS_TYPE_STRING, &backend->priv->display_name,
				 DBUS_TYPE_STRING, &backend->priv->icon,
				 DBUS_TYPE_OBJECT_PATH, &backend->priv->object_path,
				 0))
    _g_dbus_oom ();

  dbus_message_iter_init_append (message, &iter);
  g_mount_spec_to_dbus (&iter, backend->priv->mount_spec);

  dbus_message_set_auto_start (message, TRUE);

  _g_dbus_connection_call_async (NULL, message, -1, 
				 callback, user_data);
  dbus_message_unref (message);
}
