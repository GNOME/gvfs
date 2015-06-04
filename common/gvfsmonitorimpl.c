/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2015 Red Hat, Inc.
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

#include <gvfsmonitorimpl.h>
#include <gio/gio.h>
#include <gvfsdbus.h>

static GVfsMonitorImplementation *
g_vfs_monitor_implementation_new (void)
{
  return g_new0 (GVfsMonitorImplementation, 1);
}

void
g_vfs_monitor_implementation_free (GVfsMonitorImplementation *impl)
{
  g_free (impl->type_name);
  g_free (impl->dbus_name);
  g_free (impl);
}

GVfsMonitorImplementation *
g_vfs_monitor_implementation_from_dbus (GVariant *value)
{
  GVfsMonitorImplementation *impl;
  GVariantIter *iter;

  impl = g_vfs_monitor_implementation_new ();
  
  g_variant_get (value, "(ssbia{sv})",
                 &impl->type_name,
                 &impl->dbus_name,
                 &impl->is_native,
                 &impl->native_priority,
                 &iter);
  
  g_variant_iter_free (iter);

  return impl;
}

GVariant   *
g_vfs_monitor_implementation_to_dbus (GVfsMonitorImplementation *impl)
{
  GVariantBuilder builder;
  GVariant *v;

  g_assert (impl->type_name != NULL);
  g_assert (impl->dbus_name != NULL);

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);

  v = g_variant_new ("(ssbia{sv})",
		     impl->type_name,
		     impl->dbus_name,
		     impl->is_native,
		     impl->native_priority,
		     &builder);
  g_variant_builder_clear (&builder);
  
  return v;
}

GList *
g_vfs_list_monitor_implementations (void)
{
  GList *res = NULL;
  GDir *dir;
  GError *error;
  const char *monitors_dir;

  monitors_dir = g_getenv ("GVFS_MONITOR_DIR");
  if (monitors_dir == NULL || *monitors_dir == 0)
    monitors_dir = REMOTE_VOLUME_MONITORS_DIR;

  error = NULL;
  dir = g_dir_open (monitors_dir, 0, &error);
  if (dir == NULL)
    {
      g_debug ("cannot open directory %s: %s", monitors_dir, error->message);
      g_error_free (error);
    }
  else
    {
      const char *name;

      while ((name = g_dir_read_name (dir)) != NULL)
        {
	  GVfsMonitorImplementation *impl;
          GKeyFile *key_file;
          char *type_name;
          char *path;
          char *dbus_name;
          gboolean is_native;
          int native_priority;

          type_name = NULL;
          key_file = NULL;
          dbus_name = NULL;
          path = NULL;

          if (!g_str_has_suffix (name, ".monitor"))
            goto cont;

          path = g_build_filename (monitors_dir, name, NULL);

          key_file = g_key_file_new ();
          error = NULL;
          if (!g_key_file_load_from_file (key_file, path, G_KEY_FILE_NONE, &error))
            {
              g_warning ("error loading key-value file %s: %s", path, error->message);
              g_error_free (error);
              goto cont;
            }

          type_name = g_key_file_get_string (key_file, "RemoteVolumeMonitor", "Name", &error);
          if (error != NULL)
            {
              g_warning ("error extracting Name key from %s: %s", path, error->message);
              g_error_free (error);
              goto cont;
            }

          dbus_name = g_key_file_get_string (key_file, "RemoteVolumeMonitor", "DBusName", &error);
          if (error != NULL)
            {
              g_warning ("error extracting DBusName key from %s: %s", path, error->message);
              g_error_free (error);
              goto cont;
            }

          is_native = g_key_file_get_boolean (key_file, "RemoteVolumeMonitor", "IsNative", &error);
          if (error != NULL)
            {
              g_warning ("error extracting IsNative key from %s: %s", path, error->message);
              g_error_free (error);
              goto cont;
            }

          if (is_native)
            {
              native_priority = g_key_file_get_integer (key_file, "RemoteVolumeMonitor", "NativePriority", &error);
              if (error != NULL)
                {
                  g_warning ("error extracting NativePriority key from %s: %s", path, error->message);
                  g_error_free (error);
                  goto cont;
                }
            }
          else
            {
              native_priority = 0;
            }


	  impl = g_vfs_monitor_implementation_new ();
	  impl->type_name = type_name;
	  type_name = NULL; /* Transfer ownership */
	  impl->dbus_name = dbus_name;
	  dbus_name = NULL; /* Transfer ownership */
	  impl->is_native = is_native;
	  impl->native_priority = native_priority;

	  res = g_list_prepend (res, impl);

        cont:

          g_free (type_name);
          g_free (dbus_name);
          g_free (path);
          if (key_file != NULL)
              g_key_file_free (key_file);
        }
      g_dir_close (dir);
    }

  return res;
}

