/* hal-pool.h
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

#if !defined(HAL_POOL_H)
#define HAL_POOL_H

#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include "hal-device.h"

#define HAL_TYPE_POOL             (hal_pool_get_type ())
#define HAL_POOL(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), HAL_TYPE_POOL, HalPool))
#define HAL_POOL_CLASS(obj)       (G_TYPE_CHECK_CLASS_CAST ((obj), HAL_POOL,  HalPoolClass))
#define HAL_IS_POOL(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), HAL_TYPE_POOL))
#define HAL_IS_POOL_CLASS(obj)    (G_TYPE_CHECK_CLASS_TYPE ((obj), HAL_TYPE_POOL))
#define HAL_POOL_GET_CLASS        (G_TYPE_INSTANCE_GET_CLASS ((obj), HAL_TYPE_POOL, HalPoolClass))


typedef struct _HalPool            HalPool;
typedef struct _HalPoolClass       HalPoolClass;

struct _HalPoolPrivate;
typedef struct _HalPoolPrivate     HalPoolPrivate;

struct _HalPool
{
  GObject parent;
        
  /* private */
  HalPoolPrivate *priv;
};

struct _HalPoolClass
{
  GObjectClass parent_class;
  
  /* signals */
  void (*device_added) (HalPool *pool, HalDevice *device);
  void (*device_removed) (HalPool *pool, HalDevice *device);
  void (*device_property_changed) (HalPool *pool, HalDevice *device, const char *key);
  void (*device_condition) (HalPool *pool, HalDevice *device, const char *name, const char *detail);
};

GType            hal_pool_get_type                            (void);
HalPool *        hal_pool_new                                 (char        **cap_only);
LibHalContext *  hal_pool_get_hal_ctx                         (HalPool      *pool);
DBusConnection * hal_pool_get_dbus_connection                 (HalPool      *pool);
HalDevice *      hal_pool_get_device_by_udi                   (HalPool      *pool, 
                                                               const char   *udi);
HalDevice *      hal_pool_get_device_by_capability_and_string (HalPool      *pool, 
                                                               const char   *capability,
                                                               const char   *key,
                                                               const char   *value);
GList *          hal_pool_find_by_capability                  (HalPool      *pool, 
                                                               const char   *capability);

#endif /* HAL_POOL_H */
