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

static GVfsMountpointInfo * mountpoint_info_from_dbus (DBusMessageIter *iter,
						       gboolean with_owner_and_path);

struct _GVfsImplDaemon
{
  GObject parent;

  DBusConnection *bus;
  GList *mounts;
  
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
  GVfsImplDaemon *vfs = data;
  GVfsMountpointInfo *info;
  GList *l, *next;
  char *name, *from, *to;

  if (dbus_message_is_signal (message,
			      G_VFS_DBUS_MOUNTPOINT_INTERFACE,
			      G_VFS_DBUS_ANNOUNCE_MOUNTPOINT))
    {
      const char *path = dbus_message_get_path (message);
      const char *sender = dbus_message_get_sender (message);
      DBusMessageIter iter;

      for (l = vfs->mounts; l != NULL; l = l->next)
	{
	  info = l->data;
	  if (strcmp (info->dbus_owner, sender) == 0 &&
	      strcmp (info->dbus_path, path) == 0)
	    break;
	}

      if (l == NULL)
	{
	  if (dbus_message_iter_init (message, &iter) &&
	      (info = mountpoint_info_from_dbus (&iter, FALSE)) != NULL)
	    {
	      info->dbus_owner = g_strdup (sender);
	      info->dbus_path = g_strdup (path);

	      g_print ("Added mountpoint: %s, %s, %s\n", info->dbus_owner, info->dbus_path, info->method);
	      
	      vfs->mounts = g_list_prepend (vfs->mounts, info);
	    }
	}
    }
  
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
      
      l = vfs->mounts;
      while (l != NULL)
	{
	  info = l->data;
	  next = l->next;
	  
	  if (strcmp (info->dbus_owner, name) == 0)
	    {
	      g_print ("Removed mountpoint: %s, %s, %s\n", info->dbus_owner, info->dbus_path, info->method);
	      info->is_mounted = FALSE;
	      g_vfs_mountpoint_info_unref (info);
	      vfs->mounts = g_list_delete_link (vfs->mounts, l);
	    }
	  
	  l = next;
	}
    }
  
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


static void
g_vfs_impl_daemon_init (GVfsImplDaemon *vfs)
{
  DBusMessage *message, *reply;
  DBusMessageIter iter, array, struct_iter;
  GVfsMountpointInfo *info;
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
      
      
      dbus_error_init (&error);
      dbus_bus_add_match (vfs->bus,
			  "interface='"G_VFS_DBUS_MOUNTPOINT_INTERFACE"',"
			  "member='"G_VFS_DBUS_ANNOUNCE_MOUNTPOINT"'",
			  &error);
      if (dbus_error_is_set (&error))
	{
	  g_warning ("Failed to add dbus match: %s\n", error.message);
	  dbus_error_free (&error);
	}
      
  
      g_print ("Sending ListMountPoints\n");
      message = dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
					      G_VFS_DBUS_MOUNTPOINT_TRACKER_PATH,
					      G_VFS_DBUS_MOUNTPOINT_TRACKER_INTERFACE,
					      G_VFS_DBUS_LIST_MOUNT_POINTS);
      if (message)
	{
	  dbus_message_set_auto_start (message, TRUE);
	  reply = dbus_connection_send_with_reply_and_block (vfs->bus,
							     message, -1, NULL);
	  g_print ("Got reply %p\n", reply);
	  if (reply &&
	      dbus_message_iter_init (reply, &iter) &&
	      dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_ARRAY)
	    {
	      dbus_message_iter_recurse (&iter, &array);
	      g_print ("in array: %c\n", dbus_message_iter_get_arg_type (&array));
	      
	      while (dbus_message_iter_get_arg_type (&array) == DBUS_TYPE_STRUCT)
		{
		  dbus_message_iter_recurse (&array, &struct_iter);

		  info = mountpoint_info_from_dbus (&struct_iter, TRUE);
		  g_print ("adding info: %p\n", info);
		  if (info)
		    {
		      info->is_mounted = TRUE;
		      vfs->mounts = g_list_prepend (vfs->mounts, info);
		    }
		  dbus_message_iter_next (&array);
		}
	    }
	  
	  if (reply)
	    dbus_message_unref (reply);
	  
	  dbus_message_unref (message);
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

static gboolean
string_equal (char *a, char *b)
{
  if (a == NULL)
    a = "";
  if (b == NULL)
    b = "";
  
  return strcmp (a, b) == 0;
}


static char *
uri_matches_mountpoint (GDecodedUri *uri,
			GVfsMountpointInfo *info)
{
  char *info_path, *uri_path;
  
  /* TODO: This matching needs to be much better */
  
  if (!string_equal (info->method, uri->scheme))
    return NULL;
  
  if (!string_equal (info->user, uri->userinfo))
    return NULL;
  
  if (!string_equal (info->host, uri->host))
    return NULL;

  uri_path = uri->path?uri->path:"";
  info_path = info->path?info->path:"";
  if (!g_str_has_prefix (uri_path, info_path))
    return NULL;

  return g_strdup (uri_path + strlen (info_path));
}

static GFile *
g_vfs_impl_daemon_get_file_for_uri (GVfs       *vfs,
				    const char *uri)
{
  GVfsImplDaemon *daemon_vfs;
  GFile *file, *wrapped;
  GDecodedUri *decoded;
  GVfsMountpointInfo *info;
  GList *l;
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
      file = NULL;
      for (l = daemon_vfs->mounts; l != NULL; l = l->next)
	{
	  info = l->data;

	  path = uri_matches_mountpoint (decoded, info);
	  if (path != NULL)
	    {
	      file = g_file_daemon_new (info, path);
	      break;
	    }
	}

      if (file == NULL)
	{
	  /* TODO: Handle unmounted */
	}
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

GVfsMountpointInfo *
g_vfs_mountpoint_info_ref (GVfsMountpointInfo *info)
{
  g_atomic_int_add (&info->ref_count, 1);
  return info;
}

void
g_vfs_mountpoint_info_unref (GVfsMountpointInfo *info)
{
  gint res;
  res = g_atomic_int_exchange_and_add (&info->ref_count, -1);

  if (res == 1)
    {
      g_free (info->dbus_owner);
      g_free (info->dbus_path);
      g_free (info->method);
      g_free (info->user);
      g_free (info->host);
      g_free (info->path);
      g_free (info);
    }
}

static GVfsMountpointInfo *
mountpoint_info_from_dbus (DBusMessageIter *iter,
			   gboolean with_owner_and_path)
{
  GVfsMountpointInfo *info;
  dbus_int32_t port;
  char *str;
  char *path_data;
  int path_len;
  DBusMessageIter array;

  info = g_new0 (GVfsMountpointInfo, 1);
  info->ref_count = 1;

  if (with_owner_and_path)
    {
      if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_STRING)
	goto out;
      
      dbus_message_iter_get_basic (iter, &str);
      info->dbus_owner = g_strdup (str);

      if (!dbus_message_iter_next (iter))
	goto out;
      
      if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_STRING)
	goto out;

      dbus_message_iter_get_basic (iter, &str);
      info->dbus_path = g_strdup (str);
      
      if (!dbus_message_iter_next (iter))
	goto out;
    }
  
  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_STRING)
    goto out;

  dbus_message_iter_get_basic (iter, &str);
  info->method = g_strdup (str);

  if (!dbus_message_iter_next (iter))
    goto out;
  
  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_STRING)
    goto out;

  dbus_message_iter_get_basic (iter, &str);
  info->user = g_strdup (str);

  if (!dbus_message_iter_next (iter))
    goto out;
  
  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_STRING)
    goto out;

  dbus_message_iter_get_basic (iter, &str);
  info->host = g_strdup (str);

  if (!dbus_message_iter_next (iter))
    goto out;
  
  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_INT32)
    goto out;

  dbus_message_iter_get_basic (iter, &port);
  info->port = port;

  if (!dbus_message_iter_next (iter))
    goto out;

  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_ARRAY ||
      dbus_message_iter_get_element_type (iter) != DBUS_TYPE_BYTE)
    goto out;

  dbus_message_iter_recurse (iter, &array);
  dbus_message_iter_get_fixed_array (&array, &path_data, &path_len);

  info->port = port;
  info->path = g_strndup (path_data, path_len);
  g_free (path_data);

  dbus_message_iter_next (iter);
  
  return info;

 out:
  g_vfs_mountpoint_info_unref (info);
  return NULL;
}
