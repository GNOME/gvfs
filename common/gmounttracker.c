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
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */

#include <config.h>

#include <string.h>

#include <gmounttracker.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsdbus.h>

enum {
  MOUNTED,
  UNMOUNTED,
  LAST_SIGNAL
};

enum {
  PROP_0,
  PROP_CONNECTION,
  PROP_USER_VISIBLE_ONLY
};

/* TODO: Real P_() */
#define P_(_x) (_x)
#define I_(string) g_intern_static_string (string)

static guint signals[LAST_SIGNAL] = { 0 };

struct _GMountTracker
{
  GObject parent_instance;

  GMutex lock;
  GList *mounts;
  GDBusConnection *connection;
  GVfsDBusMountTracker *proxy;

  gboolean user_visible_only;
};

G_DEFINE_TYPE (GMountTracker, g_mount_tracker, G_TYPE_OBJECT)

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
  copy->ref_count = 1;
  copy->display_name = g_strdup (info->display_name);
  copy->stable_name = g_strdup (info->stable_name);
  copy->x_content_types = g_strdup (info->x_content_types);
  copy->icon = g_object_ref (info->icon);
  copy->symbolic_icon = g_object_ref (info->symbolic_icon);
  copy->dbus_id = g_strdup (info->dbus_id);
  copy->object_path = g_strdup (info->object_path);
  copy->mount_spec = g_mount_spec_copy (info->mount_spec);
  copy->user_visible = info->user_visible;
  copy->prefered_filename_encoding = g_strdup (info->prefered_filename_encoding);
  copy->fuse_mountpoint = g_strdup (info->fuse_mountpoint);
  copy->default_location = g_strdup (info->default_location);

  return copy;
}

GMountInfo *
g_mount_info_ref (GMountInfo *info)
{
  g_atomic_int_inc (&info->ref_count);
  return info;
}

void
g_mount_info_unref (GMountInfo *info)
{
  if (g_atomic_int_dec_and_test (&info->ref_count))
    {
      g_free (info->display_name);
      g_free (info->stable_name);
      g_free (info->x_content_types);
      g_object_unref (info->icon);
      g_object_unref (info->symbolic_icon);
      g_free (info->dbus_id);
      g_free (info->object_path);
      g_mount_spec_unref (info->mount_spec);
      g_free (info->prefered_filename_encoding);
      g_free (info->fuse_mountpoint);
      g_free (info->default_location);
      g_free (info);
    }
}

const char *
g_mount_info_resolve_path (GMountInfo *info,
			   const char *path)
{
  const char *new_path;
  int len;

  if (info->mount_spec->mount_prefix != NULL &&
      info->mount_spec->mount_prefix[0] != 0)
    {
      len = strlen (info->mount_spec->mount_prefix);
      if (info->mount_spec->mount_prefix[len-1] == '/')
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

void
g_mount_info_apply_prefix (GMountInfo  *info,
			   char       **path)
{
  GMountSpec *spec;

  spec = info->mount_spec;

  if (spec->mount_prefix != NULL &&
      spec->mount_prefix[0] != 0)
    {
      char *path_with_prefix;
      path_with_prefix = g_build_path ("/", spec->mount_prefix,
                                       *path, NULL);
      g_free (*path);
      *path = path_with_prefix;
    }

}

GMountInfo *
g_mount_info_from_dbus (GVariant *value)
{
  GMountInfo *info;
  GMountSpec *mount_spec;
  gboolean user_visible;
  const gchar *display_name;
  const gchar *stable_name;
  const gchar *x_content_types;
  const gchar *icon_str;
  const gchar *symbolic_icon_str;
  const gchar *prefered_filename_encoding;
  const gchar *dbus_id;
  const gchar *obj_path;
  const gchar *fuse_mountpoint;
  const gchar *default_location;
  GIcon *icon;
  GIcon *symbolic_icon;
  GVariant *iter_mount_spec;
  GError *error;

  g_variant_get (value, "(&s&o&s&s&s&s&s&sb^&ay@(aya{sv})^&ay)",
                 &dbus_id,
                 &obj_path,
                 &display_name,
                 &stable_name,
                 &x_content_types,
                 &icon_str,
                 &symbolic_icon_str,
                 &prefered_filename_encoding,
                 &user_visible,
                 &fuse_mountpoint,
                 &iter_mount_spec,
                 &default_location);

  mount_spec = g_mount_spec_from_dbus (iter_mount_spec);
  g_variant_unref (iter_mount_spec);
  if (mount_spec == NULL)
    return NULL;

  if (fuse_mountpoint && fuse_mountpoint[0] == '\0')
    fuse_mountpoint = NULL;
  if (default_location && default_location[0] == '\0')
    default_location = NULL;

  if (icon_str == NULL || strlen (icon_str) == 0)
    icon_str = "drive-removable-media";
  error = NULL;
  icon = g_icon_new_for_string (icon_str, &error);
  if (icon == NULL)
    {
      g_warning ("Malformed icon string '%s': %s", icon_str, error->message);
      g_error_free (error);
      icon = g_themed_icon_new ("gtk-missing-image"); /* TODO: maybe choose a better name */
    }

  if (symbolic_icon_str == NULL || strlen (symbolic_icon_str) == 0)
    symbolic_icon_str = "drive-removable-media-symbolic";
  error = NULL;
  symbolic_icon = g_icon_new_for_string (symbolic_icon_str, &error);
  if (symbolic_icon == NULL)
    {
      g_warning ("Malformed icon string '%s': %s", symbolic_icon_str, error->message);
      g_error_free (error);
      symbolic_icon = g_themed_icon_new ("drive-removable-media-symbolic");
    }

  info = g_new0 (GMountInfo, 1);
  info->ref_count = 1;
  info->display_name = g_strdup (display_name);
  info->stable_name = g_strdup (stable_name);
  info->x_content_types = g_strdup (x_content_types);
  info->icon = icon;
  info->symbolic_icon = symbolic_icon;
  info->dbus_id = g_strdup (dbus_id);
  info->object_path = g_strdup (obj_path);
  info->mount_spec = mount_spec;
  info->user_visible = user_visible;
  info->prefered_filename_encoding = g_strdup (prefered_filename_encoding);
  info->fuse_mountpoint = g_strdup (fuse_mountpoint);
  info->default_location = g_strdup (default_location);

  return info;
}

static void
g_mount_tracker_finalize (GObject *object)
{
  GMountTracker *tracker;

  tracker = G_MOUNT_TRACKER (object);

  g_mutex_clear (&tracker->lock);
  
  g_list_free_full (tracker->mounts, (GDestroyNotify)g_mount_info_unref);

  g_clear_object (&tracker->proxy);
  g_clear_object (&tracker->connection);
  
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

  g_object_class_install_property (gobject_class,
                                   PROP_USER_VISIBLE_ONLY,
                                   g_param_spec_boolean ("user-visible-only",
                                                         P_("User visible only"),
                                                         P_("User visible only"),
                                                         FALSE,
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                                                         G_PARAM_STATIC_NAME | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB));
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
      g_clear_object (&tracker->connection);
      if (g_value_get_pointer (value))
	tracker->connection = g_object_ref (g_value_get_pointer (value));
      break;
    case PROP_USER_VISIBLE_ONLY:
      tracker->user_visible_only = g_value_get_boolean (value);
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
    case PROP_USER_VISIBLE_ONLY:
      g_value_set_boolean (value, tracker->user_visible_only);
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
  g_mutex_lock (&tracker->lock);
  
  /* Don't add multiple times */
  if (g_mount_tracker_find (tracker, info))
    {
      g_mutex_unlock (&tracker->lock);
      return;
    }

  if (tracker->user_visible_only && !info->user_visible)
    {
      g_mutex_unlock (&tracker->lock);
      return;
    }

  tracker->mounts = g_list_prepend (tracker->mounts, g_mount_info_ref (info));

  g_mutex_unlock (&tracker->lock);
  
  g_signal_emit (tracker, signals[MOUNTED], 0, info);
}

static void
g_mount_tracker_remove_mount (GMountTracker *tracker,
			      GMountInfo *info)
{
  GList *l;
  GMountInfo *old_info;

  g_mutex_lock (&tracker->lock);
  
  l = g_mount_tracker_find (tracker, info);
  
  /* Don't remove multiple times */
  if (l == NULL)
    {
      g_mutex_unlock (&tracker->lock);
      return;
    }

  old_info = l->data;
  
  tracker->mounts = g_list_delete_link (tracker->mounts, l);
  
  g_mutex_unlock (&tracker->lock);

  g_signal_emit (tracker, signals[UNMOUNTED], 0, old_info);
  g_mount_info_unref (old_info);
}

static void
list_mounts_reply (GMountTracker *tracker,
                   GVariant *mounts)
{
  GMountInfo *info;
  GVariantIter iter;
  GVariant *child;
  
  g_variant_iter_init (&iter, mounts);
  while ((child = g_variant_iter_next_value (&iter)))
    {
      info = g_mount_info_from_dbus (child);
      if (info)
        {
          g_mount_tracker_add_mount (tracker, info);
          g_mount_info_unref (info);
        }
      g_variant_unref (child);
    }
}

static void
mounted_cb (GVfsDBusMountTracker *object,
            GVariant *arg_mount,
            gpointer user_data)
{
  GMountTracker *tracker = G_MOUNT_TRACKER (user_data);
  GMountInfo *info;
  
  info = g_mount_info_from_dbus (arg_mount);
  if (info)
    {
      g_mount_tracker_add_mount (tracker, info);
      g_mount_info_unref (info);
    }
}

static void
unmounted_cb (GVfsDBusMountTracker *object,
              GVariant *arg_mount,
              gpointer user_data)
{
  GMountTracker *tracker = G_MOUNT_TRACKER (user_data);
  GMountInfo *info;
  
  info = g_mount_info_from_dbus (arg_mount);
  if (info)
    {
      g_mount_tracker_remove_mount (tracker, info);
      g_mount_info_unref (info);
    }
  
}

/* Called after construction when the construct properties (like connection) are set */
static void
init_connection_sync (GMountTracker *tracker)
{
  GError *error;
  GVariant *iter_mounts;
  gboolean res;

  if (tracker->connection == NULL)
    tracker->connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);

  error = NULL;
  tracker->proxy = gvfs_dbus_mount_tracker_proxy_new_sync (tracker->connection,
                                                           G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                           G_VFS_DBUS_DAEMON_NAME,
                                                           G_VFS_DBUS_MOUNTTRACKER_PATH,
                                                           NULL,
                                                           &error);
  if (tracker->proxy == NULL)
    {
      g_printerr ("Error creating proxy: %s (%s, %d)\n",
                  error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      return;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (tracker->proxy), 
                                    G_VFS_DBUS_TIMEOUT_MSECS);

  res = gvfs_dbus_mount_tracker_call_list_mounts2_sync (tracker->proxy, tracker->user_visible_only, &iter_mounts, NULL, &error);
  if (!res)
    {
      if (g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD))
        res = gvfs_dbus_mount_tracker_call_list_mounts_sync (tracker->proxy, &iter_mounts, NULL, NULL);
      g_clear_error (&error);
    }

  if (res)
    {
      list_mounts_reply (tracker, iter_mounts);
      g_variant_unref (iter_mounts);
    }

  g_signal_connect (tracker->proxy, "mounted", G_CALLBACK (mounted_cb), tracker);
  g_signal_connect (tracker->proxy, "unmounted", G_CALLBACK (unmounted_cb), tracker);
}

static void
g_mount_tracker_init (GMountTracker *tracker)
{
  g_mutex_init (&tracker->lock);
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
  
  init_connection_sync (tracker);
  
  return object;
}

GMountTracker *
g_mount_tracker_new (GDBusConnection *connection,
                     gboolean         user_visible_only)
{
  GMountTracker *tracker;

  tracker = g_object_new (G_TYPE_MOUNT_TRACKER, "connection", connection, "user_visible_only", user_visible_only, NULL);

  return tracker;
}

GList *
g_mount_tracker_list_mounts (GMountTracker *tracker)
{
  GList *res, *l;
  GMountInfo *copy;

  g_mutex_lock (&tracker->lock);
  
  res = NULL;
  for (l = tracker->mounts; l != NULL; l = l->next)
    {
      copy = g_mount_info_ref (l->data);
      res = g_list_prepend (res, copy);
    }

  g_mutex_unlock (&tracker->lock);
  
  return g_list_reverse (res);
}

GMountInfo *
g_mount_tracker_find_by_mount_spec (GMountTracker *tracker,
				    GMountSpec    *mount_spec)
{
  GList *l;
  GMountInfo *info, *found;

  g_mutex_lock (&tracker->lock);

  found = NULL;
  for (l = tracker->mounts; l != NULL; l = l->next)
    {
      info = l->data;

      if (g_mount_spec_equal (info->mount_spec, mount_spec))
	{
	  found = g_mount_info_ref (info);
	  break;
	}
    }

  g_mutex_unlock (&tracker->lock);
  
  return found;
}


gboolean
g_mount_tracker_has_mount_spec (GMountTracker *tracker,
				GMountSpec    *mount_spec)
{
  GList *l;
  GMountInfo *info;
  gboolean found;

  g_mutex_lock (&tracker->lock);

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

  g_mutex_unlock (&tracker->lock);
  
  return found;
}

