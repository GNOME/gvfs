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

#ifndef __G_VFS_BACKEND_TEST_H__
#define __G_VFS_BACKEND_TEST_H__

#include <gvfsbackend.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_TEST         (g_vfs_backend_test_get_type ())
#define G_VFS_BACKEND_TEST(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND_TEST, GVfsBackendTest))
#define G_VFS_BACKEND_TEST_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_TEST, GVfsBackendTestClass))
#define G_VFS_IS_BACKEND_TEST(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND_TEST))
#define G_VFS_IS_BACKEND_TEST_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND_TEST))
#define G_VFS_BACKEND_TEST_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND_TEST, GVfsBackendTestClass))

typedef struct _GVfsBackendTest        GVfsBackendTest;
typedef struct _GVfsBackendTestClass   GVfsBackendTestClass;

struct _GVfsBackendTest
{
  GVfsBackend parent_instance;
};

struct _GVfsBackendTestClass
{
  GVfsBackendClass parent_class;
};

GType g_vfs_backend_test_get_type (void) G_GNUC_CONST;
  
GVfsBackendTest *g_vfs_backend_test_new (void);

G_END_DECLS

#endif /* __G_VFS_BACKEND_TEST_H__ */
