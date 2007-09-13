#include <config.h>
#include <string.h>
#include <dbus/dbus.h>
#include "gdaemonvfs.h"
#include "gvfsuriutils.h"
#include "gdaemonfile.h"
#include <gio/glocalvfs.h>
#include <gvfsdaemonprotocol.h>
#include "gvfsdaemondbus.h"
#include "gdbusutils.h"
#include "gmountspec.h"

static void g_daemon_vfs_class_init     (GDaemonVfsClass *class);
static void g_daemon_vfs_vfs_iface_init (GVfsIface       *iface);
static void g_daemon_vfs_finalize       (GObject         *object);

struct _GDaemonVfs
{
  GObject parent;

  DBusConnection *bus;
  
  GVfs *wrapped_vfs;
  GList *mount_cache;
};

static GDaemonVfs *the_vfs = NULL;
G_LOCK_DEFINE_STATIC(mount_cache);

G_DEFINE_TYPE_WITH_CODE (GDaemonVfs, g_daemon_vfs, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_VFS,
						g_daemon_vfs_vfs_iface_init))
 
static void
g_daemon_vfs_class_init (GDaemonVfsClass *class)
{
  GObjectClass *object_class;
  
  object_class = (GObjectClass *) class;

  object_class->finalize = g_daemon_vfs_finalize;
}

static void
g_daemon_vfs_finalize (GObject *object)
{
  /* must chain up */
  G_OBJECT_CLASS (g_daemon_vfs_parent_class)->finalize (object);
}

static void
get_mountspec_from_uri (GDecodedUri *uri,
			GMountSpec **spec_out,
			char **path_out)
{
  GMountSpec *spec;
  char *path, *tmp;
  const char *p;
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
      if (uri->host == NULL || strlen (uri->host) == 0)
	{
	  /* uri form: smb:/// or smb:///$path */
	  spec = g_mount_spec_new ("smb-network");
	  if (uri->path == NULL || *uri->path == 0)
	    path = g_strdup ("/");
	  else
	    path = g_strdup (uri->path);
	}
      else
	{
	  /* host set */
	  p = uri->path;
	  while (p && *p == '/')
	    p++;

	  if (p == NULL || *p == 0)
	    {
	      /* uri form: smb://$host/ */
	      spec = g_mount_spec_new ("smb-server");
	      tmp = g_ascii_strdown (uri->host, -1);
	      g_mount_spec_set  (spec, "server", tmp);
	      g_free (tmp);
	      path = g_strdup ("/");
	    }
	  else
	    {
	      share = p;
	      share_end = strchr (share, '/');
	      if (share_end == NULL)
		share_end = share + strlen (share);

	      p = share_end;
	      
	      while (*p == '/')
		p++;

	      if (*p == 0)
		{
		  /* uri form: smb://$host/$share/
		   * Here we special case smb-server files by adding "##" to the names in the uri */
		  if (share[0] == '#' && share[1] == '#')
		    {
		      spec = g_mount_spec_new ("smb-server");
		      tmp = g_ascii_strdown (uri->host, -1);
		      g_mount_spec_set  (spec, "server", tmp);
		      g_free (tmp);
		      tmp = g_strndup (share+2, share_end - share);
		      path = g_strconcat ("/", tmp, NULL);
		      g_free (tmp);
		    }
		  else
		    {
		      spec = g_mount_spec_new ("smb-share");
		      tmp = g_ascii_strdown (uri->host, -1);
		      g_mount_spec_set  (spec, "server", tmp);
		      g_free (tmp);
		      g_mount_spec_set_with_len  (spec, "share", share, share_end - share);
		      path = g_strdup ("/");
		    }
		}
	      else
		{
		  spec = g_mount_spec_new ("smb-share");
		  
		  tmp = g_ascii_strdown (uri->host, -1);
		  g_mount_spec_set  (spec, "server", tmp);
		  g_free (tmp);

		  g_mount_spec_set_with_len  (spec, "share", share, share_end - share);

		  path = g_strconcat ("/", p, NULL);
		}
	    }
	}
      if (uri->userinfo)
	{
	  const char *user = uri->userinfo;
	  p = strchr (uri->userinfo, ';');
	  if (p)
	    {
	      if (p != user)
		g_mount_spec_set_with_len  (spec, "domain", user, p - user);
	      user = p + 1;
	    }
	  if (*user != 0)
	    g_mount_spec_set  (spec, "user", user);
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
g_daemon_vfs_init (GDaemonVfs *vfs)
{
  g_assert (the_vfs == NULL);
  the_vfs = vfs;
  
  vfs->wrapped_vfs = g_local_vfs_new ();

  if (g_thread_supported ())
    dbus_threads_init_default ();
  
  vfs->bus = dbus_bus_get (DBUS_BUS_SESSION, NULL);

  if (vfs->bus)
    _g_dbus_connection_integrate_with_main (vfs->bus);
}

GDaemonVfs *
g_daemon_vfs_new (void)
{
  return g_object_new (G_TYPE_DAEMON_VFS, NULL);
}

static GFile *
g_daemon_vfs_get_file_for_path (GVfs       *vfs,
				const char *path)
{
  /* TODO: detect fuse paths and convert to daemon vfs GFiles */
  
  return g_vfs_get_file_for_path (G_DAEMON_VFS (vfs)->wrapped_vfs, path);
}

static GFile *
g_daemon_vfs_get_file_for_uri (GVfs       *vfs,
			       const char *uri)
{
  GDaemonVfs *daemon_vfs;
  GFile *file;
  GDecodedUri *decoded;
  GMountSpec *spec;
  char *path;

  daemon_vfs = G_DAEMON_VFS (vfs);
  
  decoded = _g_decode_uri (uri);
  if (decoded == NULL)
    return NULL;

  if (strcmp (decoded->scheme, "file") == 0)
    file = g_daemon_vfs_get_file_for_path (vfs, decoded->path);
  else
    {
      get_mountspec_from_uri (decoded, &spec, &path);
      file = g_daemon_file_new (spec, path);
      g_mount_spec_unref (spec);
      g_free (path);
    }

  _g_decoded_uri_free (decoded);
  
  return file;
}


static GMountRef *
mount_ref_ref (GMountRef *ref)
{
  g_atomic_int_inc (&ref->ref_count);
  return ref;
}


void
_g_mount_ref_unref (GMountRef *ref)
{
  if (g_atomic_int_dec_and_test (&ref->ref_count))
    {
      g_free (ref->dbus_id);
      g_free (ref->object_path);
      g_mount_spec_unref (ref->spec);
      g_free (ref);
    }
}

const char *
_g_mount_ref_resolve_path (GMountRef *ref,
			    const char *path)
{
  const char *new_path;
  
  if (ref->spec->mount_prefix != NULL &&
      ref->spec->mount_prefix[0] != 0)
    new_path = path + strlen (ref->spec->mount_prefix);
  else
    new_path = path;

  if (new_path == NULL ||
      new_path[0] == 0)
    new_path = "/";

  return new_path;
}

static GMountRef *
lookup_mount_ref_in_cache_locked (GMountSpec *spec,
				  const char *path)
{
  GMountRef *ref;
  GList *l;

  ref = NULL;
  for (l = the_vfs->mount_cache; l != NULL; l = l->next)
    {
      GMountRef *mount_ref = l->data;

      if (g_mount_spec_match_with_path (mount_ref->spec, spec, path))
	{
	  ref = mount_ref_ref (mount_ref);
	  break;
	}
    }
  
  return ref;
}

static GMountRef *
lookup_mount_ref_in_cache (GMountSpec *spec,
			   const char *path)
{
  GMountRef *ref;

  G_LOCK (mount_cache);
  ref = lookup_mount_ref_in_cache_locked (spec, path);
  G_UNLOCK (mount_cache);

  return ref;
}

static GMountRef *
handler_lookup_mount_reply (DBusMessage *reply,
			    GError **error)
{
  DBusError derror;
  GMountRef *ref;
  DBusMessageIter iter, struct_iter;
  const char *display_name, *icon, *obj_path, *dbus_id;
  GMountSpec *mount_spec;
  GList *l;

  if (_g_error_from_message (reply, error))
    return NULL;

  dbus_error_init (&derror);
  dbus_message_iter_init (reply, &iter);

  dbus_message_iter_recurse (&iter, &struct_iter);

  if (!_g_dbus_message_iter_get_args (&struct_iter,
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

  mount_spec = g_mount_spec_from_dbus (&struct_iter);
  if (mount_spec == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "Error while getting mount info: %s",
		   "Invalid reply");
      return NULL;
    }

  G_LOCK (mount_cache);
  
  ref = NULL;
  /* Already in cache from other thread? */
  for (l = the_vfs->mount_cache; l != NULL; l = l->next)
    {
      GMountRef *mount_ref = l->data;
      
      if (strcmp (mount_ref->dbus_id, dbus_id) == 0 &&
	  strcmp (mount_ref->object_path, obj_path) == 0)
	{
	  ref = mount_ref;
	  break;
	}
    }

  /* No, lets add it to the cache */
  if (ref == NULL)
    {
      ref = g_new0 (GMountRef, 1);
      ref->ref_count = 1;
      ref->dbus_id = g_strdup (dbus_id);
      ref->object_path = g_strdup (obj_path);
      ref->spec = g_mount_spec_ref (mount_spec);

      the_vfs->mount_cache = g_list_prepend (the_vfs->mount_cache, ref);
    }

  mount_ref_ref (ref);

  G_UNLOCK (mount_cache);

  g_mount_spec_unref (mount_spec);

  return ref;
}

typedef struct {
  GMountRefLookupCallback callback;
  gpointer user_data;
} GetMountRefData;

static void
async_get_mount_ref_response (DBusMessage *reply,
			      GError *io_error,
			      void *_data)
{
  GetMountRefData *data = _data;
  GMountRef *ref;
  GError *error;

  if (reply == NULL)
    data->callback (NULL, data->user_data, io_error);
  else
    {
      error = NULL;
      ref = handler_lookup_mount_reply (reply, &error);

      data->callback (ref, data->user_data, error);

      if (ref)
	_g_mount_ref_unref (ref);

      if (error)
	g_error_free (error);
    }
  
  g_free (data);
}

void
_g_daemon_vfs_get_mount_ref_async (GMountSpec *spec,
				   const char *path,
				   GMountRefLookupCallback callback,
				   gpointer user_data)
{
  GMountRef *ref;
  GetMountRefData *data;
  DBusMessage *message;
  DBusMessageIter iter;
  
  ref = lookup_mount_ref_in_cache (spec, path);

  if (ref != NULL)
    {
      callback (ref, user_data, NULL);
      _g_mount_ref_unref (ref);
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

  data = g_new0 (GetMountRefData, 1);
  data->callback = callback;
  data->user_data = user_data;
  
  _g_dbus_connection_call_async (the_vfs->bus, message, 2000,
				 async_get_mount_ref_response,
				 data);
  
  dbus_message_unref (message);
}


GMountRef *
_g_daemon_vfs_get_mount_ref_sync (GMountSpec *spec,
				  const char *path,
				  GError **error)
{
  GMountRef *ref;
  DBusConnection *conn;
  DBusMessage *message, *reply;
  DBusMessageIter iter;
  DBusError derror;
	
  ref = lookup_mount_ref_in_cache (spec, path);

  if (ref != NULL)
    return ref;
  
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
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		   "Error while getting mount info: %s",
		   derror.message);
      dbus_error_free (&derror);
      return NULL;
    }

  ref = handler_lookup_mount_reply (reply, error);

  dbus_message_unref (reply);
  
  return ref;
}

static GFile *
g_daemon_vfs_parse_name (GVfs       *vfs,
			      const char *parse_name)
{
  GFile *file;
  char *path;
  
  if (g_path_is_absolute (parse_name))
    {
      path = g_filename_from_utf8 (parse_name, -1, NULL, NULL, NULL);
      file = g_daemon_vfs_get_file_for_path  (vfs, path);
      g_free (path);
    }
  else
    {
      file = g_daemon_vfs_get_file_for_uri (vfs, parse_name);
    }

  return file;
}

static void
g_daemon_vfs_vfs_iface_init (GVfsIface *iface)
{
  iface->get_file_for_path = g_daemon_vfs_get_file_for_path;
  iface->get_file_for_uri = g_daemon_vfs_get_file_for_uri;
  iface->parse_name = g_daemon_vfs_parse_name;
}

/* Module API */

GVfs * create_vfs (void);

GVfs *
create_vfs (void)
{
  return G_VFS (g_daemon_vfs_new ());
}
