/*
 * Copyright © 2012 Red Hat, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * See the included COPYING file for more information.
 */

#ifndef _gvfsbackendrecent_h_
#define _gvfsbackendrecent_h_

#include <gvfsbackend.h>

#define G_VFS_TYPE_BACKEND_RECENT    (g_vfs_backend_recent_get_type ())
#define G_VFS_BACKEND_RECENT(obj)    (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
                                     G_VFS_TYPE_BACKEND_RECENT, \
                                     GVfsBackendRecent))

typedef struct OPAQUE_TYPE__GVfsBackendRecent GVfsBackendRecent;
GType g_vfs_backend_recent_get_type (void);

#endif /* _gvfsbackendrecent_h_ */
