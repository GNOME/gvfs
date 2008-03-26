/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib/gi18n-lib.h>
#include <gdaemonfileenumerator.h>
#include <gio/gio.h>
#include <gvfsdaemondbus.h>
#include <gvfsdaemonprotocol.h>

#define OBJ_PATH_PREFIX "/org/gtk/vfs/client/enumerator/"

/* atomic */
static volatile gint path_counter = 1;

G_LOCK_DEFINE_STATIC(infos);

struct _GDaemonFileEnumerator
{
  GFileEnumerator parent;

  gint id;
  DBusConnection *sync_connection;

  /* protected by infos lock */
  GList *infos;
  gboolean done;
  
};

G_DEFINE_TYPE (GDaemonFileEnumerator, g_daemon_file_enumerator, G_TYPE_FILE_ENUMERATOR)

static GFileInfo *       g_daemon_file_enumerator_next_file   (GFileEnumerator  *enumerator,
							       GCancellable     *cancellable,
							       GError          **error);
static gboolean          g_daemon_file_enumerator_close       (GFileEnumerator  *enumerator,
							       GCancellable     *cancellable,
							       GError          **error);
static DBusHandlerResult g_daemon_file_enumerator_dbus_filter (DBusConnection   *connection,
							       DBusMessage      *message,
							       void             *user_data);

static void
g_daemon_file_enumerator_finalize (GObject *object)
{
  GDaemonFileEnumerator *daemon;
  char *path;

  daemon = G_DAEMON_FILE_ENUMERATOR (object);

  path = g_daemon_file_enumerator_get_object_path (daemon);
  _g_dbus_unregister_vfs_filter (path);
  g_free (path);

  g_list_foreach (daemon->infos, (GFunc)g_object_unref, NULL);
  g_list_free (daemon->infos);

  if (daemon->sync_connection)
    dbus_connection_unref (daemon->sync_connection);
  
  if (G_OBJECT_CLASS (g_daemon_file_enumerator_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_file_enumerator_parent_class)->finalize) (object);
}


static void
g_daemon_file_enumerator_class_init (GDaemonFileEnumeratorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GFileEnumeratorClass *enumerator_class = G_FILE_ENUMERATOR_CLASS (klass);
  
  gobject_class->finalize = g_daemon_file_enumerator_finalize;

  enumerator_class->next_file = g_daemon_file_enumerator_next_file;
  enumerator_class->close_fn = g_daemon_file_enumerator_close;
}

static void
g_daemon_file_enumerator_init (GDaemonFileEnumerator *daemon)
{
  char *path;
  
  daemon->id = g_atomic_int_exchange_and_add (&path_counter, 1);

  path = g_daemon_file_enumerator_get_object_path (daemon);
  _g_dbus_register_vfs_filter (path, g_daemon_file_enumerator_dbus_filter,
			       G_OBJECT (daemon));
  g_free (path);
}

GDaemonFileEnumerator *
g_daemon_file_enumerator_new (void)
{
  GDaemonFileEnumerator *daemon;

  daemon = g_object_new (G_TYPE_DAEMON_FILE_ENUMERATOR, NULL);
  
  return daemon;
}

static DBusHandlerResult
g_daemon_file_enumerator_dbus_filter (DBusConnection     *connection,
				      DBusMessage        *message,
				      void               *user_data)
{
  GDaemonFileEnumerator *enumerator = user_data;
  const char *member;
  DBusMessageIter iter, array_iter;
  GList *infos;
  GFileInfo *info;
  
  member = dbus_message_get_member (message);

  if (strcmp (member, G_VFS_DBUS_ENUMERATOR_OP_DONE) == 0)
    {
      G_LOCK (infos);
      enumerator->done = TRUE;
      G_UNLOCK (infos);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
  else if (strcmp (member, G_VFS_DBUS_ENUMERATOR_OP_GOT_INFO) == 0)
    {
      infos = NULL;
      
      dbus_message_iter_init (message, &iter);
      if (dbus_message_iter_get_arg_type (&iter) == DBUS_TYPE_ARRAY &&
	  dbus_message_iter_get_element_type (&iter) == DBUS_TYPE_STRUCT)
	{
	  dbus_message_iter_recurse (&iter, &array_iter);

	  while (dbus_message_iter_get_arg_type (&array_iter) == DBUS_TYPE_STRUCT)
	    {
	      info = _g_dbus_get_file_info (&array_iter, NULL);
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
      return DBUS_HANDLER_RESULT_HANDLED;
    }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

char  *
g_daemon_file_enumerator_get_object_path (GDaemonFileEnumerator *enumerator)
{
  return g_strdup_printf (OBJ_PATH_PREFIX"%d", enumerator->id);
}

void
g_daemon_file_enumerator_set_sync_connection (GDaemonFileEnumerator *enumerator,
					      DBusConnection        *connection)
{
  enumerator->sync_connection = dbus_connection_ref (connection);
}

static GFileInfo *
g_daemon_file_enumerator_next_file (GFileEnumerator *enumerator,
				    GCancellable     *cancellable,
				    GError **error)
{
  GDaemonFileEnumerator *daemon = G_DAEMON_FILE_ENUMERATOR (enumerator);
  GFileInfo *info;
  gboolean done;
  int count;
  
  info = NULL;
  done = FALSE;
  count = 0;
  while (count < G_VFS_DBUS_TIMEOUT_MSECS / 100)
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

      /* We sleep only 100 msecs here, not the full time because we might have
       * raced with the filter func being called after unlocking
       * and setting done or ->infos. So, we want to check it again reasonaby soon.
       */
      if (!dbus_connection_read_write_dispatch (daemon->sync_connection, 100))
	  break;
    }

  return info;
}

static gboolean
g_daemon_file_enumerator_close (GFileEnumerator *enumerator,
				GCancellable     *cancellable,
				GError          **error)
{
  /*GDaemonFileEnumerator *daemon = G_DAEMON_FILE_ENUMERATOR (enumerator); */

  return TRUE;
}


