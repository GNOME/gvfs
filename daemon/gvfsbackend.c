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
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include "gvfsbackend.h"
#include "gvfsjobsource.h"
#include "gvfsdaemonprotocol.h"
#include <gvfsjobopenforread.h>
#include <gvfsjobopenforwrite.h>
#include <gvfsjobqueryinfo.h>
#include <gvfsjobqueryfsinfo.h>
#include <gvfsjobsetdisplayname.h>
#include <gvfsjobenumerate.h>
#include <gvfsjobdelete.h>
#include <gvfsjobtrash.h>
#include <gvfsjobunmount.h>
#include <gvfsjobmountmountable.h>
#include <gvfsjobunmountmountable.h>
#include <gvfsjobmakedirectory.h>
#include <gvfsjobmakesymlink.h>
#include <gvfsjobcreatemonitor.h>
#include <gvfsjobupload.h>
#include <gvfsjobcopy.h>
#include <gvfsjobmove.h>
#include <gvfsjobsetattribute.h>
#include <gvfsjobqueryattributes.h>
#include <gdbusutils.h>

enum {
  PROP_0,
  PROP_OBJECT_PATH,
  PROP_DAEMON
};

struct _GVfsBackendPrivate
{
  GVfsDaemon *daemon;
  char *object_path;
  
  char *display_name;
  char *stable_name;
  char *icon;
  char *prefered_filename_encoding;
  gboolean user_visible;
  GMountSpec *mount_spec;
};


/* TODO: Real P_() */
#define P_(_x) (_x)

static void              g_vfs_backend_job_source_iface_init (GVfsJobSourceIface    *iface);
static void              g_vfs_backend_get_property          (GObject               *object,
							      guint                  prop_id,
							      GValue                *value,
							      GParamSpec            *pspec);
static void              g_vfs_backend_set_property          (GObject               *object,
							      guint                  prop_id,
							      const GValue          *value,
							      GParamSpec            *pspec);
static GObject*          g_vfs_backend_constructor           (GType                  type,
							      guint                  n_construct_properties,
							      GObjectConstructParam *construct_params);
static DBusHandlerResult backend_dbus_handler                (DBusConnection        *connection,
							      DBusMessage           *message,
							      void                  *user_data);


G_DEFINE_TYPE_WITH_CODE (GVfsBackend, g_vfs_backend, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_VFS_TYPE_JOB_SOURCE,
						g_vfs_backend_job_source_iface_init))


static GHashTable *registered_backends = NULL;

void
g_vfs_register_backend (GType backend_type,
			const char *type)
{
  if (registered_backends == NULL)
    registered_backends = g_hash_table_new_full (g_str_hash, g_str_equal,
						 g_free, NULL);

  g_hash_table_insert (registered_backends,
		       g_strdup (type), (void *)backend_type);
}

GType
g_vfs_lookup_backend (const char *type)
{
  gpointer res;
  
  if (registered_backends != NULL)
    {
      res = g_hash_table_lookup (registered_backends, type);
      if (res != NULL)
	return (GType)res;
    }
  
  return G_TYPE_INVALID;
}
  
static void
g_vfs_backend_finalize (GObject *object)
{
  GVfsBackend *backend;

  backend = G_VFS_BACKEND (object);

  g_vfs_daemon_unregister_path (backend->priv->daemon, backend->priv->object_path);
  g_object_unref (backend->priv->daemon);
  g_free (backend->priv->object_path);
  
  g_free (backend->priv->display_name);
  g_free (backend->priv->stable_name);
  g_free (backend->priv->icon);
  g_free (backend->priv->prefered_filename_encoding);
  if (backend->priv->mount_spec)
    g_mount_spec_unref (backend->priv->mount_spec);
  
  if (G_OBJECT_CLASS (g_vfs_backend_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_parent_class)->finalize) (object);
}

static void
g_vfs_backend_job_source_iface_init (GVfsJobSourceIface *iface)
{
}

static void
g_vfs_backend_class_init (GVfsBackendClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GVfsBackendPrivate));
  
  gobject_class->constructor = g_vfs_backend_constructor;
  gobject_class->finalize = g_vfs_backend_finalize;
  gobject_class->set_property = g_vfs_backend_set_property;
  gobject_class->get_property = g_vfs_backend_get_property;

  g_object_class_install_property (gobject_class,
				   PROP_OBJECT_PATH,
				   g_param_spec_string ("object-path",
							P_("Backend object path"),
							P_("The dbus object path for the backend object."),
							"",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));
  g_object_class_install_property (gobject_class,
				   PROP_DAEMON,
				   g_param_spec_object ("daemon",
							P_("Daemon"),
							P_("The daemon this backend is handled by."),
							G_VFS_TYPE_DAEMON,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
							G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));
}

static void
g_vfs_backend_init (GVfsBackend *backend)
{
  backend->priv = G_TYPE_INSTANCE_GET_PRIVATE (backend, G_VFS_TYPE_BACKEND, GVfsBackendPrivate);
  backend->priv->icon = g_strdup ("");
  backend->priv->prefered_filename_encoding = g_strdup ("");
  backend->priv->display_name = g_strdup ("");
  backend->priv->stable_name = g_strdup ("");
  backend->priv->user_visible = TRUE;
}

static void
g_vfs_backend_set_property (GObject         *object,
			    guint            prop_id,
			    const GValue    *value,
			    GParamSpec      *pspec)
{
  GVfsBackend *backend = G_VFS_BACKEND (object);
  
  switch (prop_id)
    {
    case PROP_OBJECT_PATH:
      backend->priv->object_path = g_value_dup_string (value);
      break;
    case PROP_DAEMON:
      backend->priv->daemon = G_VFS_DAEMON (g_value_dup_object (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_vfs_backend_get_property (GObject    *object,
			    guint       prop_id,
			    GValue     *value,
			    GParamSpec *pspec)
{
  GVfsBackend *backend = G_VFS_BACKEND (object);
  
  switch (prop_id)
    {
    case PROP_OBJECT_PATH:
      g_value_set_string (value, backend->priv->object_path);
      break;
    case PROP_DAEMON:
      g_value_set_object (value, backend->priv->daemon);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static GObject*
g_vfs_backend_constructor (GType                  type,
			   guint                  n_construct_properties,
			   GObjectConstructParam *construct_params)
{
  GObject *object;
  GVfsBackend *backend;

  object = (* G_OBJECT_CLASS (g_vfs_backend_parent_class)->constructor) (type,
									 n_construct_properties,
									 construct_params);
  backend = G_VFS_BACKEND (object);
  
  g_vfs_daemon_register_path (backend->priv->daemon,
			      backend->priv->object_path, 
			      backend_dbus_handler,
			      backend);
  
  return object;
}

GVfsDaemon *
g_vfs_backend_get_daemon (GVfsBackend *backend)
{
  return backend->priv->daemon;
}


void
g_vfs_backend_set_display_name (GVfsBackend *backend,
				const char *display_name)
{
  g_free (backend->priv->display_name);
  backend->priv->display_name = g_strdup (display_name);
}

/**
 * g_vfs_backend_set_stable_name:
 * @backend: backend
 * @stable_name: the stable name
 *
 * For filesystems that can change the name during the lifetime
 * of the filesystem this can be uses to set a separate stable
 * name. This is used for instance as the directory representing
 * the mounted file system in the standard UNIX file system
 * namespace.
 *
 * If this function isn't called, the value passed to
 * g_vfs_backend_set_display_name() will be used instead.
 **/
void
g_vfs_backend_set_stable_name (GVfsBackend        *backend,
			       const char         *stable_name)
{
  g_free (backend->priv->stable_name);
  backend->priv->stable_name = g_strdup (stable_name);
}

void
g_vfs_backend_set_icon_name (GVfsBackend *backend,
			     const char *icon)
{
  g_free (backend->priv->icon);
  backend->priv->icon = g_strdup (icon);
}

void
g_vfs_backend_set_prefered_filename_encoding (GVfsBackend  *backend,
					      const char *prefered_filename_encoding)
{
  g_free (backend->priv->prefered_filename_encoding);
  backend->priv->prefered_filename_encoding = g_strdup (prefered_filename_encoding);
}

void
g_vfs_backend_set_user_visible (GVfsBackend  *backend,
				gboolean user_visible)
{
  backend->priv->user_visible = user_visible;
}

void
g_vfs_backend_set_mount_spec (GVfsBackend *backend,
			      GMountSpec *mount_spec)
{
  if (backend->priv->mount_spec)
    g_mount_spec_unref (backend->priv->mount_spec);
  backend->priv->mount_spec = g_mount_spec_ref (mount_spec);
}

const char *
g_vfs_backend_get_backend_type (GVfsBackend *backend)
{
  if (backend->priv->mount_spec)
    return g_mount_spec_get_type (backend->priv->mount_spec);
  return NULL;
}

const char *
g_vfs_backend_get_display_name (GVfsBackend *backend)
{
  return backend->priv->display_name;
}

const char *
g_vfs_backend_get_stable_name (GVfsBackend *backend)
{
  return backend->priv->stable_name;
}

const char *
g_vfs_backend_get_icon_name (GVfsBackend *backend)
{
  return backend->priv->icon;
}

GMountSpec *
g_vfs_backend_get_mount_spec (GVfsBackend *backend)
{
  return backend->priv->mount_spec;
}



static DBusHandlerResult
backend_dbus_handler (DBusConnection  *connection,
		      DBusMessage     *message,
		      void            *user_data)
{
  GVfsBackend *backend = user_data;
  GVfsJob *job;

  job = NULL;

  g_print ("backend_dbus_handler %s:%s\n",
	   dbus_message_get_interface (message),
	   dbus_message_get_member (message));
  
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_MOUNT_INTERFACE,
				   G_VFS_DBUS_MOUNT_OP_UNMOUNT))
    job = g_vfs_job_unmount_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_MOUNT_INTERFACE,
				   G_VFS_DBUS_MOUNT_OP_OPEN_FOR_READ))
    job = g_vfs_job_open_for_read_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_QUERY_INFO))
    job = g_vfs_job_query_info_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_QUERY_FILESYSTEM_INFO))
    job = g_vfs_job_query_fs_info_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_ENUMERATE))
    job = g_vfs_job_enumerate_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_OPEN_FOR_WRITE))
    job = g_vfs_job_open_for_write_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_MOUNT_MOUNTABLE))
    job = g_vfs_job_mount_mountable_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_UNMOUNT_MOUNTABLE))
    job = g_vfs_job_unmount_mountable_new (connection, message, backend, FALSE);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_EJECT_MOUNTABLE))
    job = g_vfs_job_unmount_mountable_new (connection, message, backend, TRUE);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_SET_DISPLAY_NAME))
    job = g_vfs_job_set_display_name_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_DELETE))
    job = g_vfs_job_delete_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_TRASH))
    job = g_vfs_job_trash_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_MAKE_DIRECTORY))
    job = g_vfs_job_make_directory_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_MAKE_SYMBOLIC_LINK))
    job = g_vfs_job_make_symlink_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_COPY))
    job = g_vfs_job_copy_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_UPLOAD))
    job = g_vfs_job_upload_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_MOVE))
    job = g_vfs_job_move_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_SET_ATTRIBUTE))
    job = g_vfs_job_set_attribute_new (connection, message, backend);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_QUERY_SETTABLE_ATTRIBUTES))
    job = g_vfs_job_query_attributes_new (connection, message, backend, FALSE);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_QUERY_WRITABLE_NAMESPACES))
    job = g_vfs_job_query_attributes_new (connection, message, backend, TRUE);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_CREATE_DIR_MONITOR))
    job = g_vfs_job_create_monitor_new (connection, message, backend, TRUE);
  else if (dbus_message_is_method_call (message,
					G_VFS_DBUS_MOUNT_INTERFACE,
					G_VFS_DBUS_MOUNT_OP_CREATE_FILE_MONITOR))
    job = g_vfs_job_create_monitor_new (connection, message, backend, FALSE);

  if (job)
    {
      g_vfs_job_source_new_job (G_VFS_JOB_SOURCE (backend), job);
      g_object_unref (job);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
      
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void
g_vfs_backend_register_mount (GVfsBackend *backend,
			      GAsyncDBusCallback callback,
			      gpointer user_data)
{
  const char *stable_name;
  DBusMessage *message;
  DBusMessageIter iter;
  dbus_bool_t user_visible;
  
  message = dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
					  G_VFS_DBUS_MOUNTTRACKER_PATH,
					  G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					  G_VFS_DBUS_MOUNTTRACKER_OP_REGISTER_MOUNT);
  if (message == NULL)
    _g_dbus_oom ();

  if (backend->priv->stable_name != NULL)
   stable_name = backend->priv->stable_name;
  else
   stable_name = backend->priv->display_name;

  user_visible = backend->priv->user_visible;
  if (!dbus_message_append_args (message,
				 DBUS_TYPE_OBJECT_PATH, &backend->priv->object_path,
				 DBUS_TYPE_STRING, &backend->priv->display_name,
				 DBUS_TYPE_STRING, &stable_name,
				 DBUS_TYPE_STRING, &backend->priv->icon,
				 DBUS_TYPE_STRING, &backend->priv->prefered_filename_encoding,
				 DBUS_TYPE_BOOLEAN, &user_visible,
				 0))
    _g_dbus_oom ();

  dbus_message_iter_init_append (message, &iter);
  g_mount_spec_to_dbus (&iter, backend->priv->mount_spec);

  dbus_message_set_auto_start (message, TRUE);

  _g_dbus_connection_call_async (NULL, message, -1, 
				 callback, user_data);
  dbus_message_unref (message);
}

void
g_vfs_backend_unregister_mount (GVfsBackend *backend,
				GAsyncDBusCallback callback,
				gpointer user_data)
{
  DBusMessage *message;
  
  message = dbus_message_new_method_call (G_VFS_DBUS_DAEMON_NAME,
					  G_VFS_DBUS_MOUNTTRACKER_PATH,
					  G_VFS_DBUS_MOUNTTRACKER_INTERFACE,
					  G_VFS_DBUS_MOUNTTRACKER_OP_UNREGISTER_MOUNT);
  if (message == NULL)
    _g_dbus_oom ();

  if (!dbus_message_append_args (message,
				 DBUS_TYPE_OBJECT_PATH, &backend->priv->object_path,
				 0))
    _g_dbus_oom ();

  _g_dbus_connection_call_async (NULL, message, -1, 
				 callback, user_data);
  dbus_message_unref (message);
}
