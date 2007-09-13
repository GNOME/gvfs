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

enum {
  PROP_0,
  PROP_MOUNTPOINT
};

/* TODO: Real P_() */
#define P_(_x) (_x)

static void g_vfs_backend_job_source_iface_init (GVfsJobSourceIface *iface);
static void g_vfs_backend_get_property          (GObject            *object,
						 guint               prop_id,
						 GValue             *value,
						 GParamSpec         *pspec);
static void g_vfs_backend_set_property          (GObject            *object,
						 guint               prop_id,
						 const GValue       *value,
						 GParamSpec         *pspec);
static void g_vfs_backend_reset                 (GVfsJobSource      *job_source);

G_DEFINE_TYPE_WITH_CODE (GVfsBackend, g_vfs_backend, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_VFS_JOB_SOURCE,
						g_vfs_backend_job_source_iface_init))

volatile gint mountpoint_counter = 0;

static void
g_vfs_backend_finalize (GObject *object)
{
  GVfsBackend *backend;

  backend = G_VFS_BACKEND (object);

  if (backend->mountpoint)
    g_vfs_mountpoint_free (backend->mountpoint);

  g_free (backend->mountpoint_path);
  
  if (G_OBJECT_CLASS (g_vfs_backend_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_parent_class)->finalize) (object);
}

static void
g_vfs_backend_job_source_iface_init (GVfsJobSourceIface *iface)
{
  iface->reset = g_vfs_backend_reset;
}

static void
g_vfs_backend_class_init (GVfsBackendClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_finalize;
  gobject_class->set_property = g_vfs_backend_set_property;
  gobject_class->get_property = g_vfs_backend_get_property;

  g_object_class_install_property (gobject_class,
				   PROP_MOUNTPOINT,
				   g_param_spec_pointer ("mountpoint",
							P_("Mountpoint"),
							P_("The mountpoint this backend handles."),
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));

  
}

static void
g_vfs_backend_init (GVfsBackend *backend)
{
  int id;

  id = g_atomic_int_exchange_and_add (&mountpoint_counter, 1);
  backend->mountpoint_path = g_strdup_printf ("/org/gtk/vfs/mountpoint/%d", id);
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
    case PROP_MOUNTPOINT:
      backend->mountpoint = g_vfs_mountpoint_copy (g_value_get_pointer (value));
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
    case PROP_MOUNTPOINT:
      g_value_set_pointer (value, backend->mountpoint);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
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

static DBusHandlerResult
backend_dbus_handler (DBusConnection  *connection,
		      DBusMessage     *message,
		      void            *user_data)
{
  GVfsBackend *backend = user_data;
  GVfsJob *job;

  job = NULL;
  
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_DAEMON_INTERFACE,
				   G_VFS_DBUS_OP_OPEN_FOR_READ))
    job = g_vfs_job_open_for_read_new (connection, message, backend);

  if (job)
    {
      g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), job);
      g_object_unref (job);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
      
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void
signal_mountpoint (GVfsBackend *backend)
{
  DBusMessage *signal;
  DBusMessageIter iter;
  DBusConnection *bus;

  signal = dbus_message_new_signal (backend->mountpoint_path,
				    G_VFS_DBUS_MOUNTPOINT_INTERFACE,
				    G_VFS_DBUS_ANNOUNCE_MOUNTPOINT);
  
  dbus_message_iter_init_append (signal, &iter);

  g_vfs_mountpoint_to_dbus (backend->mountpoint, &iter);

  bus = dbus_bus_get (DBUS_BUS_SESSION, NULL);
  if (bus)
    {
      dbus_connection_send (bus, signal, NULL);
      dbus_connection_unref (bus);
    }
  
  dbus_message_unref (signal);
}

static void
g_vfs_backend_reset (GVfsJobSource *job_source)
{
  signal_mountpoint (G_VFS_BACKEND (job_source));
}

void
g_vfs_backend_register_with_daemon (GVfsBackend     *backend,
				    GVfsDaemon      *daemon)
{
  g_vfs_daemon_add_job_source (daemon, G_VFS_JOB_SOURCE (backend));
  g_vfs_daemon_register_path  (daemon, backend->mountpoint_path,
			       backend_dbus_handler, backend);
  signal_mountpoint (backend);
}
