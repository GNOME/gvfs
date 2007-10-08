#include <config.h>
#include <string.h>
#include <dbus/dbus.h>
#include "gdaemonvfs.h"
#include "gvfsuriutils.h"
#include "gdaemonfile.h"
#include <gio/giomodule.h>
#include <gio/gdummyfile.h>
#include <gvfsdaemonprotocol.h>
#include <gmodule.h>
#include "gvfsdaemondbus.h"
#include "gdbusutils.h"
#include "gmountspec.h"
#include "gvfsurimapper.h"

#define G_TYPE_DAEMON_VFS		(g_daemon_vfs_type)
#define G_DAEMON_VFS(obj)		(G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_DAEMON_VFS, GDaemonVfs))
#define G_DAEMON_VFS_CLASS(klass)	(G_TYPE_CHECK_CLASS_CAST ((klass), G_TYPE_DAEMON_VFS, GDaemonVfsClass))
#define G_IS_DAEMON_VFS(obj)		(G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_DAEMON_VFS))
#define G_IS_DAEMON_VFS_CLASS(klass)	(G_TYPE_CHECK_CLASS_TYPE ((klass), G_TYPE_DAEMON_VFS))
#define G_DAEMON_VFS_GET_CLASS(obj)	(G_TYPE_INSTANCE_GET_CLASS ((obj), G_TYPE_DAEMON_VFS, GDaemonVfsClass))

static void g_daemon_vfs_class_init     (GDaemonVfsClass *class);
static void g_daemon_vfs_finalize       (GObject         *object);
static void g_daemon_vfs_init           (GDaemonVfs      *vfs);

struct _GDaemonVfs
{
  GVfs parent;

  DBusConnection *async_bus;
  
  GVfs *wrapped_vfs;
  GList *mount_cache;

  GHashTable *from_uri_hash;
  GHashTable *to_uri_hash;

  gchar **supported_uri_schemes;
};

struct _GDaemonVfsClass
{
  GVfsClass parent_class;
};

static GType g_daemon_vfs_type = 0;
static GDaemonVfs *the_vfs = NULL;
static GObjectClass *g_daemon_vfs_parent_class = NULL;

G_LOCK_DEFINE_STATIC(mount_cache);

GType
g_daemon_vfs_get_type (GTypeModule *module)
{
  if (!g_daemon_vfs_type)
    {
      static const GTypeInfo type_info =
	{
	  sizeof (GDaemonVfsClass),
	  (GBaseInitFunc) NULL,
	  (GBaseFinalizeFunc) NULL,
	  (GClassInitFunc) g_daemon_vfs_class_init,
	  NULL,           /* class_finalize */
	  NULL,           /* class_data     */
	  sizeof (GDaemonVfs),
	  0,              /* n_preallocs    */
	  (GInstanceInitFunc) g_daemon_vfs_init
	};

      g_daemon_vfs_type =
        g_type_module_register_type (module, G_TYPE_VFS,
                                     "GDaemonVfs", &type_info, 0);
    }
  
  return g_daemon_vfs_type;
}

static void
g_daemon_vfs_finalize (GObject *object)
{
  GDaemonVfs *vfs;

  vfs = G_DAEMON_VFS (object);

  g_hash_table_destroy (vfs->from_uri_hash);
  g_hash_table_destroy (vfs->to_uri_hash);

  g_strfreev (vfs->supported_uri_schemes);

  if (vfs->async_bus)
    {
      dbus_connection_close (vfs->async_bus);
      dbus_connection_unref (vfs->async_bus);
    }


  /* must chain up */
  G_OBJECT_CLASS (g_daemon_vfs_parent_class)->finalize (object);
}

static gboolean
get_mountspec_from_uri (GDaemonVfs *vfs,
			const char *uri,
			GMountSpec **spec_out,
			char **path_out)
{
  GMountSpec *spec;
  char *path;
  GVfsUriMapper *mapper;
  char *scheme;

  scheme = g_uri_get_scheme (uri);
  if (scheme == NULL)
    return FALSE;
  
  spec = NULL;
  
  mapper = g_hash_table_lookup (vfs->from_uri_hash, scheme);
  if (mapper == NULL ||
      !g_vfs_uri_mapper_from_uri (mapper, uri, &spec, &path))
    {
      GDecodedUri *decoded;
      
      decoded = g_decode_uri (uri);
      if (decoded)
	{
	  spec = g_mount_spec_new (decoded->scheme);
	  
	  if (decoded->host && *decoded->host)
	    g_mount_spec_set (spec, "host", decoded->host);
	  
	  if (decoded->userinfo && *decoded->userinfo)
	    g_mount_spec_set (spec, "user", decoded->userinfo);
	  
	  if (decoded->port != -1)
	    {
	      char *port = g_strdup_printf ("%d", decoded->port);
	      g_mount_spec_set (spec, "port", port);
	      g_free (port);
	    }
	  
	  path = g_strdup (decoded->path);
	}
    }
  
  g_free (scheme);
  
  if (spec == NULL)
    return FALSE;

  *spec_out = g_mount_spec_get_unique_for (spec);
  g_mount_spec_unref (spec);
  *path_out = path;
  
  return TRUE;
}

static void
g_daemon_vfs_init (GDaemonVfs *vfs)
{
  GType *mappers;
  guint n_mappers;
  const char * const *schemes, * const *mount_types;
  GVfsUriMapper *mapper;
  int i;
  
  g_assert (the_vfs == NULL);
  the_vfs = vfs;

  vfs->wrapped_vfs = g_vfs_get_local ();

  if (g_thread_supported ())
    dbus_threads_init_default ();
  
  vfs->async_bus = dbus_bus_get_private (DBUS_BUS_SESSION, NULL);

  if (vfs->async_bus)
    _g_dbus_connection_integrate_with_main (vfs->async_bus);

  g_io_modules_ensure_loaded (GVFS_MODULE_DIR);

  vfs->from_uri_hash = g_hash_table_new (g_str_hash, g_str_equal);
  vfs->to_uri_hash = g_hash_table_new (g_str_hash, g_str_equal);
  
  mappers = g_type_children (G_VFS_TYPE_URI_MAPPER, &n_mappers);
  for (i = 0; i < n_mappers; i++)
    {
      mapper = g_object_new (mappers[i], NULL);

      schemes = g_vfs_uri_mapper_get_handled_schemes (mapper);

      for (i = 0; schemes != NULL && schemes[i] != NULL; i++)
	g_hash_table_insert (vfs->from_uri_hash, (char *)schemes[i], mapper);
      
      mount_types = g_vfs_uri_mapper_get_handled_mount_types (mapper);
      for (i = 0; mount_types != NULL && mount_types[i] != NULL; i++)
	g_hash_table_insert (vfs->to_uri_hash, (char *)mount_types[i], mapper);
    }
  
  g_free (mappers);
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
  GMountSpec *spec;
  char *path;

  daemon_vfs = G_DAEMON_VFS (vfs);

  if (g_str_has_prefix (uri, "file:"))
    {
      path = g_filename_from_uri (uri, NULL, NULL);

      if (path == NULL)
	return g_dummy_file_new (uri);
      
      file = g_daemon_vfs_get_file_for_path (vfs, path);
      g_free (path);
      return file;
    }
  
  if (get_mountspec_from_uri (daemon_vfs, uri, &spec, &path))
    {
      file = g_daemon_file_new (spec, path);
      g_mount_spec_unref (spec);
      g_free (path);
      return file;
    }
  
  return g_dummy_file_new (uri);
}

char *
_g_daemon_vfs_get_uri_for_mountspec (GMountSpec *spec,
				     char *path,
				     gboolean allow_utf8)
{
  char *uri;
  const char *type;
  GVfsUriMapper *mapper;

  type = g_mount_spec_get_type (spec);
  if (type == NULL)
    {
      GString *string = g_string_new ("unknown://");
      if (path)
	g_string_append_uri_encoded (string,
				     path,
				     "!$&'()*+,;=:@/",
				     allow_utf8);
      
      return g_string_free (string, FALSE);
    }

  uri = NULL;
  mapper = g_hash_table_lookup (the_vfs->to_uri_hash, type);
  if (mapper)
    uri = g_vfs_uri_mapper_to_uri (mapper, spec, path, allow_utf8);

  if (uri == NULL)
    {
      GString *string = g_string_new ("");
      g_string_append (string, type);
      g_string_append (string, "://");
      if (path)
	g_string_append_uri_encoded (string,
				     path,
				     "!$&'()*+,;=:@/",
				     allow_utf8);
      
      uri = g_string_free (string, FALSE);
    }
  
  return uri;
}

static void
fill_supported_uri_schemes (GDaemonVfs *vfs)
{
  DBusConnection *connection;
  DBusMessage *message, *reply;
  DBusError error;
  DBusMessageIter iter, array_iter;
  gint i, count;
  GList *l, *list = NULL;

  connection = dbus_bus_get (DBUS_BUS_SESSION, NULL);

  
  message = dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
                                          G_VFS_DBUS_MOUNTTRACKER_PATH,
					  G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					  G_VFS_DBUS_MOUNTTRACKER_OP_LIST_MOUNT_TYPES);

  if (message == NULL)
    _g_dbus_oom ();
  
  dbus_message_set_auto_start (message, TRUE);
  
  dbus_error_init (&error);
  reply = dbus_connection_send_with_reply_and_block (connection,
                                                     message,
						     G_VFS_DBUS_TIMEOUT_MSECS,
						     &error);
  dbus_message_unref (message);

  if (dbus_error_is_set (&error))
    {
      dbus_error_free (&error);
      dbus_connection_unref (connection);
      return;
    }

  if (reply == NULL)
    _g_dbus_oom ();

  dbus_message_iter_init (reply, &iter);
  g_assert (dbus_message_iter_get_element_type (&iter) == DBUS_TYPE_STRING);

  dbus_message_iter_recurse (&iter, &array_iter);
  
  count = 0;
  do
    {
      gchar *type, *scheme = NULL;
      GVfsUriMapper *mapper = NULL;
      GMountSpec *spec;
      gboolean new = TRUE;

      dbus_message_iter_get_basic (&array_iter, &type);

      spec = g_mount_spec_new (type);

      mapper = g_hash_table_lookup (vfs->to_uri_hash, type);
    
      if (mapper)
        scheme = g_vfs_uri_mapper_to_uri_scheme (mapper, spec);

      if (scheme == NULL)
        scheme = g_strdup (type);

      for (l = list; l != NULL; l = l->next)
        {
          if (strcmp (l->data, scheme) == 0)
            {
              new = FALSE;
              break;
            }
        }

      if (new)
        {
          list = g_list_prepend (list, scheme);
          count++;
        }
      else
        {
          g_free (scheme);
        }

      g_mount_spec_unref (spec);
    }
  while (dbus_message_iter_next (&array_iter));

  dbus_message_unref (reply);
  dbus_connection_unref (connection);

  list = g_list_prepend (list, g_strdup ("file"));
  list = g_list_sort (list, (GCompareFunc) strcmp);

  vfs->supported_uri_schemes = g_new0 (gchar *, count + 2);

  for (i = 0, l = list; l != NULL; l = l->next, i++)
    vfs->supported_uri_schemes[i] = l->data;

  g_list_free (list);
}

static const gchar * const *
g_daemon_vfs_get_supported_uri_schemes (GVfs *vfs)
{
  GDaemonVfs *daemon_vfs = G_DAEMON_VFS (vfs);

  if (!daemon_vfs->supported_uri_schemes)
    fill_supported_uri_schemes (daemon_vfs);
    
  return (const gchar * const *) G_DAEMON_VFS (vfs)->supported_uri_schemes;
}

GMountRef *
_g_mount_ref_ref (GMountRef *ref)
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
      g_free (ref->prefered_filename_encoding);
      g_free (ref);
    }
}

const char *
_g_mount_ref_resolve_path (GMountRef *ref,
			   const char *path)
{
  const char *new_path;
  int len;

  if (ref->spec->mount_prefix != NULL &&
      ref->spec->mount_prefix[0] != 0)
    {
      len = strlen (ref->spec->mount_prefix);
      if (ref->spec->mount_prefix[len-1] == '/')
	len--;
      new_path = path + len;
    }
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
	  ref = _g_mount_ref_ref (mount_ref);
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
  const char *display_name, *icon, *obj_path, *dbus_id, *prefered_filename_encoding;
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
				      DBUS_TYPE_STRING, &prefered_filename_encoding,
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
      if (prefered_filename_encoding != NULL && *prefered_filename_encoding != 0) 
	ref->prefered_filename_encoding = g_strdup (prefered_filename_encoding);

      the_vfs->mount_cache = g_list_prepend (the_vfs->mount_cache, ref);
    }

  _g_mount_ref_ref (ref);

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
  
  _g_dbus_connection_call_async (the_vfs->async_bus, message, 2000,
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

DBusConnection *
_g_daemon_vfs_get_async_bus (void)
{
  return the_vfs->async_bus;
}

static const char *
g_daemon_vfs_get_name (GVfs *vfs)
{
  return "gvfs";
}

static int
g_daemon_vfs_get_priority (GVfs *vfs)
{
  return 10;
}

static void
g_daemon_vfs_class_init (GDaemonVfsClass *class)
{
  GObjectClass *object_class;
  GVfsClass *vfs_class;
  
  object_class = (GObjectClass *) class;

  g_daemon_vfs_parent_class = g_type_class_peek_parent (class);
  
  object_class->finalize = g_daemon_vfs_finalize;

  vfs_class = G_VFS_CLASS (class);
  vfs_class->get_name = g_daemon_vfs_get_name;
  vfs_class->get_priority = g_daemon_vfs_get_priority;
  vfs_class->get_file_for_path = g_daemon_vfs_get_file_for_path;
  vfs_class->get_file_for_uri = g_daemon_vfs_get_file_for_uri;
  vfs_class->get_supported_uri_schemes = g_daemon_vfs_get_supported_uri_schemes;
  vfs_class->parse_name = g_daemon_vfs_parse_name;
}

/* Module API */

void
g_io_module_load (GIOModule *module)
{
  g_daemon_vfs_get_type (G_TYPE_MODULE (module));
}

void
g_io_module_unload (GIOModule   *module)
{
}
