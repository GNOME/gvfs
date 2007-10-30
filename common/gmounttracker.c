#include <config.h>

#include <string.h>

#include <gmounttracker.h>
#include <gdbusutils.h>
#include <gvfsdaemonprotocol.h>

enum {
  MOUNTED,
  UNMOUNTED,
  LAST_SIGNAL
};

enum {
  PROP_0,
  PROP_CONNECTION
};

/* TODO: Real P_() */
#define P_(_x) (_x)

static guint signals[LAST_SIGNAL] = { 0 };

struct _GMountTracker
{
  GObject parent_instance;

  GMutex *lock;
  GList *mounts;
  DBusConnection *connection;
};

G_DEFINE_TYPE (GMountTracker, g_mount_tracker, G_TYPE_OBJECT);

static DBusHandlerResult g_mount_tracker_filter_func  (DBusConnection        *conn,
						       DBusMessage           *message,
						       gpointer               data);
static GObject*          g_mount_tracker_constructor  (GType                  type,
						       guint                  n_construct_properties,
						       GObjectConstructParam *construct_params);
static void              g_mount_tracker_set_property (GObject               *object,
						       guint                  prop_id,
						       const GValue          *value,
						       GParamSpec            *pspec);
static void              g_mount_tracker_get_property (GObject               *object,
						       guint                  prop_id,
						       GValue                *value,
						       GParamSpec            *pspec);

gboolean
g_mount_info_equal (GMountInfo *info1,
		    GMountInfo *info2)
{
  return
    strcmp (info1->dbus_id, info2->dbus_id) == 0 &&
    strcmp (info1->object_path, info2->object_path) == 0;
}

GMountInfo *
g_mount_info_dup (GMountInfo *info)
{
  GMountInfo *copy;

  copy = g_new (GMountInfo, 1);
  copy->display_name = g_strdup (info->display_name);
  copy->icon = g_strdup (info->icon);
  copy->dbus_id = g_strdup (info->dbus_id);
  copy->object_path = g_strdup (info->object_path);
  copy->mount_spec = g_mount_spec_copy (info->mount_spec);
  
  return copy;
}

void
g_mount_info_free (GMountInfo *info)
{
  g_free (info->display_name);
  g_free (info->icon);
  g_free (info->dbus_id);
  g_free (info->object_path);
  g_mount_spec_unref (info->mount_spec);
  g_free (info);
}

static GMountInfo *
g_mount_info_from_dbus (DBusMessageIter *iter)
{
  DBusMessageIter struct_iter;
  GMountInfo *info;
  GMountSpec *mount_spec;
  dbus_bool_t user_visible;
  char *display_name;
  char *icon;
  char *prefered_filename_encoding;
  char *dbus_id;
  char *obj_path;
  char *fuse_mountpoint;

  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_STRUCT)
    return NULL;
    
  dbus_message_iter_recurse (iter, &struct_iter);
    
  if (!_g_dbus_message_iter_get_args (&struct_iter, NULL,
				      DBUS_TYPE_STRING, &dbus_id,
				      DBUS_TYPE_OBJECT_PATH, &obj_path,
				      DBUS_TYPE_STRING, &display_name,
				      DBUS_TYPE_STRING, &icon,
				      DBUS_TYPE_STRING, &prefered_filename_encoding,
				      DBUS_TYPE_BOOLEAN, &user_visible,
				      G_DBUS_TYPE_CSTRING, &fuse_mountpoint,
				      0))
    return NULL;

  g_free (fuse_mountpoint);
  
  
  mount_spec = g_mount_spec_from_dbus (&struct_iter);
  if (mount_spec == NULL)
    return NULL;

  info = g_new0 (GMountInfo, 1);
  info->display_name = g_strdup (display_name);
  info->icon = g_strdup (icon);
  info->dbus_id = g_strdup (dbus_id);
  info->object_path = g_strdup (obj_path);
  info->mount_spec = mount_spec;
  info->user_visible = user_visible;
  
  return info;
}


static void
g_mount_tracker_finalize (GObject *object)
{
  GMountTracker *tracker;

  tracker = G_MOUNT_TRACKER (object);

  g_mutex_free (tracker->lock);
  
  g_list_foreach (tracker->mounts,
		  (GFunc)g_mount_info_free, NULL);
  g_list_free (tracker->mounts);

  dbus_connection_remove_filter (tracker->connection, g_mount_tracker_filter_func, tracker);


  dbus_bus_remove_match (tracker->connection,
			 "sender='"G_VFS_DBUS_DAEMON_NAME"',"
			 "interface='"G_VFS_DBUS_MOUNTTRACKER_INTERFACE"',"
			 "member='"G_VFS_DBUS_MOUNTTRACKER_SIGNAL_MOUNTED"'",
			 NULL);
  dbus_bus_remove_match (tracker->connection,
			 "sender='"G_VFS_DBUS_DAEMON_NAME"',"
			 "interface='"G_VFS_DBUS_MOUNTTRACKER_INTERFACE"',"
			 "member='"G_VFS_DBUS_MOUNTTRACKER_SIGNAL_UNMOUNTED"'",
			 NULL);
  
  dbus_connection_unref (tracker->connection);
  
  if (G_OBJECT_CLASS (g_mount_tracker_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_mount_tracker_parent_class)->finalize) (object);
}

static void
g_mount_tracker_class_init (GMountTrackerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_mount_tracker_finalize;
  gobject_class->constructor = g_mount_tracker_constructor;
  gobject_class->set_property = g_mount_tracker_set_property;
  gobject_class->get_property = g_mount_tracker_get_property;

  signals[MOUNTED] = g_signal_new (I_("mounted"),
				   G_TYPE_MOUNT_TRACKER,
				   G_SIGNAL_RUN_LAST,
				   G_STRUCT_OFFSET (GMountTrackerClass, mounted),
				   NULL, NULL,
				   g_cclosure_marshal_VOID__POINTER,
				   G_TYPE_NONE, 1, G_TYPE_POINTER);
  
  signals[UNMOUNTED] = g_signal_new (I_("unmounted"),
				     G_TYPE_MOUNT_TRACKER,
				     G_SIGNAL_RUN_LAST,
				     G_STRUCT_OFFSET (GMountTrackerClass, unmounted),
				     NULL, NULL,
				     g_cclosure_marshal_VOID__POINTER,
				     G_TYPE_NONE, 1, G_TYPE_POINTER);

  g_object_class_install_property (gobject_class,
				   PROP_CONNECTION,
				   g_param_spec_pointer ("connection",
							 P_("DBus connection"),
							 P_("The dbus connection to use for ipc."),
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));
  
}

static void
g_mount_tracker_set_property (GObject         *object,
			      guint            prop_id,
			      const GValue    *value,
			      GParamSpec      *pspec)
{
  GMountTracker *tracker = G_MOUNT_TRACKER (object);
  
  switch (prop_id)
    {
    case PROP_CONNECTION:
      if (tracker->connection)
	dbus_connection_unref (tracker->connection);
      tracker->connection = NULL;
      if (g_value_get_pointer (value))
	tracker->connection = dbus_connection_ref (g_value_get_pointer (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_mount_tracker_get_property (GObject    *object,
			      guint       prop_id,
			      GValue     *value,
			      GParamSpec *pspec)
{
  GMountTracker *tracker = G_MOUNT_TRACKER (object);
  
  switch (prop_id)
    {
    case PROP_CONNECTION:
      g_value_set_pointer (value, tracker->connection);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static GList *
g_mount_tracker_find (GMountTracker *tracker,
		      GMountInfo *info)
{
  GList *l;

  for (l = tracker->mounts; l != NULL; l = l->next)
    {
      if (g_mount_info_equal (info, (GMountInfo *)l->data))
	return l;
    }
  
  return NULL;
}

static void
g_mount_tracker_add_mount (GMountTracker *tracker,
			   GMountInfo *info)
{
  g_mutex_lock (tracker->lock);
  
  /* Don't add multiple times */
  if (g_mount_tracker_find (tracker, info))
    return;

  tracker->mounts = g_list_prepend (tracker->mounts, g_mount_info_dup (info));

  g_mutex_unlock (tracker->lock);
  
  g_signal_emit (tracker, signals[MOUNTED], 0, info);
}

static void
g_mount_tracker_remove_mount (GMountTracker *tracker,
			      GMountInfo *info)
{
  GList *l;
  GMountInfo *old_info;

  g_mutex_lock (tracker->lock);
  
  
  l = g_mount_tracker_find (tracker, info);
  
  /* Don't remove multiple times */
  if (l == NULL)
    return;

  old_info = l->data;
  
  tracker->mounts = g_list_delete_link (tracker->mounts, l);
  
  g_mutex_unlock (tracker->lock);

  g_signal_emit (tracker, signals[UNMOUNTED], 0, old_info);
  g_mount_info_free (old_info);
}

static void
list_mounts_reply (DBusPendingCall *pending,
		   void            *_data)
{
  GMountTracker *tracker = _data;
  DBusMessageIter iter, array_iter;
  GMountInfo *info;
  DBusMessage *reply;
  gboolean b;

  reply = dbus_pending_call_steal_reply (pending);
  dbus_pending_call_unref (pending);

  b = dbus_message_iter_init (reply, &iter);
  dbus_message_iter_recurse (&iter, &array_iter);

  do
    {
      info = g_mount_info_from_dbus (&array_iter);
      if (info)
	{
	  g_mount_tracker_add_mount (tracker, info);
	  g_mount_info_free (info);
	}
    }
  while (dbus_message_iter_next (&array_iter));
  
  dbus_message_unref (reply);
}

static DBusHandlerResult
g_mount_tracker_filter_func (DBusConnection *conn,
			     DBusMessage    *message,
			     gpointer        data)
{
  GMountTracker *tracker = data;
  GMountInfo *info;
  DBusMessageIter iter;

  if (dbus_message_is_signal (message,
			      G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
			      G_VFS_DBUS_MOUNTTRACKER_SIGNAL_MOUNTED))
    {
      dbus_message_iter_init (message, &iter);
      info = g_mount_info_from_dbus (&iter);

      if (info)
	{
	  g_mount_tracker_add_mount (tracker, info);
	  g_mount_info_free (info);
	}
    }
  else if (dbus_message_is_signal (message,
				   G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
				   G_VFS_DBUS_MOUNTTRACKER_SIGNAL_UNMOUNTED))
    {
      dbus_message_iter_init (message, &iter);
      info = g_mount_info_from_dbus (&iter);

      if (info)
	{
	  g_mount_tracker_remove_mount (tracker, info);
	  g_mount_info_free (info);
	}
    }
    
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* Called after construction when the construct properties (like connection) are set */
static void
init_connection (GMountTracker *tracker)
{
  DBusMessage *message;
  DBusPendingCall *pending;

  if (tracker->connection == NULL)
    tracker->connection = dbus_bus_get (DBUS_BUS_SESSION, NULL);

  message =
    dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
				  G_VFS_DBUS_MOUNTTRACKER_PATH,
				  G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
				  G_VFS_DBUS_MOUNTTRACKER_OP_LIST_MOUNTS);
  if (message == NULL)
    _g_dbus_oom ();
  
  dbus_message_set_auto_start (message, TRUE);
  
  if (!dbus_connection_send_with_reply (tracker->connection, message,
					&pending,
					G_VFS_DBUS_TIMEOUT_MSECS))
    _g_dbus_oom ();
  
  dbus_message_unref (message);
  
  if (pending != NULL)
    {
      if (!dbus_pending_call_set_notify (pending,
					 list_mounts_reply,
					 g_object_ref (tracker), g_object_unref))
	_g_dbus_oom ();
    }
  
  dbus_connection_add_filter (tracker->connection, g_mount_tracker_filter_func, tracker, NULL);
      
  dbus_bus_add_match (tracker->connection,
		      "sender='"G_VFS_DBUS_DAEMON_NAME"',"
		      "interface='"G_VFS_DBUS_MOUNTTRACKER_INTERFACE"',"
		      "member='"G_VFS_DBUS_MOUNTTRACKER_SIGNAL_MOUNTED"'",
		      NULL);
  dbus_bus_add_match (tracker->connection,
		      "sender='"G_VFS_DBUS_DAEMON_NAME"',"
		      "interface='"G_VFS_DBUS_MOUNTTRACKER_INTERFACE"',"
		      "member='"G_VFS_DBUS_MOUNTTRACKER_SIGNAL_UNMOUNTED"'",
		      NULL);
}

static void
g_mount_tracker_init (GMountTracker *tracker)
{
  if (g_thread_supported ())
    tracker->lock = g_mutex_new ();
}


static GObject*
g_mount_tracker_constructor (GType                  type,
			     guint                  n_construct_properties,
			     GObjectConstructParam *construct_params)
{
  GObject *object;
  GMountTracker *tracker;

  object = (* G_OBJECT_CLASS (g_mount_tracker_parent_class)->constructor) (type,
									   n_construct_properties,
									   construct_params);
  
  tracker = G_MOUNT_TRACKER (object);
  
  init_connection (tracker);
  
  return object;
}

GMountTracker *
g_mount_tracker_new (DBusConnection *connection)
{
  GMountTracker *tracker;

  tracker = g_object_new (G_TYPE_MOUNT_TRACKER, "connection", connection, NULL);
  
  return tracker;
}

GList *
g_mount_tracker_list_mounts (GMountTracker *tracker)
{
  GList *res, *l;
  GMountInfo *copy;

  if (tracker->lock)
    g_mutex_lock (tracker->lock);
  
  res = NULL;
  for (l = tracker->mounts; l != NULL; l = l->next)
    {
      copy = g_mount_info_dup (l->data);
      res = g_list_prepend (res, copy);
    }

  if (tracker->lock)
    g_mutex_unlock (tracker->lock);
  
  return g_list_reverse (res);
}

GMountInfo *
g_mount_tracker_find_by_mount_spec (GMountTracker *tracker,
				    GMountSpec    *mount_spec)
{
  GList *l;
  GMountInfo *info, *found;

  if (tracker->lock)
    g_mutex_lock (tracker->lock);

  found = NULL;
  for (l = tracker->mounts; l != NULL; l = l->next)
    {
      info = l->data;

      if (g_mount_spec_equal (info->mount_spec, mount_spec))
	{
	  found = g_mount_info_dup (info);
	  break;
	}
    }

  if (tracker->lock)
    g_mutex_unlock (tracker->lock);
  
  return found;
}


gboolean
g_mount_tracker_has_mount_spec (GMountTracker *tracker,
				GMountSpec    *mount_spec)
{
  GList *l;
  GMountInfo *info;
  gboolean found;

  if (tracker->lock)
    g_mutex_lock (tracker->lock);

  found = FALSE;
  for (l = tracker->mounts; l != NULL; l = l->next)
    {
      info = l->data;

      if (g_mount_spec_equal (info->mount_spec, mount_spec))
	{
	  found = TRUE;
	  break;
	}
    }

  if (tracker->lock)
    g_mutex_unlock (tracker->lock);
  
  return found;
}

