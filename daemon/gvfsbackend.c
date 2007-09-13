#include <config.h>

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
#include <gvfsjobgetinfo.h>

enum {
  PROP_0,
  PROP_OBJECT_PATH,
  PROP_BUS_NAME,
};

/* TODO: Real P_() */
#define P_(_x) (_x)

static void     g_vfs_backend_job_source_iface_init (GVfsJobSourceIface    *iface);
static void     g_vfs_backend_get_property          (GObject               *object,
						     guint                  prop_id,
						     GValue                *value,
						     GParamSpec            *pspec);
static void     g_vfs_backend_set_property          (GObject               *object,
						     guint                  prop_id,
						     const GValue          *value,
						     GParamSpec            *pspec);
static GObject* g_vfs_backend_constructor           (GType                  type,
						     guint                  n_construct_properties,
						     GObjectConstructParam *construct_params);

G_DEFINE_TYPE_WITH_CODE (GVfsBackend, g_vfs_backend, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_VFS_JOB_SOURCE,
						g_vfs_backend_job_source_iface_init))

static void
g_vfs_backend_finalize (GObject *object)
{
  GVfsBackend *backend;

  backend = G_VFS_BACKEND (object);

  g_free (backend->object_path);
  g_free (backend->bus_name);
  
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
  
  gobject_class->constructor = g_vfs_backend_constructor;
  gobject_class->finalize = g_vfs_backend_finalize;
  gobject_class->set_property = g_vfs_backend_set_property;
  gobject_class->get_property = g_vfs_backend_get_property;

  g_object_class_install_property (gobject_class,
				   PROP_OBJECT_PATH,
				   g_param_spec_string ("object-path",
							P_("Object path"),
							P_("dbus object path for the mountpoint"),
							"",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));
  
  g_object_class_install_property (gobject_class,
				   PROP_BUS_NAME,
				   g_param_spec_string ("bus-name",
							P_("Bus name"),
							P_("dbus bus name for the mountpoint"),
							"",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));
}

static void
g_vfs_backend_init (GVfsBackend *backend)
{
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
      g_free (backend->object_path);
      backend->object_path = g_value_dup_string (value);
      break;
    case PROP_BUS_NAME:
      g_free (backend->bus_name);
      backend->bus_name = g_value_dup_string (value);
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
      g_value_set_string (value, backend->object_path);
      break;
    case PROP_BUS_NAME:
      g_value_set_string (value, backend->bus_name);
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
  unsigned int flags;
  DBusError error;
  DBusConnection *conn;
  int ret;

  object = (* G_OBJECT_CLASS (g_vfs_backend_parent_class)->constructor) (type,
									 n_construct_properties,
									 construct_params);

  backend = G_VFS_BACKEND (object);

  if (backend->bus_name)
    {
      conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);
      if (conn)
	{
	  flags = DBUS_NAME_FLAG_ALLOW_REPLACEMENT | DBUS_NAME_FLAG_DO_NOT_QUEUE;
	  
	  dbus_error_init (&error);
	  ret = dbus_bus_request_name (conn, backend->bus_name, flags, &error);
	  if (ret == -1)
	    {
	      g_printerr ("Failed to acquire bus name '%s': %s",
			  backend->bus_name, error.message);
	      dbus_error_free (&error);
	    }
	  else if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
	    g_printerr ("Unable to acquire bus name '%s'\n", backend->bus_name);

	  dbus_connection_unref (conn);
	}
    }
  
  return object;
}

gboolean
g_vfs_backend_open_for_read (GVfsBackend *backend,
			     GVfsJobOpenForRead *job,
			     char *filename)
{
  GVfsBackendClass *class;

  class = G_VFS_BACKEND_GET_CLASS (backend);
  
  return class->open_for_read (backend, job, filename);
}

gboolean
g_vfs_backend_close_read (GVfsBackend        *backend,
			  GVfsJobCloseRead   *job,
			  GVfsBackendHandle   handle)
{
  GVfsBackendClass *class;

  class = G_VFS_BACKEND_GET_CLASS (backend);
  
  return class->close_read (backend, job, handle);
}

gboolean
g_vfs_backend_read (GVfsBackend *backend,
		    GVfsJobRead *job,
		    GVfsBackendHandle handle,
		    char *buffer,
		    gsize bytes_requested)
{
  GVfsBackendClass *class;

  class = G_VFS_BACKEND_GET_CLASS (backend);
  
  return class->read (backend, job, handle,
		      buffer, bytes_requested);
}

gboolean
g_vfs_backend_seek_on_read  (GVfsBackend        *backend,
			     GVfsJobSeekRead    *job,
			     GVfsBackendHandle   handle,
			     goffset             offset,
			     GSeekType           type)
{
  GVfsBackendClass *class;

  class = G_VFS_BACKEND_GET_CLASS (backend);
  
  return class->seek_on_read (backend, job, handle,
			      offset, type);
}

gboolean
g_vfs_backend_get_info (GVfsBackend           *backend,
			GVfsJobGetInfo        *job,
			char                  *filename,
			GFileInfoRequestFlags  requested,
			const char            *attributes,
			gboolean               follow_symlinks)
{
  GVfsBackendClass *class;

  class = G_VFS_BACKEND_GET_CLASS (backend);
  
  return class->get_info (backend, job, filename, requested,
			  attributes, follow_symlinks);
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
				   G_VFS_DBUS_MOUNTPOINT_INTERFACE,
				   G_VFS_DBUS_OP_OPEN_FOR_READ))
    job = g_vfs_job_open_for_read_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNTPOINT_INTERFACE,
					G_VFS_DBUS_OP_GET_INFO))
    job = g_vfs_job_get_info_new (connection, message, backend);

  if (job)
    {
      g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), job);
      g_object_unref (job);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
      
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


void
g_vfs_backend_register_with_daemon (GVfsBackend     *backend,
				    GVfsDaemon      *daemon)
{
  g_print ("registering %s\n", backend->object_path);
  g_vfs_daemon_add_job_source (daemon, G_VFS_JOB_SOURCE (backend));
  g_vfs_daemon_register_path  (daemon, backend->object_path,
			       backend_dbus_handler, backend);
}
