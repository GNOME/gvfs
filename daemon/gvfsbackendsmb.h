#ifndef __G_VFS_BACKEND_SMB_H__
#define __G_VFS_BACKEND_SMB_H__

#include <gvfsbackend.h>
#include <gmountspec.h>

#include <libsmbclient.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_BACKEND_SMB         (g_vfs_backend_smb_get_type ())
#define G_VFS_BACKEND_SMB(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS_BACKEND_SMB, GVfsBackendSmb))
#define G_VFS_BACKEND_SMB_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS_BACKEND_SMB, GVfsBackendSmbClass))
#define G_IS_VFS_BACKEND_SMB(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS_BACKEND_SMB))
#define G_IS_VFS_BACKEND_SMB_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS_BACKEND_SMB))
#define G_VFS_BACKEND_SMB_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS_BACKEND_SMB, GVfsBackendSmbClass))

typedef struct _GVfsBackendSmb        GVfsBackendSmb;
typedef struct _GVfsBackendSmbClass   GVfsBackendSmbClass;

struct _GVfsBackendSmb
{
  GVfsBackend parent_instance;

  char *server;
  char *share;
  
  SMBCCTX *smb_context;

  /* Cache */
  char *cached_server_name;
  char *cached_share_name;
  char *cached_domain;
  char *cached_username;
  SMBCSRV *cached_server;
};

struct _GVfsBackendSmbClass
{
  GVfsBackendClass parent_class;
};

GType g_vfs_backend_smb_get_type (void) G_GNUC_CONST;
  
GVfsBackendSmb *g_vfs_backend_smb_new (GMountSpec *mount_spec,
				       GError **error);

G_END_DECLS

#endif /* __G_VFS_BACKEND_SMB_H__ */
