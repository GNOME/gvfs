/* hal-pool.c
 *
 * Copyright (C) 2007 David Zeuthen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <config.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <gvfsdbusutils.h>

#include "hal-pool.h"
#include "hal-marshal.h"

enum {
  DEVICE_ADDED,
  DEVICE_REMOVED,
  DEVICE_PROPERTY_CHANGED,
  DEVICE_CONDITION,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

struct _HalPoolPrivate
{
  char **cap_only;
  
  DBusConnection *dbus_connection;
  LibHalContext *hal_ctx;
  GHashTable *devices;
};

G_DEFINE_TYPE (HalPool, hal_pool, G_TYPE_OBJECT)

static void
hal_pool_finalize (HalPool *pool)
{
  g_strfreev (pool->priv->cap_only);

  dbus_bus_remove_match (pool->priv->dbus_connection,
                         "type='signal',"
                         "interface='org.freedesktop.Hal.Device',"
                         "sender='org.freedesktop.Hal'", NULL);
  libhal_ctx_shutdown (pool->priv->hal_ctx, NULL);
  dbus_connection_close (pool->priv->dbus_connection);
  dbus_connection_unref (pool->priv->dbus_connection);
  
  if (G_OBJECT_CLASS (hal_pool_parent_class)->finalize)
    (* G_OBJECT_CLASS (hal_pool_parent_class)->finalize) (G_OBJECT (pool));
}

static void
hal_pool_class_init (HalPoolClass *klass)
{
  GObjectClass *obj_class = (GObjectClass *) klass;
  
  obj_class->finalize = (GObjectFinalizeFunc) hal_pool_finalize;

  g_type_class_ref (HAL_TYPE_DEVICE);
  
  signals[DEVICE_ADDED] =
    g_signal_new ("device_added",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (HalPoolClass, device_added),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__OBJECT,
                  G_TYPE_NONE, 1,
                  HAL_TYPE_DEVICE);
  
  signals[DEVICE_REMOVED] =
    g_signal_new ("device_removed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (HalPoolClass, device_removed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__OBJECT,
                  G_TYPE_NONE, 1,
                  HAL_TYPE_DEVICE);
  
  signals[DEVICE_PROPERTY_CHANGED] =
    g_signal_new ("device_property_changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (HalPoolClass, device_property_changed),
                  NULL, NULL,
                  hal_marshal_VOID__OBJECT_STRING,
                  G_TYPE_NONE, 2,
                  HAL_TYPE_DEVICE,
                  G_TYPE_STRING);

  signals[DEVICE_CONDITION] =
    g_signal_new ("device_condition",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (HalPoolClass, device_condition),
                  NULL, NULL,
                  hal_marshal_VOID__OBJECT_STRING_STRING,
                  G_TYPE_NONE, 3,
                  HAL_TYPE_DEVICE,
                  G_TYPE_STRING,
                  G_TYPE_STRING);
}

static void
hal_pool_init (HalPool *pool)
{
  pool->priv = g_new0 (HalPoolPrivate, 1);
  pool->priv->hal_ctx = NULL;
}

static gboolean
has_cap_only (HalPool *pool, HalDevice *device)
{
  const char *subsys;
  unsigned int n;

  for (n = 0; pool->priv->cap_only != NULL && pool->priv->cap_only[n] != NULL; n++)
    {
      if (hal_device_has_capability (device, pool->priv->cap_only[n]))
        return TRUE;

      subsys = hal_device_get_property_string (device, "info.subsystem");

      if (subsys != NULL && strcmp (subsys, pool->priv->cap_only[n]) == 0)
        return TRUE;
    }
  
  return FALSE;
}

static void
hal_pool_add_device_by_udi (HalPool *pool, 
                            const char *udi, 
                            gboolean emit_signal)
{
  HalDevice *device;
  
  device = hal_device_new_from_udi (pool->priv->hal_ctx, udi);
  if (device != NULL)
    {
      if (!has_cap_only (pool, device))
        g_object_unref (device);
      else 
        {
          g_hash_table_insert (pool->priv->devices, g_strdup (udi), device);
          if (emit_signal)
            g_signal_emit (pool, signals[DEVICE_ADDED], 0, device);
        }
    }
}

#ifdef HAVE_HAL_FAST_INIT
static void
hal_pool_add_device_by_udi_and_properties (HalPool *pool, 
                                           char *udi, 
                                           LibHalPropertySet *properties, 
                                           gboolean emit_signal)
{
  HalDevice *device;
  
  device = hal_device_new_from_udi_and_properties (pool->priv->hal_ctx, udi, properties);
  if (device != NULL)
    {
      if (!has_cap_only (pool, device))
        g_object_unref (device);
      else 
        {
          g_hash_table_insert (pool->priv->devices, g_strdup (udi), device);
          if (emit_signal)
            g_signal_emit (pool, signals[DEVICE_ADDED], 0, device);
        }
    }
}
#endif

static void
_hal_device_added (LibHalContext *hal_ctx, const char *udi)
{
  HalPool *pool;
  
  pool = HAL_POOL (libhal_ctx_get_user_data (hal_ctx));
  hal_pool_add_device_by_udi (pool, udi, TRUE);
}

static void
_hal_device_removed (LibHalContext *hal_ctx, const char *udi)
{
  HalPool *pool;
  HalDevice *device;
  
  pool = HAL_POOL (libhal_ctx_get_user_data (hal_ctx));
  if ((device = hal_pool_get_device_by_udi (pool, udi)) != NULL)
    {
      g_object_ref (device);
      g_hash_table_remove (pool->priv->devices, udi);
      g_signal_emit (pool, signals[DEVICE_REMOVED], 0, device);                
      g_object_unref (device);
    }
}

void
_hal_device_hal_property_changed (HalDevice *device, const char *key);

void
_hal_device_hal_condition (HalDevice *device, const char *name, const char *detail);

static void
_hal_property_modified (LibHalContext *ctx,
                        const char *udi,
                        const char *key,
                        dbus_bool_t is_removed,
                        dbus_bool_t is_added)
{
  HalPool *pool;
  HalDevice *device;
  
  pool = HAL_POOL (libhal_ctx_get_user_data (ctx));
  
  device = hal_pool_get_device_by_udi (pool, udi);
  if (device != NULL)
    {
      _hal_device_hal_property_changed (device, key);
      g_signal_emit (pool, signals[DEVICE_PROPERTY_CHANGED], 0, device, key);
    }
}

static void
_hal_condition (LibHalContext *ctx,
                const char *udi,
                const char *condition_name,
                const char *condition_detail)
{
  HalPool *pool;
  HalDevice *device;
  
  pool = HAL_POOL (libhal_ctx_get_user_data (ctx));
  
  device = hal_pool_get_device_by_udi (pool, udi);
  if (device != NULL)
    {
      _hal_device_hal_condition (device, condition_name, condition_detail);
      g_signal_emit (pool, signals[DEVICE_CONDITION], 0, device, condition_name, condition_detail);
    }
}

LibHalContext *
hal_pool_get_hal_ctx (HalPool *pool)
{
  return pool->priv->hal_ctx;
}

DBusConnection *
hal_pool_get_dbus_connection (HalPool *pool)
{
  return pool->priv->dbus_connection;
}

HalPool *
hal_pool_new (char **cap_only)
{
  int i;
  char **devices;
  int num_devices;
  HalPool *pool;
  LibHalContext *hal_ctx;
  DBusError error;
  DBusConnection *dbus_connection;
#ifdef HAVE_HAL_FAST_INIT
  LibHalPropertySet **properties;
#endif
  
  pool = NULL;
  
  dbus_error_init (&error);
  /* see discussion on gtk-devel-list (Subject: Re: gvfs hal volume monitoring backend) on 
   * why this is private
   */
  dbus_connection = dbus_bus_get_private (DBUS_BUS_SYSTEM, &error);
  if (dbus_error_is_set (&error))
    {
      dbus_error_free (&error);
      goto out;
    }
  
  dbus_connection_set_exit_on_disconnect (dbus_connection, FALSE);

  hal_ctx = libhal_ctx_new ();
  if (hal_ctx == NULL)
    {
      dbus_connection_close (dbus_connection);
      dbus_connection_unref (dbus_connection);
      goto out;
    }

  _g_dbus_connection_integrate_with_main (dbus_connection);
  libhal_ctx_set_dbus_connection (hal_ctx, dbus_connection);
  
  if (!libhal_ctx_init (hal_ctx, &error))
    {
      dbus_connection_close (dbus_connection);
      dbus_connection_unref (dbus_connection);
      dbus_error_free (&error);
      goto out;
    }

  pool = HAL_POOL (g_object_new (HAL_TYPE_POOL, NULL));
  pool->priv->dbus_connection = dbus_connection;
  pool->priv->hal_ctx = hal_ctx;
  pool->priv->devices = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  pool->priv->cap_only = g_strdupv (cap_only);
  
  libhal_ctx_set_device_added (hal_ctx, _hal_device_added);
  libhal_ctx_set_device_removed (hal_ctx, _hal_device_removed);
  libhal_ctx_set_device_property_modified (hal_ctx, _hal_property_modified);
  libhal_ctx_set_device_condition (hal_ctx, _hal_condition);
  libhal_ctx_set_user_data (hal_ctx, pool);

#ifdef HAVE_HAL_FAST_INIT
  /* First try new O(1) algorithm to get all devices and properties in a single call..
   *
   * This method is only available in post hal 0.5.10.
   */
  if (libhal_get_all_devices_with_properties (pool->priv->hal_ctx, 
                                              &num_devices, 
                                              &devices, 
                                              &properties, 
                                              NULL))
    {
      for (i = 0; i < num_devices; i++)
        hal_pool_add_device_by_udi_and_properties (pool, devices[i], properties[i], FALSE);
      libhal_free_string_array (devices);
      free (properties); /* hal_pool_add_device_by_udi_and_properties steals the given properties */
      goto out;
    }
#endif

  /* fallback to using O(n) algorithm; will work on any hal 0.5.x release */
  devices = libhal_get_all_devices (pool->priv->hal_ctx, &num_devices, NULL);
  if (devices != NULL)
    {
      for (i = 0; i < num_devices; i++)
        {
          char *device_udi;
          device_udi = devices[i];
          hal_pool_add_device_by_udi (pool, device_udi, FALSE);
        }
      libhal_free_string_array (devices);
      goto out;
    }
  
  /* FAIL! */
  
  g_object_unref (pool);
  return NULL;
  
 out:
  return pool;
}

HalDevice *
hal_pool_get_device_by_udi (HalPool *pool, const char *udi)
{
  return g_hash_table_lookup (pool->priv->devices, udi);
}

HalDevice *
hal_pool_get_device_by_capability_and_string (HalPool    *pool, 
                                              const char *capability,
                                              const char *key,
                                              const char *value)
{
  GList *i;
  GList *devices;
  HalDevice *result;
  
  result = NULL;
  devices = NULL;
  
  if (pool->priv->devices == NULL)
    goto out;
  
  devices = g_hash_table_get_values (pool->priv->devices);
  for (i = devices; i != NULL; i = i->next)
    {
      HalDevice *d = i->data;
      const char *s;
      
      if (!hal_device_has_capability (d, capability))
        continue;
      
      s = hal_device_get_property_string (d, key);
      if (s == NULL)
        continue;
      
      if (strcmp (s, value) == 0)
        {
          result = d;
          goto out;
        }
    }
  
out:
  if (devices != NULL)
    g_list_free (devices);
  return result;
}

GList *
hal_pool_find_by_capability (HalPool *pool, const char *capability)
{
  GList *i;
  GList *j;
  GList *devices;
  
  devices = NULL;
  
  if (pool->priv->devices == NULL)
    goto out;
  
  devices = g_hash_table_get_values (pool->priv->devices);
  for (i = devices; i != NULL; i = j)
    {
      HalDevice *d = i->data;
      
      j = i->next;
      
      if (!hal_device_has_capability (d, capability))
        devices = g_list_delete_link (devices, i);
    }
  
 out:
  return devices;
}
