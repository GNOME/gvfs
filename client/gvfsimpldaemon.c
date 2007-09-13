#include <config.h>
#include <string.h>
#include <dbus/dbus.h>
#include "gvfsimpldaemon.h"
#include "gvfsuriutils.h"
#include "gfiledaemon.h"
#include "gfiledaemonlocal.h"
#include <gio/gvfslocal.h>
#include <gvfsdaemonprotocol.h>
#include "gvfsdaemondbus.h"
#include "gdbusutils.h"
#include "gmountspec.h"

static void g_vfs_impl_daemon_class_init     (GVfsImplDaemonClass *class);
static void g_vfs_impl_daemon_vfs_iface_init (GVfsIface       *iface);
static void g_vfs_impl_daemon_finalize       (GObject         *object);

struct _GVfsImplDaemon
{
  GObject parent;

  DBusConnection *bus;
  
  GVfs *wrapped_vfs;
  GList *mount_cache;
};

static GVfsImplDaemon *the_vfs = NULL;
G_LOCK_DEFINE_STATIC(mount_cache);

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

static void
get_mountspec_from_uri (GDecodedUri *uri,
			GMountSpec **spec_out,
			char **path_out)
{
  GMountSpec *spec;
  char *path, *tmp;
  const char *share, *share_end;
  
  /* TODO: Share MountSpec objects between files (its refcounted) */

  /* TODO: Make less hardcoded */
  
  spec = NULL;
  
  if (strcmp (uri->scheme, "test") == 0)
    {
      spec = g_mount_spec_new ("test");
      path = g_strdup (uri->path);
    }
  else if (strcmp (uri->scheme, "smb") == 0)
    {
      if (uri->host != NULL && strlen (uri->host) > 0 &&
	  uri->path != NULL && uri->path[0] == '/' && strlen (uri->path) > 1)
	{
	  spec = g_mount_spec_new ("smb-share");
	  
	  g_mount_spec_add_item  (spec, "server", uri->host);

	  share = uri->path + 1;
	  share_end = strchr (share, '/');

	  if (share_end != NULL)
	    tmp = g_strndup (share, share_end - share);
	  else
	    tmp = g_strdup (share);

	  g_mount_spec_add_item  (spec, "share", tmp);
	  g_free (tmp);

	  if (share_end)
	    path = g_strdup (share_end);
	  else
	    path = g_strdup ("/");
	}
    }

  if (spec == NULL)
    {
      tmp = g_strdup_printf ("unknown-%s", uri->scheme);
      spec = g_mount_spec_new (tmp);
      g_free (tmp);
      path = g_strdup (uri->path);
    }
  
  *spec_out = spec;
  *path_out = path;
}

static void
g_vfs_impl_daemon_init (GVfsImplDaemon *vfs)
{
  g_assert (the_vfs == NULL);
  the_vfs = vfs;
  
  vfs->wrapped_vfs = g_vfs_local_new ();

  if (g_thread_supported ())
    dbus_threads_init_default ();
  
  vfs->bus = dbus_bus_get (DBUS_BUS_SESSION, NULL);

  if (vfs->bus)
    _g_dbus_connection_integrate_with_main (vfs->bus);
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

static GFile *
g_vfs_impl_daemon_get_file_for_uri (GVfs       *vfs,
				    const char *uri)
{
  GVfsImplDaemon *daemon_vfs;
  GFile *file, *wrapped;
  GDecodedUri *decoded;
  GMountSpec *spec;
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
      get_mountspec_from_uri (decoded, &spec, &path);
      file = g_file_daemon_new (spec, path);
      g_mount_spec_unref (spec);
      g_free (path);
    }

  _g_decoded_uri_free (decoded);
  
  return file;
}


static GMountInfo *
mount_info_ref (GMountInfo *info)
{
  g_atomic_int_inc (&info->ref_count);
  return info;
}


void
_g_mount_info_unref (GMountInfo *info)
{
  if (g_atomic_int_dec_and_test (&info->ref_count))
    {
      g_free (info->dbus_id);
      g_free (info->object_path);
      g_mount_spec_unref (info->spec);
      g_free (info);
    }
}

const char *
_g_mount_info_resolve_path (GMountInfo *info,
			    const char *path)
{
  const char *new_path;
  
  if (info->spec->mount_prefix != NULL &&
      info->spec->mount_prefix[0] != 0)
    new_path = path + strlen (info->spec->mount_prefix);
  else
    new_path = path;

  if (new_path == NULL ||
      new_path[0] == 0)
    new_path = "/";

  return new_path;
}

static GMountInfo *
lookup_mount_info_in_cache_locked (GMountSpec *spec,
				   const char *path)
{
  GMountInfo *info;
  GList *l;

  info = NULL;
  for (l = the_vfs->mount_cache; l != NULL; l = l->next)
    {
      GMountInfo *mount_info = l->data;

      if (g_mount_spec_match_with_path (mount_info->spec, spec, path))
	{
	  info = mount_info_ref (mount_info);
	  break;
	}
    }
  
  return info;
}

static GMountInfo *
lookup_mount_info_in_cache (GMountSpec *spec,
			    const char *path)
{
  GMountInfo *info;

  G_LOCK (mount_cache);
  info = lookup_mount_info_in_cache_locked (spec, path);
  G_UNLOCK (mount_cache);

  return info;
}

static GMountInfo *
handler_lookup_mount_reply (DBusMessage *reply,
			    GError **error)
{
  DBusError derror;
  GMountInfo *info;
  DBusMessageIter iter;
  const char *display_name, *icon, *obj_path, *dbus_id;
  GMountSpec *mount_spec;
  GList *l;

  dbus_error_init (&derror);
  
  if (dbus_set_error_from_message (&derror, reply))
    {
      _g_error_from_dbus (&derror, error);
      dbus_error_free (&derror);
      return NULL;
    }

  dbus_message_iter_init (reply, &iter);
  if (!_g_dbus_message_iter_get_args (&iter,
				      &derror,
				      DBUS_TYPE_STRING, &display_name,
				      DBUS_TYPE_STRING, &icon,
				      DBUS_TYPE_STRING, &dbus_id,
				      DBUS_TYPE_OBJECT_PATH, &obj_path,
				      0))
    {
      _g_error_from_dbus (&derror, error);
      dbus_error_free (&derror);
      return NULL;
    }

  mount_spec = g_mount_spec_from_dbus (&iter);
  if (mount_spec == NULL)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting mount info: %s",
		   "Invalid reply");
      return NULL;
    }

  G_LOCK (mount_cache);
  
  info = NULL;
  /* Already in cache from other thread? */
  for (l = the_vfs->mount_cache; l != NULL; l = l->next)
    {
      GMountInfo *mount_info = l->data;
      
      if (strcmp (mount_info->dbus_id, dbus_id) == 0 &&
	  strcmp (mount_info->object_path, obj_path) == 0)
	{
	  info = mount_info;
	  break;
	}
    }

  /* No, lets add it to the cache */
  if (info == NULL)
    {
      info = g_new0 (GMountInfo, 1);
      info->ref_count = 1;
      info->dbus_id = g_strdup (dbus_id);
      info->object_path = g_strdup (obj_path);
      info->spec = g_mount_spec_ref (mount_spec);

      the_vfs->mount_cache = g_list_prepend (the_vfs->mount_cache, info);
    }

  mount_info_ref (info);

  G_UNLOCK (mount_cache);

  g_mount_spec_unref (mount_spec);

  return info;
}

typedef struct {
  GMountInfoLookupCallback callback;
  gpointer user_data;
} GetMountInfoData;

static void
async_get_mount_info_response (DBusPendingCall *pending,
			       void            *_data)
{
  GetMountInfoData *data = _data;
  DBusMessage *reply;
  GError *error;
  GMountInfo *info;

  
  reply = dbus_pending_call_steal_reply (pending);
  dbus_pending_call_unref (pending);

  error = NULL;
  info = handler_lookup_mount_reply (reply, &error);

  dbus_message_unref (reply);
  
  data->callback (info, data->user_data, error);

  if (info)
    _g_mount_info_unref (info);
  
  if (error)
    g_error_free (error);
  
  g_free (data);
}

void
_g_vfs_impl_daemon_get_mount_info_async (GMountSpec *spec,
					 const char *path,
					 GMountInfoLookupCallback callback,
					 gpointer user_data)
{
  DBusPendingCall *pending;
  GMountInfo *info;
  GError *error;
  GetMountInfoData *data;
  DBusMessage *message;
  DBusMessageIter iter;
  
  info = lookup_mount_info_in_cache (spec, path);

  if (info != NULL)
    {
      callback (info, user_data, NULL);
      _g_mount_info_unref (info);
      return;
    }

  message =
    dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
				  G_VFS_DBUS_MOUNTTRACKER_PATH,
				  G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
				  G_VFS_DBUS_MOUNTTRACKER_OP_LOOKUP_MOUNT);
  dbus_message_set_auto_start (message, TRUE);
  
  dbus_message_iter_init_append (message, &iter);
  g_mount_spec_to_dbus_with_path (&iter, spec, path);

  if (!dbus_connection_send_with_reply (the_vfs->bus, message, &pending,
					1000))
    _g_dbus_oom ();

  dbus_message_unref (message);

  if (pending == NULL)
    {
      error = NULL;
      g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting peer-to-peer dbus connection: %s",
		   "Connection is closed");
      callback (NULL, user_data, error);
      return;
    }

  data = g_new0 (GetMountInfoData, 1);
  data->callback = callback;
  data->user_data = user_data;
  
  if (!dbus_pending_call_set_notify (pending,
				     async_get_mount_info_response,
				     data,
				     NULL))
    _g_dbus_oom ();
}


GMountInfo *
_g_vfs_impl_daemon_get_mount_info_sync (GMountSpec *spec,
					const char *path,
					GError **error)
{
  GMountInfo *info;
  DBusConnection *conn;
  DBusMessage *message, *reply;
  DBusMessageIter iter;
  DBusError derror;
	
  info = lookup_mount_info_in_cache (spec, path);

  if (info != NULL)
    return info;
  
  conn = _g_dbus_connection_get_sync (NULL, error);
  if (conn == NULL)
    return NULL;

  message =
    dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
				  G_VFS_DBUS_MOUNTTRACKER_PATH,
				  G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
				  G_VFS_DBUS_MOUNTTRACKER_OP_LOOKUP_MOUNT);
  dbus_message_set_auto_start (message, TRUE);
  
  dbus_message_iter_init_append (message, &iter);
  g_mount_spec_to_dbus_with_path (&iter, spec, path);

  dbus_error_init (&derror);
  reply = dbus_connection_send_with_reply_and_block (conn, message, -1, &derror);
  dbus_message_unref (message);

  if (!reply)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   "Error while getting mount info: %s",
		   derror.message);
      dbus_error_free (&derror);
      return NULL;
    }

  info = handler_lookup_mount_reply (reply, error);

  dbus_message_unref (reply);
  
  return info;
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

/* Module API */

GVfs * create_vfs (void);

GVfs *
create_vfs (void)
{
  return G_VFS (g_vfs_impl_daemon_new ());
}
