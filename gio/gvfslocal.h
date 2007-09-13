#ifndef __G_VFS_LOCAL_H__
#define __G_VFS_LOCAL_H__

#include <gio/gvfs.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_LOCAL			(g_vfs_local_get_type ())
#define G_VFS_LOCAL(obj)			(G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_VFS_LOCAL, GVfsLocal))
#define G_VFS_LOCAL_CLASS(klass)		(G_TYPE_CHECK_CLASS_CAST ((klass), G_TYPE_VFS_LOCAL, GVfsLocalClass))
#define G_IS_VFS_LOCAL(obj)			(G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_VFS_LOCAL))
#define G_IS_VFS_LOCAL_CLASS(klass)		(G_TYPE_CHECK_CLASS_TYPE ((klass), G_TYPE_VFS_LOCAL))
#define G_VFS_LOCAL_GET_CLASS(obj)		(G_TYPE_INSTANCE_GET_CLASS ((obj), G_TYPE_VFS_LOCAL, GVfsLocalClass))


typedef struct _GVfsLocal       GVfsLocal;
typedef struct _GVfsLocalClass  GVfsLocalClass;

struct _GVfsLocalClass
{
  GObjectClass parent_class;
  
};

GType   g_vfs_local_get_type  (void) G_GNUC_CONST;

GVfs *g_vfs_local_new (void);


G_END_DECLS

#endif /* __G_VFS_LOCAL_H__ */
