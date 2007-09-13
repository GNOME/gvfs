#include <config.h>

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "mounttracker.h"
#include "gdbusutils.h"

typedef struct {
  char *key;
  char *value;
} KeyValue;

typedef struct {
  char *display_name;
  char *icon;

  /* Daemon object ref */
  char *dbus_id;
  char *object_path;

  /* Mount details */
  char *mount_prefix;
  KeyValue *spec;
} VFSMount;

struct _GMountTracker
{
  GObject parent_instance;

  GList *mounts;
};

G_DEFINE_TYPE (GMountTracker, g_mount_tracker, G_TYPE_OBJECT);

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
  int i;
  g_free (mount->display_name);
  g_free (mount->icon);
  g_free (mount->dbus_id);
  g_free (mount->object_path);
  g_free (mount->mount_prefix);
  if (mount->spec)
    {
      for (i = 0; mount->spec[i].key != NULL; i++)
	{
	  g_free (mount->spec[i].key);
	  g_free (mount->spec[i].value);
	}
      g_free (mount->spec);
    }
	   
  g_free (mount);
}


static void
g_mount_tracker_finalize (GObject *object)
{
  GMountTracker *tracker;

  tracker = G_MOUNT_TRACKER (object);

  g_list_foreach (tracker->mounts, (GFunc)vfs_mount_free, NULL);
  g_list_free (tracker->mounts);

  if (G_OBJECT_CLASS (g_mount_tracker_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_mount_tracker_parent_class)->finalize) (object);
}

static void
g_mount_tracker_class_init (GMountTrackerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_mount_tracker_finalize;
}


static int
key_compare (const void *_a, const void *_b)
{
  const KeyValue *a = _a;
  const KeyValue *b = _b;

  return strcmp (a->key, b->key);
}
  
static KeyValue *
get_spec_from_dbus (DBusMessageIter *iter)
{
  KeyValue *spec;
  int len, i;
  DBusMessageIter array_iter, struct_iter;
  const char *key;
  char *value;
  
  len = 0;
  dbus_message_iter_recurse (iter, &array_iter);
  while (dbus_message_iter_get_arg_type (&array_iter) == DBUS_TYPE_STRUCT)
    {
      len++;
      dbus_message_iter_next (&array_iter);
    }

  spec = g_new (KeyValue, len + 1);
  i = 0;
  dbus_message_iter_recurse (iter, &array_iter);
  while (dbus_message_iter_get_arg_type (&array_iter) == DBUS_TYPE_STRUCT)
    {
      dbus_message_iter_recurse (&array_iter, &struct_iter);
      if (_g_dbus_message_iter_get_args (&struct_iter, NULL,
					 DBUS_TYPE_STRING, &key,
					 G_DBUS_TYPE_CSTRING, &value,
					 0))
	{
	  spec[i].key = g_strdup (key);
	  spec[i].value = value; /* No dup */
	  i++;
	}
      dbus_message_iter_next (&array_iter);
    }
  spec[i].key = NULL;

  dbus_message_iter_next (iter);
  
  /* Sort on key */
  qsort (spec, sizeof (KeyValue), i, key_compare);
  return spec;
}

static void
register_mount (GMountTracker *tracker,
		DBusConnection *connection,
		DBusMessage *message)
{
  VFSMount *mount;
  DBusMessage *reply;
  DBusError error;
  const char *display_name, *icon, *obj_path, *id;
  DBusMessageIter iter;
  char *mount_prefix;

  id = dbus_message_get_sender (message);

  dbus_message_iter_init (message, &iter);
  
  if (_g_dbus_message_iter_get_args (&iter,
				     &error,
				     DBUS_TYPE_STRING, &display_name,
				     DBUS_TYPE_STRING, &icon,
				     DBUS_TYPE_OBJECT_PATH, &obj_path,
				     G_DBUS_TYPE_CSTRING, &mount_prefix,
				     0))
    {
      if (find_vfs_mount (tracker, id, obj_path) != NULL)
	reply = dbus_message_new_error (message,
					DBUS_ERROR_INVALID_ARGS,
					"Mountpoint Already registered");
      else if (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_ARRAY)
	reply = dbus_message_new_error (message,
					DBUS_ERROR_INVALID_ARGS,
					"No mount spec");
      else
	{
	  mount = g_new0 (VFSMount, 1);
	  mount->display_name = g_strdup (display_name);
	  mount->icon = g_strdup (icon);
	  mount->dbus_id = g_strdup (id);
	  mount->object_path = g_strdup (obj_path);
	  mount->mount_prefix = mount_prefix; /* No dup */
	  mount->spec = get_spec_from_dbus (&iter);
	  
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


static DBusHandlerResult
dbus_message_function (DBusConnection  *connection,
		       DBusMessage     *message,
		       void            *user_data)
{
  GMountTracker *tracker = user_data;

  DBusHandlerResult res;
  
  res = DBUS_HANDLER_RESULT_HANDLED;
  if (dbus_message_is_method_call (message,
				   "org.gtk.gvfs.MountTracker",
				   "registerMount"))
    register_mount (tracker, connection, message);
  else
    res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  
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
