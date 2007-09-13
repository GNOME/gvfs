#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gfileenumeratordaemon.h>
#include <gio/gfileinfo.h>
#include <gvfsdaemondbus.h>
#include <gvfsdaemonprotocol.h>

#define OBJ_PATH_PREFIX "/org/gtk/vfs/client/enumerator/"

/* atomic */
volatile gint path_counter = 1;

G_LOCK_DEFINE_STATIC(infos);

struct _GFileEnumeratorDaemon
{
  GFileEnumerator parent;

  gint id;
  DBusConnection *sync_connection;
  GFileInfoRequestFlags request_flags;

  /* protected by infos lock */
  GList *infos;
  gboolean done;
  
};

G_DEFINE_TYPE (GFileEnumeratorDaemon, g_file_enumerator_daemon, G_TYPE_FILE_ENUMERATOR);

static GFileInfo *       g_file_enumerator_daemon_next_file   (GFileEnumerator  *enumerator,
							       GCancellable     *cancellable,
							       GError          **error);
static gboolean          g_file_enumerator_daemon_stop        (GFileEnumerator  *enumerator,
							       GCancellable     *cancellable,
							       GError          **error);
static DBusHandlerResult g_file_enumerator_daemon_dbus_filter (DBusConnection   *connection,
							       DBusMessage      *message,
							       void             *user_data);

static void
g_file_enumerator_daemon_finalize (GObject *object)
{
  GFileEnumeratorDaemon *daemon;
  char *path;

  daemon = G_FILE_ENUMERATOR_DAEMON (object);

  path = g_file_enumerator_daemon_get_object_path (daemon);
  _g_dbus_unregister_vfs_filter (path);
  g_free (path);

  g_list_foreach (daemon->infos, (GFunc)g_object_unref, NULL);
  g_list_free (daemon->infos);

  if (daemon->sync_connection)
    dbus_connection_unref (daemon->sync_connection);
  
  if (G_OBJECT_CLASS (g_file_enumerator_daemon_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_enumerator_daemon_parent_class)->finalize) (object);
}


static void
g_file_enumerator_daemon_class_init (GFileEnumeratorDaemonClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS (klass);
  
  gobject_class->finalize = g_file_enumerator_daemon_finalize;

  enumerator_class->next_file = g_file_enumerator_daemon_next_file;
  enumerator_class->stop = g_file_enumerator_daemon_stop;
}

static void
g_file_enumerator_daemon_init (GFileEnumeratorDaemon *daemon)
{
  char *path;
  
  daemon->id = g_atomic_int_exchange_and_add (&path_counter, 1);

  path = g_file_enumerator_daemon_get_object_path (daemon);
  _g_dbus_register_vfs_filter (path, g_file_enumerator_daemon_dbus_filter,
			       G_OBJECT (daemon));
  g_free (path);
}

GFileEnumeratorDaemon *
g_file_enumerator_daemon_new (void)
{
  GFileEnumeratorDaemon *daemon;

  daemon = g_object_new (G_TYPE_FILE_ENUMERATOR_DAEMON, NULL);
  
  return daemon;
}

static DBusHandlerResult
g_file_enumerator_daemon_dbus_filter (DBusConnection     *connection,
				      DBusMessage        *message,
				      void               *user_data)
{
  GFileEnumeratorDaemon *enumerator = user_data;
  const char *member;
  DBusMessageIter iter, array_iter;
  GList *infos;
  GFileInfo *info;
  
  member = dbus_message_get_member (message);

  if (strcmp (member, G_VFS_DBUS_ENUMERATOR_DONE) == 0)
    {
      G_LOCK (infos);
      enumerator->done = TRUE;
      G_UNLOCK (infos);
    }
  else if (strcmp (member, G_VFS_DBUS_ENUMERATOR_GOT_INFO) == 0)
    {
      infos = NULL;
      
      dbus_message_iter_init (message, &iter);
      if (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_ARRAY &&
	  dbus_message_iter_get_element_type (&iter) == DBUS_TYPE_STRUCT)
	{
	  dbus_message_iter_recurse (&iter, &array_iter);

	  while (dbus_message_iter_get_arg_type (&array_iter) == DBUS_TYPE_STRUCT)
	    {
	      info = _g_dbus_get_file_info (&array_iter, enumerator->request_flags, NULL);
	      if (info)
		g_assert (G_IS_FILE_INFO (info));

	      if (info)
		infos = g_list_prepend (infos, info);

	      dbus_message_iter_next (&iter);
	    }
	}

      infos = g_list_reverse (infos);
      
      G_LOCK (infos);
      enumerator->infos = g_list_concat (enumerator->infos, infos);
      G_UNLOCK (infos);
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

char  *
g_file_enumerator_daemon_get_object_path (GFileEnumeratorDaemon *enumerator)
{
  return g_strdup_printf (OBJ_PATH_PREFIX"%d", enumerator->id);
}

void
g_file_enumerator_daemon_set_sync_connection (GFileEnumeratorDaemon *enumerator,
					      DBusConnection        *connection)
{
  enumerator->sync_connection = dbus_connection_ref (connection);
}

void
g_file_enumerator_daemon_set_request_flags (GFileEnumeratorDaemon *enumerator,
					    GFileInfoRequestFlags  flags)
{
  enumerator->request_flags = flags;
}

static GFileInfo *
g_file_enumerator_daemon_next_file (GFileEnumerator *enumerator,
				    GCancellable     *cancellable,
				    GError **error)
{
  GFileEnumeratorDaemon *daemon = G_FILE_ENUMERATOR_DAEMON (enumerator);
  GFileInfo *info;
  gboolean done;
  
  info = NULL;
  done = FALSE;
  while (1)
    {
      G_LOCK (infos);
      if (daemon->infos)
	{
	  done = TRUE;
	  info = daemon->infos->data;
	  if (info)
	    g_assert (G_IS_FILE_INFO (info));
	  daemon->infos = g_list_delete_link (daemon->infos, daemon->infos);
	}
      else if (daemon->done)
	done = TRUE;
      G_UNLOCK (infos);

      if (info)
	g_assert (G_IS_FILE_INFO (info));
      
      if (done)
	break;
  
      if (!dbus_connection_read_write_dispatch (daemon->sync_connection, -1))
	  break;
    }

  return info;
}

static gboolean
g_file_enumerator_daemon_stop (GFileEnumerator *enumerator,
			      GCancellable     *cancellable,
			      GError          **error)
{
  /*GFileEnumeratorDaemon *daemon = G_FILE_ENUMERATOR_DAEMON (enumerator); */

  return TRUE;
}


