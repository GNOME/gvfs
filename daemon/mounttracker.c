#include <config.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "mounttracker.h"
#include "gdbusutils.h"
#include "gfileref.h"

typedef struct {
  char *display_name;
  char *dbus_id;
  char *object_path;
} VFSMount;

typedef struct {
  GFileRefTemplate *template;
  char *path;
  VFSMount *mount;
} VFSMountMap;

typedef struct {
  GFileRefTemplate *template;
  char *dbus_id;
  char *object_path;
  gboolean automount;
} DBusVFSMountPoint;

typedef struct {
  GFileRefTemplate *template;
  gboolean automount;
  char *exec;
} ExecVFSMountPoint;

struct _GMountTracker
{
  GObject parent_instance;

  GList *mounts;
  GList *mappings;
  GList *exec_mountpoints;
  GList *dbus_mountpoints;
};

G_DEFINE_TYPE (GMountTracker, g_mount_tracker, G_TYPE_OBJECT);

static GFileRefTemplate *
template_from_keyfile (GKeyFile *keyfile, const char *group)
{
  GFileRefTemplate *template;

  template = g_new0 (GFileRefTemplate, 1);
  if (g_key_file_has_key (keyfile, group, "protocol", NULL))
    template->protocol = g_key_file_get_string (keyfile, group, "protocol", NULL);
  
  if (g_key_file_has_key (keyfile, group, "user", NULL))
    template->username = g_key_file_get_string (keyfile, group, "user", NULL);
  
  if (g_key_file_has_key (keyfile, group, "host", NULL))
    template->host = g_key_file_get_string (keyfile, group, "host", NULL);

  template->port = G_FILE_REF_PORT_ANY;
  if (g_key_file_has_key (keyfile, group, "port", NULL))
    template->port = g_key_file_get_integer (keyfile, group, "port", NULL);

  if (g_key_file_has_key (keyfile, group, "pathprefix", NULL))
    template->path_prefix = g_key_file_get_string (keyfile, group, "pathprefix", NULL);
  
  if (g_key_file_has_key (keyfile, group, "maxdepth", NULL))
    template->max_path_depth = g_key_file_get_integer (keyfile, group, "maxdepth", NULL);
  
  if (g_key_file_has_key (keyfile, group, "mindepth", NULL))
    template->min_path_depth = g_key_file_get_integer (keyfile, group, "mindepth", NULL);
  
  return template;
}

static VFSMount *
find_vfs_mount (GMountTracker *tracker,
		const char *dbus_id,
		const char *obj_path)
{
  GList *l;
  for (l = tracker->mounts; l != NULL; l = l->next)
    {
      VFSMount *mount = l->data;

      if (strcmp (mount->dbus_id, dbus_id) == 0 &&
	  strcmp (mount->object_path, obj_path) == 0)
	return mount;
    }
  
  return NULL;
}

static void
vfs_mount_free (VFSMount *mount)
{
  g_free (mount->dbus_id);
  g_free (mount->object_path);
  g_free (mount);
}

static void
vfs_mount_map_free (VFSMountMap *map)
{
  g_file_ref_template_free (map->template);
  g_free (map->path);
  g_free (map);
}

static void
dbus_vfs_mount_point_free (DBusVFSMountPoint *point)
{
  g_file_ref_template_free (point->template);
  g_free (point->dbus_id);
  g_free (point->object_path);
  g_free (point);
}

static void
exec_vfs_mount_point_free (ExecVFSMountPoint *point)
{
  g_file_ref_template_free (point->template);
  g_free (point->exec);
  g_free (point);
}

static void
g_mount_tracker_finalize (GObject *object)
{
  GMountTracker *tracker;

  tracker = G_MOUNT_TRACKER (object);

  g_list_foreach (tracker->mounts, (GFunc)vfs_mount_free, NULL);
  g_list_free (tracker->mounts);

  g_list_foreach (tracker->mappings, (GFunc)vfs_mount_map_free, NULL);
  g_list_free (tracker->mappings);
  
  g_list_foreach (tracker->exec_mountpoints, (GFunc)exec_vfs_mount_point_free, NULL);
  g_list_free (tracker->exec_mountpoints);
  
  g_list_foreach (tracker->dbus_mountpoints, (GFunc)dbus_vfs_mount_point_free, NULL);
  g_list_free (tracker->dbus_mountpoints);
  
  if (G_OBJECT_CLASS (g_mount_tracker_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_mount_tracker_parent_class)->finalize) (object);
}

static void
g_mount_tracker_class_init (GMountTrackerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_mount_tracker_finalize;
}

static void
load_exec_mountpoints (GMountTracker *tracker)
{
  GDir *dir;
  const char *filename;
  char *path;
  GKeyFile *keyfile;
  ExecVFSMountPoint *point;
  
  dir = g_dir_open (MOUNTPOINT_DIR, 0, NULL);
  if (dir)
    {
      filename = g_dir_read_name (dir);
      path = g_build_filename (MOUNTPOINT_DIR, filename, NULL);
      keyfile = g_key_file_new ();
      
      if (g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, NULL))
	{
	  point = g_new0 (ExecVFSMountPoint, 1);
	  point->exec = g_key_file_get_string (keyfile, "vfshandler", "exec", NULL);
	  point->automount = g_key_file_get_boolean (keyfile, "vfshandler", "automount", NULL);
	  point->template = template_from_keyfile (keyfile, "vfshandler");

	  tracker->exec_mountpoints = g_list_prepend (tracker->exec_mountpoints, point);
	}
      
      g_key_file_free (keyfile);
      g_free (path);
    }
}

#if 0
static GFileRefTemplate *
g_file_ref_template_from_dbus (DBusMessageIter *iter)
{
  return NULL;
}
#endif

static void
register_mount (GMountTracker *tracker,
		DBusConnection *connection,
		DBusMessage *message)
{
  VFSMount *mount;
  DBusMessage *reply;
  DBusError error;
  const char *name, *obj_path, *id;


  id = dbus_message_get_sender (message);

  dbus_error_init (&error);
  if (dbus_message_get_args (message,
			     &error,
			     DBUS_TYPE_STRING, &name,
			     DBUS_TYPE_OBJECT_PATH, &obj_path,
			     0))
    {
      if (find_vfs_mount (tracker, id, obj_path) != NULL)
	reply = dbus_message_new_error (message,
					DBUS_ERROR_INVALID_SIGNATURE,
					"Wrong arguments to registerMount");
      else
	{      
	  mount = g_new0 (VFSMount, 1);
	  mount->display_name = g_strdup (name);
	  mount->dbus_id = g_strdup (id);
	  mount->object_path = g_strdup (obj_path);
	  
	  tracker->mounts = g_list_prepend (tracker->mounts, mount);

	  reply = dbus_message_new_method_return (message);
	}
    }
  else
    {
      reply = dbus_message_new_error (message,
				      error.name, error.message);
      dbus_error_free (&error);
    }
  
  if (reply == NULL)
    _g_dbus_oom ();
  
  dbus_connection_send (connection, reply, NULL);
}

static void
register_mount_map (GMountTracker *tracker,
		    DBusConnection *connection,
		    DBusMessage *message)
{
  DBusMessageIter iter;
  VFSMountMap *map;
  VFSMount *mount;
  DBusMessage *reply;
  const char *obj_path, *id, *path;
  int path_len;
  DBusError error;
  GFileRefTemplate *template;

  id = dbus_message_get_sender (message);
  dbus_message_iter_init (message, &iter);

  reply = NULL;
  dbus_error_init (&error);
  if (_g_dbus_message_iter_get_args (&iter,
				     &error,
				     DBUS_TYPE_OBJECT_PATH, &obj_path,
				     DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
				     &path, &path_len,
				     0))
    {
      mount = find_vfs_mount (tracker, id, obj_path);
      if (mount != NULL)
	{
	  template = g_file_ref_template_from_dbus (&iter);
	  
	  if (template)
	    {
	      map = g_new0 (VFSMountMap, 1);
	      map->mount = mount;
	      map->path = g_strndup (path, path_len);
	      map->template = template;
	      
	      tracker->mappings = g_list_prepend (tracker->mappings, map);
	      
	      reply = dbus_message_new_method_return (message);
	    }
	}
      
      if (reply == NULL)
	reply = dbus_message_new_error (message,
					DBUS_ERROR_INVALID_SIGNATURE,
					"Wrong arguments to registerMount");
    }
  else
    {
      reply = dbus_message_new_error (message,
				      error.name, error.message);
      dbus_error_free (&error);
    }
  
  if (reply == NULL)
    _g_dbus_oom ();
  
  dbus_connection_send (connection, reply, NULL);
}

static DBusHandlerResult
dbus_message_function (DBusConnection  *connection,
		       DBusMessage     *message,
		       void            *user_data)
{
  GMountTracker *tracker = user_data;

  DBusHandlerResult res;
  
  /* API:
     registerMount(template, obj_path, prefix_path)
  */

  res = DBUS_HANDLER_RESULT_HANDLED;
  if (dbus_message_is_method_call (message,
				   "org.gtk.gvfs.MountTracker",
				   "registerMount"))
    register_mount (tracker, connection, message);
  else if (dbus_message_is_method_call (message,
				   "org.gtk.gvfs.MountTracker",
				   "registerMountMap"))
    register_mount_map (tracker, connection, message);
  else
    res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;  ;
  
  return res;
}

struct DBusObjectPathVTable tracker_dbus_vtable = {
  NULL,
  dbus_message_function,
};
  
static void
g_mount_tracker_init (GMountTracker *tracker)
{
  DBusConnection *conn;
  
  load_exec_mountpoints (tracker);

  conn = dbus_bus_get (DBUS_BUS_SESSION, NULL);

  if (!dbus_connection_register_object_path (conn, "/org/gtk/gvfs/mounttracker",
					     &tracker_dbus_vtable, tracker))
    _g_dbus_oom ();

}

GMountTracker *
g_mount_tracker_new (void)
{
  GMountTracker *tracker;
  
  tracker = g_object_new (G_TYPE_MOUNT_TRACKER, NULL);

  return tracker;
}
