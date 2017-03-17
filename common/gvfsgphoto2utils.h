/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2009 Martin Pitt <martin.pitt@ubuntu.com>
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
 * Public License along with this library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef __G_VFS_GPHOTO2_UTILS_H__
#define __G_VFS_GPHOTO2_UTILS_H__

#include <glib.h>
#include <gudev/gudev.h>

char * g_vfs_get_volume_name (GUdevDevice *device, const char *device_id);
char * g_vfs_get_volume_icon (GUdevDevice *device);
char * g_vfs_get_volume_symbolic_icon (GUdevDevice *device);
char **g_vfs_get_x_content_types (GUdevDevice *device);

#endif
