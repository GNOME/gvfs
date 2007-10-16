#include <config.h>

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gi18n.h>
#include <gvfsmonitor.h>
#include <gio/gsocketinputstream.h>
#include <gio/gsocketoutputstream.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsdaemonutils.h>
#include <gvfsjobcloseread.h>
#include <gvfsjobclosewrite.h>

#define OBJ_PATH_PREFIX "/org/gtk/vfs/daemon/dirmonitor/"

/* TODO: Real P_() */
#define P_(_x) (_x)

typedef struct {
  DBusConnection *connection;
  char *id;
  char *object_path;
} Subscriber;

struct _GVfsMonitorPrivate
{
  GVfsDaemon *daemon;
  char *object_path;
  GList *subscribers;
};

/* atomic */
static volatile gint path_counter = 1;

G_DEFINE_TYPE (GVfsMonitor, g_vfs_monitor, G_TYPE_OBJECT)

static void
g_vfs_monitor_finalize (GObject *object)
{
  GVfsMonitor *monitor;

  monitor = G_VFS_MONITOR (object);

  g_vfs_daemon_unregister_path (monitor->priv->daemon, monitor->priv->object_path);
  
  g_free (monitor->priv->object_path);
  g_object_unref (monitor->priv->daemon);
  
  if (G_OBJECT_CLASS (g_vfs_monitor_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_monitor_parent_class)->finalize) (object);
}

static void
g_vfs_monitor_class_init (GVfsMonitorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GVfsMonitorPrivate));
  
  gobject_class->finalize = g_vfs_monitor_finalize;
}

static void
g_vfs_monitor_init (GVfsMonitor *monitor)
{
  gint id;
  
  monitor->priv = G_TYPE_INSTANCE_GET_PRIVATE (monitor,
					       G_TYPE_VFS_MONITOR,
					       GVfsMonitorPrivate);

  
  id = g_atomic_int_exchange_and_add (&path_counter, 1);
  monitor->priv->object_path = g_strdup_printf (OBJ_PATH_PREFIX"%d", id);
}

static gboolean
vfs_monitor_initial_unref (gpointer data)
{
  GVfsMonitor *monitor = data;

  /* Unref the initial refcount for the VfsMonitor. If we
     didn't get an initial subscriber this is where we free the
     monitor */
     
  g_object_unref (monitor);
  
  return FALSE;
}

static DBusHandlerResult
vfs_monitor_message_callback (DBusConnection  *connection,
			      DBusMessage     *message,
			      void            *user_data)
{
  GVfsMonitor *monitor = user_data;
  char *object_path;
  DBusError derror;
  GList *l;
  Subscriber *subscriber;
  DBusMessage *reply;
  
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_MONITOR_INTERFACE,
				   G_VFS_DBUS_MONITOR_OP_SUBSCRIBE))
    {
      dbus_error_init (&derror);
      if (!dbus_message_get_args (message, &derror, 
				  DBUS_TYPE_OBJECT_PATH, &object_path,
				  0))
	{
	  reply = dbus_message_new_error (message,
					  derror.name,
					  derror.message);
	  dbus_error_free (&derror);
	  
	  dbus_connection_send (connection, reply, NULL);
	}
      else
	{
	  subscriber = g_new0 (Subscriber, 1);
	  subscriber->connection = dbus_connection_ref (connection);
	  subscriber->id = g_strdup (dbus_message_get_sender (message));
	  subscriber->object_path = g_strdup (object_path);

	  g_object_ref (monitor);
	  monitor->priv->subscribers = g_list_prepend (monitor->priv->subscribers, subscriber);
	}
      
      return DBUS_HANDLER_RESULT_HANDLED;
    }
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MONITOR_INTERFACE,
					G_VFS_DBUS_MONITOR_OP_UNSUBSCRIBE))
    {
      dbus_error_init (&derror);
      if (!dbus_message_get_args (message, &derror, 
				  DBUS_TYPE_OBJECT_PATH, &object_path,
				  0))
	{
	  reply = dbus_message_new_error (message,
					  derror.name,
					  derror.message);
	  dbus_error_free (&derror);
	  
	  dbus_connection_send (connection, reply, NULL);

	  /* TODO: Handle connection dying and unregister subscription */
	}
      else
	{
	  for (l = monitor->priv->subscribers; l != NULL; l = l->next)
	    {
	      subscriber = l->data;

	      if (subscriber->connection == connection &&
		  strcmp (subscriber->object_path, object_path) == 0 &&
		  ((dbus_message_get_sender (message) == NULL && subscriber->id == NULL) ||
		   (dbus_message_get_sender (message) != NULL && subscriber->id != NULL &&
		    strcmp (subscriber->id, dbus_message_get_sender (message)) == 0)))
		{
		  dbus_connection_unref (subscriber->connection);
		  g_free (subscriber->id);
		  g_free (subscriber->object_path);
		  g_free (subscriber);
		  g_object_unref (monitor);

		  monitor->priv->subscribers = g_list_delete_link (monitor->priv->subscribers, l);
		  break;
		}
	    }
	}
      
      return DBUS_HANDLER_RESULT_HANDLED;
    }
      
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  
}

GVfsMonitor *
g_vfs_monitor_new (GVfsDaemon *daemon)
{
  GVfsMonitor *monitor;
  
  monitor = g_object_new (G_TYPE_VFS_MONITOR, NULL);

  monitor->priv->daemon = g_object_ref (daemon);

  g_vfs_daemon_register_path (daemon,
			      monitor->priv->object_path,
			      vfs_monitor_message_callback,
			      monitor);

  g_timeout_add (5000,
		 vfs_monitor_initial_unref,
		 monitor);

  return monitor;  
}

const char *
g_vfs_monitor_get_object_path (GVfsMonitor *monitor)
{
  return monitor->priv->object_path;
}


void
g_vfs_monitor_emit_event (GVfsMonitor       *monitor,
			  GFileMonitorEvent  event_type,
			  GMountSpec        *file_spec,
			  const char        *file_path,
			  GMountSpec        *other_file_spec,
			  const char        *other_file_path)
{
  GList *l;
  Subscriber *subscriber;
  DBusMessage *message;
  DBusMessageIter iter;
  guint32 event_type_dbus;
  
  for (l = monitor->priv->subscribers; l != NULL; l = l->next)
    {
      subscriber = l->data;

      message =
	dbus_message_new_method_call (subscriber->id,
				      subscriber->object_path,
				      G_VFS_DBUS_MONITOR_CLIENT_INTERFACE,
				      G_VFS_DBUS_MONITOR_CLIENT_OP_CHANGED);

      dbus_message_iter_init_append (message, &iter);
      event_type_dbus = event_type;
      dbus_message_iter_append_basic (&iter,
				      DBUS_TYPE_UINT32,
				      &event_type_dbus);
      g_mount_spec_to_dbus (&iter, file_spec);
      _g_dbus_message_iter_append_cstring (&iter, file_path);

      if (other_file_spec && other_file_path)
	{
	  g_mount_spec_to_dbus (&iter, other_file_spec);
	  _g_dbus_message_iter_append_cstring (&iter, other_file_path);
	}

      dbus_message_set_no_reply (message, FALSE);
      
      dbus_connection_send (subscriber->connection, message, NULL);
      dbus_message_unref (message);
    }
}
