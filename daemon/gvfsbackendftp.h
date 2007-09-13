#ifndef __G_VFS_BACKEND_FTP_H__
#define __G_VFS_BACKEND_FTP_H__

#include <gvfsbackend.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_FTP         (g_vfs_backend_ftp_get_type ())
#define G_VFS_BACKEND_FTP(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND_FTP, GVfsBackendFtp))
#define G_VFS_BACKEND_FTP_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_FTP, GVfsBackendFtpClass))
#define G_VFS_IS_BACKEND_FTP(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND_FTP))
#define G_VFS_IS_BACKEND_FTP_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND_FTP))
#define G_VFS_BACKEND_FTP_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND_FTP, GVfsBackendFtpClass))

typedef struct _GVfsBackendFtp        GVfsBackendFtp;
typedef struct _GVfsBackendFtpClass   GVfsBackendFtpClass;

struct _GVfsBackendFtpClass
{
  GVfsBackendClass parent_class;
};

GType g_vfs_backend_ftp_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif /* __G_VFS_BACKEND_FTP_H__ */
