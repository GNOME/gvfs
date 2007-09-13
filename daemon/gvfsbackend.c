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
#include <gvfsjobgetinfo.h>
#include <gvfsjobenumerate.h>
#include <gdbusutils.h>

enum {
  PROP_0,
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
  g_free (backend->display_name);
  if (backend->mount_spec)
    g_mount_spec_unref (backend->mount_spec);
  
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
  /*GVfsBackend *backend = G_VFS_BACKEND (object);*/
  
  switch (prop_id)
    {
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
  /*GVfsBackend *backend = G_VFS_BACKEND (object);*/
  
  switch (prop_id)
    {
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
  
  return object;
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
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNTPOINT_INTERFACE,
					G_VFS_DBUS_OP_ENUMERATE))
    job = g_vfs_job_enumerate_new (connection, message, backend);

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
  DBusConnection *conn;
  DBusMessage *message, *reply;
  DBusMessageIter iter;
  char *icon = "icon";
  DBusError error;
  
  g_vfs_daemon_add_job_source (daemon, G_VFS_JOB_SOURCE (backend));
  backend->object_path = g_vfs_daemon_register_mount  (daemon,
						       backend_dbus_handler,
						       backend);
  g_print ("registered backend at path %s\n", backend->object_path);

  conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);

  message = dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
					  "/org/gtk/vfs/mounttracker",
					  "org.gtk.gvfs.MountTracker",
					  "registerMount");
  if (message == NULL)
    _g_dbus_oom ();

  if (!dbus_message_append_args (message,
				 DBUS_TYPE_STRING, &backend->display_name,
				 DBUS_TYPE_STRING, &icon,
				 DBUS_TYPE_OBJECT_PATH, &backend->object_path,
				 0))
    _g_dbus_oom ();

  dbus_message_iter_init_append (message, &iter);
  g_mount_spec_to_dbus (&iter, backend->mount_spec);

  dbus_message_set_auto_start (message, TRUE);
  
  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (conn, message,
						     -1, &error);
  if (reply == NULL)
    {
      /* TODO: handle better */
      g_error ("failed to register mountpoint: %s", error.message);
      exit (1);
    }

  /* TODO: check result for ok */
}
