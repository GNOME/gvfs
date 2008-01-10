/* GIO - GLib Input, Output and Streaming Library
 *   Local testing backend wrapping error injection 
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
 * Author: Tomas Bzatek <tbzatek@redhat.com>
 */

#ifndef __G_VFS_BACKEND_LOCALTEST_H__
#define __G_VFS_BACKEND_LOCALTEST_H__

#include <gvfsbackend.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_LOCALTEST         (g_vfs_backend_localtest_get_type ())
#define G_VFS_BACKEND_LOCALTEST(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND_LOCALTEST, GVfsBackendLocalTest))
#define G_VFS_BACKEND_LOCALTEST_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_LOCALTEST, GVfsBackendLocalTestClass))
#define G_VFS_IS_BACKEND_LOCALTEST(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND_LOCALTEST))
#define G_VFS_IS_BACKEND_LOCALTEST_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND_LOCALTEST))
#define G_VFS_BACKEND_LOCALTEST_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND_LOCALTEST, GVfsBackendLocalTestClass))

typedef struct _GVfsBackendLocalTest        GVfsBackendLocalTest;
typedef struct _GVfsBackendLocalTestClass   GVfsBackendLocalTestClass;



typedef enum {
  GVFS_JOB_UNMOUNT = 1<<0,
  GVFS_JOB_MOUNT = 1<<1,
  GVFS_JOB_OPEN_FOR_READ = 1<<2,
  GVFS_JOB_CLOSE_READ = 1<<3,
  GVFS_JOB_READ = 1<<4,
  GVFS_JOB_SEEK_ON_READ = 1<<5,
  GVFS_JOB_CREATE = 1<<6,
  GVFS_JOB_APPEND_TO = 1<<7,
  GVFS_JOB_REPLACE = 1<<8,
  GVFS_JOB_CLOSE_WRITE = 1<<9,
  GVFS_JOB_WRITE = 1<<10,
  GVFS_JOB_SEEK_ON_WRITE = 1<<11,
  GVFS_JOB_QUERY_INFO = 1<<12,
  GVFS_JOB_QUERY_FS_INFO = 1<<13,
  GVFS_JOB_ENUMERATE = 1<<14,
  GVFS_JOB_SET_DISPLAY_NAME = 1<<15,
  GVFS_JOB_DELETE = 1<<16,
  GVFS_JOB_TRASH = 1<<17,
  GVFS_JOB_MAKE_DIRECTORY = 1<<18,
  GVFS_JOB_MAKE_SYMLINK = 1<<19,
  GVFS_JOB_COPY = 1<<20,
  GVFS_JOB_MOVE = 1<<21,
  GVFS_JOB_SET_ATTRIBUTE = 1<<22,
  GVFS_JOB_CREATE_DIR_MONITOR = 1<<23,
  GVFS_JOB_CREATE_FILE_MONITOR = 1<<24,
  GVFS_JOB_QUERY_SETTABLE_ATTRIBUTES = 1<<25,
  GVFS_JOB_QUERY_WRITABLE_NAMESPACES = 1<<26
} GVfsJobType;



struct _GVfsBackendLocalTest
{
	  GVfsBackend parent_instance;
	  const gchar *test;
	  GMountSpec *mount_spec;
	  int errorneous;
	  GVfsJobType inject_op_types;
};

struct _GVfsBackendLocalTestClass
{
  GVfsBackendClass parent_class;
};

GType g_vfs_backend_localtest_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif /* __G_VFS_BACKEND_LOCALTEST_H__ */
