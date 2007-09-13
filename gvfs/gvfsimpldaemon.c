#include <config.h>
#include <string.h>
#include <dbus/dbus.h>
#include "gvfsimpldaemon.h"
#include "gvfsuriutils.h"
#include "gfiledaemon.h"
#include "gfiledaemonlocal.h"
#include "gvfslocal.h"
#include <gvfsdaemonprotocol.h>
#include <gvfsdaemondbus.h>

static void g_vfs_impl_daemon_class_init     (GVfsImplDaemonClass *class);
static void g_vfs_impl_daemon_vfs_iface_init (GVfsIface       *iface);
static void g_vfs_impl_daemon_finalize       (GObject         *object);

struct _GVfsImplDaemon
{
  GObject parent;

  DBusConnection *bus;
  
  GVfs *wrapped_vfs;
};

G_DEFINE_TYPE_WITH_CODE (GVfsImplDaemon, g_vfs_impl_daemon, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_VFS,
						g_vfs_impl_daemon_vfs_iface_init))
 
static void
g_vfs_impl_daemon_class_init (GVfsImplDaemonClass *class)
{
  GObjectClass *object_class;
  
  object_class = (GObjectClass *) class;

  object_class->finalize = g_vfs_impl_daemon_finalize;
}

static void
g_vfs_impl_daemon_finalize (GObject *object)
{
  /* must chain up */
  G_OBJECT_CLASS (g_vfs_impl_daemon_parent_class)->finalize (object);
}

static DBusHandlerResult
session_bus_message_filter (DBusConnection *conn,
			    DBusMessage    *message,
			    gpointer        data)
{
  /*GVfsImplDaemon *vfs = data;*/
  char *name, *from, *to;

  
  if (dbus_message_is_signal (message, DBUS_INTERFACE_DBUS, "NameOwnerChanged") &&
      dbus_message_get_args (message, NULL,
			     DBUS_TYPE_STRING, &name,
			     DBUS_TYPE_STRING, &from,
			     DBUS_TYPE_STRING, &to,
			     DBUS_TYPE_INVALID) &&
      *name == ':' &&
      *to == 0)
    {
      /* a bus client died */
    }
  
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


static void
g_vfs_impl_daemon_init (GVfsImplDaemon *vfs)
{
  DBusError error;
  
  vfs->wrapped_vfs = g_vfs_local_new ();

  if (g_thread_supported ())
    dbus_threads_init_default ();
  
  vfs->bus = dbus_bus_get (DBUS_BUS_SESSION, NULL);

  if (vfs->bus)
    {
      _g_dbus_connection_setup_with_main (vfs->bus, NULL);

      dbus_connection_add_filter (vfs->bus, session_bus_message_filter, vfs, NULL);
      
      
      dbus_error_init (&error);
      dbus_bus_add_match (vfs->bus,
			  "sender='org.freedesktop.DBus',"
			  "interface='org.freedesktop.DBus',"
			  "member='NameOwnerChanged'",
			  &error);
      if (dbus_error_is_set (&error))
	{
	  g_warning ("Failed to add dbus match: %s\n", error.message);
	  dbus_error_free (&error);
	}
    }
}

GVfsImplDaemon *
g_vfs_impl_daemon_new (void)
{
  return g_object_new (G_TYPE_VFS_IMPL_DAEMON, NULL);
}

static GFile *
g_vfs_impl_daemon_get_file_for_path (GVfs       *vfs,
				     const char *path)
{
  GFile *file;

  /* TODO: detect fuse paths and convert to daemon vfs GFiles */
  
  file = g_vfs_get_file_for_path (G_VFS_IMPL_DAEMON (vfs)->wrapped_vfs, path);
  
  return g_file_daemon_local_new (file);
}

static GDaemonMountInfo foo_info = {
  "org.gtk.vfs.mount.foo",
  "/org/gtk/vfs/mount/foo"
};

static char *
get_path_for_uri (GDecodedUri *uri, GDaemonMountInfo **info)
{
  if (strcmp (uri->scheme, "foo") == 0)
    {
      *info = &foo_info;
      return g_strdup (uri->path);
    }
  
  return NULL;
}

static GFile *
g_vfs_impl_daemon_get_file_for_uri (GVfs       *vfs,
				    const char *uri)
{
  GVfsImplDaemon *daemon_vfs;
  GFile *file, *wrapped;
  GDecodedUri *decoded;
  char *path;

  daemon_vfs = G_VFS_IMPL_DAEMON (vfs);
  
  decoded = _g_decode_uri (uri);
  if (decoded == NULL)
    return NULL;

  if (strcmp (decoded->scheme, "file") == 0)
    {
      wrapped = g_vfs_impl_daemon_get_file_for_path  (vfs, decoded->path);
      file = g_file_daemon_local_new (wrapped);
    }
  else
    {
      GDaemonMountInfo *info;
      
      path = get_path_for_uri (decoded, &info);
      if (path != NULL)
	file = g_file_daemon_new (info, path);
      else
	file = NULL; /* TODO: Handle non-supported uris? */
    }

  _g_decoded_uri_free (decoded);
  
  return file;
}

static GFile *
g_vfs_impl_daemon_parse_name (GVfs       *vfs,
			      const char *parse_name)
{
  GFile *file;
  char *path;
  
  if (g_path_is_absolute (parse_name))
    {
      path = g_filename_from_utf8 (parse_name, -1, NULL, NULL, NULL);
      file = g_vfs_impl_daemon_get_file_for_path  (vfs, path);
      g_free (path);
    }
  else
    {
      file = g_vfs_impl_daemon_get_file_for_uri (vfs, parse_name);
    }

  return file;
}

static void
g_vfs_impl_daemon_vfs_iface_init (GVfsIface *iface)
{
  iface->get_file_for_path = g_vfs_impl_daemon_get_file_for_path;
  iface->get_file_for_uri = g_vfs_impl_daemon_get_file_for_uri;
  iface->parse_name = g_vfs_impl_daemon_parse_name;
}
