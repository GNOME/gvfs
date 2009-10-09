/* hal-device.c
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
#include <glib/gi18n-lib.h>
#include "hal-device.h"
#include "hal-marshal.h"

struct _HalDevicePrivate
{
  LibHalContext *hal_ctx;
  LibHalPropertySet *properties;
  char *udi;
  GTimeVal time_added;
};

enum {
  HAL_PROPERTY_CHANGED,
  HAL_CONDITION,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (HalDevice, hal_device, G_TYPE_OBJECT)

static void
hal_device_finalize (HalDevice *device)
{
  libhal_device_remove_property_watch (device->priv->hal_ctx,
      device->priv->udi, NULL);
  
  if (device->priv->properties != NULL)
    libhal_free_property_set (device->priv->properties);
  g_free (device->priv->udi);
  
  if (G_OBJECT_CLASS (hal_device_parent_class)->finalize)
    (* G_OBJECT_CLASS (hal_device_parent_class)->finalize) (G_OBJECT (device));
}

static void
hal_device_class_init (HalDeviceClass *klass)
{
  GObjectClass *obj_class = (GObjectClass *) klass;
  
  obj_class->finalize = (GObjectFinalizeFunc) hal_device_finalize;

  signals[HAL_PROPERTY_CHANGED] =
    g_signal_new ("hal_property_changed",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (HalDeviceClass, hal_property_changed),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__STRING,
                  G_TYPE_NONE, 1,
                  G_TYPE_STRING);

  signals[HAL_CONDITION] =
    g_signal_new ("hal_condition",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (HalDeviceClass, hal_condition),
                  NULL, NULL,
                  hal_marshal_VOID__STRING_STRING,
                  G_TYPE_NONE, 2,
                  G_TYPE_STRING,
                  G_TYPE_STRING);
}

static void
hal_device_init (HalDevice *device)
{
  device->priv = g_new0 (HalDevicePrivate, 1);
  g_get_current_time (&(device->priv->time_added));
}

const char *
hal_device_get_property_string (HalDevice *device, const char *key)
{
  const char *ret;
  
  ret = libhal_ps_get_string (device->priv->properties, key);
  if (ret != NULL)
    return ret;
  
  /* play it safe and don't make clients crash */
  return "";
}

int
hal_device_get_property_int (HalDevice *device, const char *key)
{
  return libhal_ps_get_int32 (device->priv->properties, key);
}

double
hal_device_get_property_double (HalDevice *device, const char *key)
{
  return libhal_ps_get_double (device->priv->properties, key);
}

guint64
hal_device_get_property_uint64 (HalDevice *device, const char *key)
{
  return libhal_ps_get_uint64 (device->priv->properties, key);
}

gboolean
hal_device_get_property_bool (HalDevice *device, const char *key)
{
  return libhal_ps_get_bool (device->priv->properties, key);
}

char **
hal_device_get_property_strlist (HalDevice *device, const char *key)
{
  static char * empty[1] = {NULL};
  char **ret;
  
  ret = (char **) libhal_ps_get_strlist (device->priv->properties, key);
  if (ret != NULL)
    return (char **) ret;
  
  /* play it safe and don't make clients crash */
  return empty;
}

gboolean
hal_device_has_capability (HalDevice *device, const char *capability)
{
  int n;
  char **caps;
  gboolean ret;
  
  ret = FALSE;
  caps = hal_device_get_property_strlist (device, "info.capabilities");
  if (caps == NULL)
    goto out;
  
  for (n = 0; caps[n] != NULL; n++)
    {
      if (g_ascii_strcasecmp (caps[n], capability) == 0)
        {
          ret = TRUE;
          break;
        }
    }
  
 out:
  return ret;
}

gboolean
hal_device_has_interface (HalDevice *device, const char *interface)
{
  int n;
  char **ifs;
  gboolean ret;
  
  ret = FALSE;
  ifs = hal_device_get_property_strlist (device, "info.interfaces");
  if (ifs == NULL)
    goto out;
  
  for (n = 0; ifs[n] != NULL; n++)
    {
      if (g_ascii_strcasecmp (ifs[n], interface) == 0)
        {
          ret = TRUE;
          break;
        }
    }

out:
  return ret;
}

gboolean
hal_device_has_property (HalDevice *device, const char *key)
{
  gboolean ret;
  LibHalPropertySetIterator it;

  ret = FALSE;
  if (device->priv->properties == NULL)
    goto out;
  
  libhal_psi_init (&it, device->priv->properties);
  
  while (libhal_psi_has_more (&it))
    {
      char *pkey = libhal_psi_get_key (&it);
      
      if (pkey != NULL && g_ascii_strcasecmp (pkey, key) == 0)
        {
          ret = TRUE;
          break;
        }
      libhal_psi_next (&it);
    }
  
 out:
  return ret;
}


HalDevice *
hal_device_new_from_udi (LibHalContext *hal_ctx, const char *udi)
{
  HalDevice *device;

  libhal_device_add_property_watch (hal_ctx, udi, NULL);
  
  device = HAL_DEVICE (g_object_new (HAL_TYPE_DEVICE, NULL));
  device->priv->udi = g_strdup (udi);
  device->priv->hal_ctx = hal_ctx;
  device->priv->properties = libhal_device_get_all_properties (hal_ctx, udi, NULL);
  return device;
}

HalDevice *
hal_device_new_from_udi_and_properties (LibHalContext *hal_ctx, 
                                        char *udi, 
                                        LibHalPropertySet *properties)
{
  HalDevice *device;
  
  libhal_device_add_property_watch (hal_ctx, udi, NULL);
  
  device = HAL_DEVICE (g_object_new (HAL_TYPE_DEVICE, NULL));
  device->priv->udi = g_strdup (udi);
  device->priv->hal_ctx = hal_ctx;
  device->priv->properties = properties;
  return device;
}

void
_hal_device_hal_property_changed (HalDevice *device, const char *key);

void
_hal_device_hal_condition (HalDevice *device, const char *name, const char *detail);

void
_hal_device_hal_property_changed (HalDevice *device, const char *key)
{
  LibHalPropertySet *new_props;
  
  new_props = libhal_device_get_all_properties (device->priv->hal_ctx, device->priv->udi, NULL);
  if (new_props != NULL)
    {
      libhal_free_property_set (device->priv->properties);
      device->priv->properties = new_props;
      g_signal_emit (device, signals[HAL_PROPERTY_CHANGED], 0, key);
    }
}

void
_hal_device_hal_condition (HalDevice *device, const char *name, const char *detail)
{
  g_signal_emit (device, signals[HAL_CONDITION], 0, name, detail);
}

const char *
hal_device_get_udi (HalDevice *device)
{
  return device->priv->udi;
}

LibHalPropertySet *
hal_device_get_properties (HalDevice *device)
{
  return device->priv->properties;
}

gboolean
hal_device_is_recently_plugged_in (HalDevice *device)
{
  GTimeVal now;
  glong delta_msec;

  g_get_current_time (&now);

  delta_msec = (now.tv_sec - device->priv->time_added.tv_sec) * 1000  + 
    (now.tv_usec - device->priv->time_added.tv_usec) / 1000;

  return delta_msec < 2000;
}
