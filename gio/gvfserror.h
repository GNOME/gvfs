#ifndef __G_VFS_ERROR_H__
#define __G_VFS_ERROR_H__

#include <glib-object.h>

G_BEGIN_DECLS

GQuark          g_vfs_error_quark      (void);

#define G_VFS_ERROR g_vfs_error_quark()

/* Don't add stuff here that alread exist in GFileError (in gfileutils.h) */
typedef enum
{
  G_VFS_ERROR_INTERNAL_ERROR,
  G_VFS_ERROR_NOT_MOUNTED,
  G_VFS_ERROR_ALREADY_MOUNTED,
  G_VFS_ERROR_CLOSED,
  G_VFS_ERROR_CANCELLED,
  G_VFS_ERROR_PENDING,
  G_VFS_ERROR_NOT_SUPPORTED,
  G_VFS_ERROR_READ_ONLY,
  G_VFS_ERROR_NOT_REGULAR_FILE,
  G_VFS_ERROR_NOT_SYMBOLIC_LINK,
  G_VFS_ERROR_CANT_CREATE_BACKUP,
  G_VFS_ERROR_WRONG_MTIME,
  G_VFS_ERROR_INVALID_FILENAME,
} GVfsError;

#endif /* __G_VFS_ERROR_H__ */
