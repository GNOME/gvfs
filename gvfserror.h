#ifndef __G_VFS_ERROR_H__
#define __G_VFS_ERROR_H__

#include <glib-object.h>

G_BEGIN_DECLS

GQuark          g_vfs_error_quark      (void);

#define G_VFS_ERROR g_vfs_error_quark()

void g_vfs_error_from_errno (GError **error,
			     int err_no);

typedef enum
{
  G_VFS_ERROR_INTERNAL_ERROR,
  G_VFS_ERROR_INVALID_ARGUMENT,
  G_VFS_ERROR_CLOSED,
  G_VFS_ERROR_CANCELLED,
  G_VFS_ERROR_PENDING,
  G_VFS_ERROR_IO,
} GVfsError;

#endif /* __G_VFS_ERROR_H__ */
