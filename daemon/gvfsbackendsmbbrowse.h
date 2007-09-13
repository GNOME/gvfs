#ifndef __G_VFS_BACKEND_SMB_BROWSE_H__
#define __G_VFS_BACKEND_SMB_BROWSE_H__

#include <gvfsbackend.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_SMB_BROWSE         (g_vfs_backend_smb_browse_get_type ())
#define G_VFS_BACKEND_SMB_BROWSE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND_SMB_BROWSE, GVfsBackendSmbBrowse))
#define G_VFS_BACKEND_SMB_BROWSE_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_SMB_BROWSE, GVfsBackendSmbBrowseClass))
#define G_VFS_IS_BACKEND_SMB_BROWSE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND_SMB_BROWSE))
#define G_VFS_IS_BACKEND_SMB_BROWSE_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND_SMB_BROWSE))
#define G_VFS_BACKEND_SMB_BROWSE_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND_SMB_BROWSE, GVfsBackendSmbBrowseClass))

typedef struct _GVfsBackendSmbBrowse        GVfsBackendSmbBrowse;
typedef struct _GVfsBackendSmbBrowseClass   GVfsBackendSmbBrowseClass;

struct _GVfsBackendSmbBrowseClass
{
  GVfsBackendClass parent_class;
};

GType g_vfs_backend_smb_browse_get_type (void) G_GNUC_CONST;
  
G_END_DECLS

#endif /* __G_VFS_BACKEND_SMB_BROWSE_H__ */
