/*
 * Copyright © 2008 Ryan Lortie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.   
 */

#ifndef _gvfsbackendtrash_h_
#define _gvfsbackendtrash_h_

#include <gvfsbackend.h>

#define G_VFS_TYPE_BACKEND_TRASH    (g_vfs_backend_trash_get_type ())
#define G_VFS_BACKEND_TRASH(obj)    (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
                                     G_VFS_TYPE_BACKEND_TRASH, \
                                     GVfsBackendTrash))

typedef struct OPAQUE_TYPE__GVfsBackendTrash GVfsBackendTrash;
GType g_vfs_backend_trash_get_type (void);

#endif /* _gvfsbackendtrash_h_ */
