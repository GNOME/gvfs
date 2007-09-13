#include <config.h>
#include <string.h>
#include <dbus/dbus.h>
#include "gvfsimpldaemon.h"
#include "gvfsuriutils.h"
#include "gfiledaemon.h"
#include "gfiledaemonlocal.h"
#include "gvfslocal.h"
#include <gvfsdaemonprotocol.h>
#include "gvfsdaemondbus.h"
#include "gdbusutils.h"

static void g_vfs_impl_daemon_class_init     (GVfsImplDaemonClass *class);
static void g_vfs_impl_daemon_vfs_iface_init (GVfsIface       *iface);
static void g_vfs_impl_daemon_finalize       (GObject         *object);

typedef struct {
  char *bus_name;
  char *object_path;
  GQuark match_name; /* bus name without path prefix */
  char *path_prefix;
} MountInfo;

struct _GVfsImplDaemon
{
  GObject parent;

  DBusConnection *bus;
  
  GVfs *wrapped_vfs;
  GList *mounts;
};

static GVfsImplDaemon *the_vfs = NULL;
G_LOCK_DEFINE_STATIC(mounts);

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
      G_LOCK (mounts);
      G_UNLOCK (mounts);
      /* a bus client died */
    }
  
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static MountInfo *
mount_info_from_name (char *name)
{
  MountInfo *info;
  char *last_dot;

  g_assert (g_str_has_prefix (name, G_VFS_DBUS_MOUNTPOINT_NAME));
  
  info = g_new0 (MountInfo, 1);
  info->bus_name = name;

  last_dot = strrchr (name, '.');
  if (last_dot != NULL &&
      g_str_has_prefix (last_dot, ".f_"))
    {
      char *without_prefix = g_strndup (name, last_dot - name);
      info->match_name = g_quark_from_string (without_prefix);
      g_free (without_prefix);
      info->path_prefix = _g_dbus_unescape_bus_name (last_dot + 3, NULL);
    }
  else
    info->match_name = g_quark_from_string (name);

  info->object_path = g_strdup_printf (G_VFS_DBUS_MOUNTPOINT_PATH "%s",
				       name + strlen (G_VFS_DBUS_MOUNTPOINT_NAME));
  return info;
}

static void
g_vfs_impl_daemon_init (GVfsImplDaemon *vfs)
{
  DBusError error;
  GList *names, *l;
  char *name;
  MountInfo *info;

  g_assert (the_vfs == NULL);
  the_vfs = vfs;
  
  vfs->wrapped_vfs = g_vfs_local_new ();

  if (g_thread_supported ())
    dbus_threads_init_default ();
  
  vfs->bus = dbus_bus_get (DBUS_BUS_SESSION, NULL);

  if (vfs->bus)
    {
      _g_dbus_connection_setup_with_main (vfs->bus);

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

      names = _g_dbus_bus_list_names_with_prefix_sync (vfs->bus, G_VFS_DBUS_MOUNTPOINT_NAME, NULL);
      for (l = names; l != NULL; l = l->next)
	{
	  name = l->data;
	  info = mount_info_from_name (name);
	  vfs->mounts = g_list_prepend (vfs->mounts, info);
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

static int
get_n_path_elements (const char *str)
{
  int n;

  n = 0;

  while (*str != 0)
    {
      while (*str == '/')
	str++;

      if (*str != '/' && *str != 0)
	{
	  n++;
	  while (*str != '/' && *str != 0)
	    str++;
	}
    }

  return n;
}

#define INCLUDE_HOST (1<<0)
#define INCLUDE_PORT (1<<1)
#define INCLUDE_USER (1<<2)
#define USES_MOUNT_PREFIX (1<<3)
#define HOSTNAME_IN_PATH (1<<3)

struct UriMapping {
  const char *uri_scheme;
  int min_path_elements;
  const char *backend;
  guint flags;
};

/* TODO: This should really be a config file */
static struct UriMapping uri_mapping[] = {
  { "ftp", 0, "ftp", INCLUDE_HOST|INCLUDE_PORT|INCLUDE_USER},
  { "smb", 2, "smb-share", INCLUDE_HOST|INCLUDE_PORT|INCLUDE_USER|USES_MOUNT_PREFIX},
  { "smb", 0, "smb-browse", HOSTNAME_IN_PATH},
  { "computer", 0, "computer", 0},
  { "network", 0, "network", 0},
  { "test", 0, "test", 0},
  { "http", 0, "http", HOSTNAME_IN_PATH},
  { "https", 0, "https", HOSTNAME_IN_PATH},
  { "dav", 0, "dav", INCLUDE_HOST|INCLUDE_PORT|INCLUDE_USER|USES_MOUNT_PREFIX},
  { "davs", 0, "davs", INCLUDE_HOST|INCLUDE_PORT|INCLUDE_USER|USES_MOUNT_PREFIX},
  { "nfs", 0, "nfs", INCLUDE_HOST|INCLUDE_PORT|INCLUDE_USER|USES_MOUNT_PREFIX},
  { "sftp", 0, "sftp", INCLUDE_HOST|INCLUDE_PORT|INCLUDE_USER},
};

static char *
get_name_from_uri (GDecodedUri *uri, guint *flags_out)
{
  GString *name;
  int path_elements;
  const char *backend;
  int flags, i;

  name = g_string_new (G_VFS_DBUS_MOUNTPOINT_NAME);

  path_elements = get_n_path_elements (uri->path);

  backend = uri->scheme;
  flags = 0;
  for (i = 0; i < G_N_ELEMENTS (uri_mapping); i++)
    {
      if (strcmp (uri_mapping[i].uri_scheme, uri->scheme) == 0 &&
	  path_elements >= uri_mapping[i].min_path_elements)
	{
	  backend = uri_mapping[i].backend;
	  flags = uri_mapping[i].flags;
	  break;
	}
    }
  
  _g_dbus_append_escaped_bus_name (name, TRUE, backend);

  if (flags && INCLUDE_HOST && uri->host && *uri->host != 0)
    {
      g_string_append (name, ".h_");
      _g_dbus_append_escaped_bus_name (name, FALSE, uri->host);
    }
    
  if (flags && INCLUDE_PORT && uri->port != -1)
    g_string_append_printf (name, ".p_%d", uri->port);
    
  if (flags && INCLUDE_USER && uri->userinfo && *uri->userinfo != 0)
    {
      g_string_append (name, ".u_");
      _g_dbus_append_escaped_bus_name (name, FALSE, uri->userinfo);
    }

  if (flags_out)
    *flags_out = flags;
  return g_string_free (name, FALSE);
}

static GFile *
g_vfs_impl_daemon_get_file_for_uri (GVfs       *vfs,
				    const char *uri)
{
  GVfsImplDaemon *daemon_vfs;
  GFile *file, *wrapped;
  GDecodedUri *decoded;
  char *path;
  GQuark name_q;
  char *name;
  guint flags;

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
      name = get_name_from_uri (decoded, &flags);
      name_q = g_quark_from_string (name);
      g_free (name);

      if (flags & HOSTNAME_IN_PATH)
	{
	  /* TODO: What to do about user/port, and escaping hostname */
	  path = g_build_filename ("/", decoded->host, decoded->path, NULL);
	  file = g_file_daemon_new (name_q, path);
	  g_free (path);
	}
      else
	file = g_file_daemon_new (name_q, decoded->path);
    }

  _g_decoded_uri_free (decoded);
  
  return file;
}



DBusMessage *
_g_vfs_impl_daemon_new_path_call_valist (GQuark match_bus_name,
					 const char *path,
					 const char *op,
					 int    first_arg_type,
					 va_list var_args)
{
  GList *l;
  DBusMessage *message = NULL;
  char *new_path;
  
  G_LOCK (mounts);

  for (l = the_vfs->mounts; l != NULL; l = l->next)
    {
      MountInfo *mount_info = l->data;

      if (mount_info->match_name == match_bus_name)
	{
	  if (mount_info->path_prefix == NULL ||
	      g_str_has_prefix (path, mount_info->path_prefix))
	    {
	      message =
		dbus_message_new_method_call (mount_info->bus_name,
					      mount_info->object_path,
					      G_VFS_DBUS_MOUNTPOINT_INTERFACE,
					      op);
	      break;
	    }
	}
    }
  
  G_UNLOCK (mounts);

  if (message)
    {
      /* TODO: Handle path prefixes */
      new_path = g_strdup (path);

      _g_dbus_message_append_args (message,
				   G_DBUS_TYPE_CSTRING, &new_path,
				   0);
      g_free (new_path);
      
      _g_dbus_message_append_args_valist (message,
					  first_arg_type,
					  var_args);
    }
  return message;
}

DBusMessage *
_g_vfs_impl_daemon_new_path_call (GQuark match_bus_name,
				  const char *path,
				  const char *op,
				  int    first_arg_type,
				  ...)
{
  va_list var_args;
  DBusMessage *message;

  va_start (var_args, first_arg_type);
  message = _g_vfs_impl_daemon_new_path_call_valist (match_bus_name,
						     path, op, 
						     first_arg_type,
						     var_args);
  va_end (var_args);
  return message;
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
