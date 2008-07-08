/* hal-device.h
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

#ifndef HAL_DEVICE_H
#define HAL_DEVICE_H

#include <glib-object.h>
#include <gio/gio.h>
#include <libhal.h>

#define HAL_TYPE_DEVICE             (hal_device_get_type ())
#define HAL_DEVICE(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), HAL_TYPE_DEVICE, HalDevice))
#define HAL_DEVICE_CLASS(obj)       (G_TYPE_CHECK_CLASS_CAST ((obj), HAL_DEVICE,  HalDeviceClass))
#define HAL_IS_DEVICE(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), HAL_TYPE_DEVICE))
#define HAL_IS_DEVICE_CLASS(obj)    (G_TYPE_CHECK_CLASS_TYPE ((obj), HAL_TYPE_DEVICE))
#define HAL_DEVICE_GET_CLASS        (G_TYPE_INSTANCE_GET_CLASS ((obj), HAL_TYPE_DEVICE, HalDeviceClass))


typedef struct _HalDevice            HalDevice;
typedef struct _HalDeviceClass       HalDeviceClass;

struct _HalDevicePrivate;
typedef struct _HalDevicePrivate     HalDevicePrivate;

struct _HalDevice
{
  GObject parent;
        
  /* private */
  HalDevicePrivate *priv;
};

struct _HalDeviceClass
{
  GObjectClass parent_class;
  
  /* signals */
  void (*hal_property_changed) (HalDevice *device, const char *key);
  void (*hal_condition) (HalDevice *device, const char *name, const char *detail);
};


GType               hal_device_get_type                    (void);

HalDevice *         hal_device_new_from_udi                (LibHalContext     *hal_ctx, 
                                                            const char        *udi);

HalDevice *         hal_device_new_from_udi_and_properties (LibHalContext     *hal_ctx, 
                                                            char              *udi, 
                                                            LibHalPropertySet *properties);

const char *        hal_device_get_udi                     (HalDevice         *device);
LibHalPropertySet * hal_device_get_properties              (HalDevice         *device);
const char *        hal_device_get_property_string         (HalDevice         *device,
                                                            const char        *key);
int                 hal_device_get_property_int            (HalDevice         *device,
                                                            const char        *key);
guint64             hal_device_get_property_uint64         (HalDevice         *device,
                                                            const char        *key);
double              hal_device_get_property_double         (HalDevice         *device,
                                                            const char        *key);
gboolean            hal_device_get_property_bool           (HalDevice         *device,
                                                            const char        *key);
char **             hal_device_get_property_strlist        (HalDevice         *device,
                                                            const char        *key);

gboolean            hal_device_has_property                (HalDevice         *device,
                                                            const char        *key);
gboolean            hal_device_has_capability              (HalDevice         *device,
                                                            const char        *capability);
gboolean            hal_device_has_interface               (HalDevice         *device,
                                                            const char        *interface);

gboolean            hal_device_is_recently_plugged_in      (HalDevice         *device);

#endif /* HAL_DEVICE_H */
