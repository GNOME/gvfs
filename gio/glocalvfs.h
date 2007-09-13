#ifndef __G_LOCAL_VFS_H__
#define __G_LOCAL_VFS_H__

#include <gio/gvfs.h>

G_BEGIN_DECLS

#define G_TYPE_LOCAL_VFS			(g_local_vfs_get_type ())
#define G_LOCAL_VFS(obj)			(G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_LOCAL_VFS, GLocalVfs))
#define G_LOCAL_VFS_CLASS(klass)		(G_TYPE_CHECK_CLASS_CAST ((klass), G_TYPE_LOCAL_VFS, GLocalVfsClass))
#define G_IS_LOCAL_VFS(obj)			(G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_LOCAL_VFS))
#define G_IS_LOCAL_VFS_CLASS(klass)		(G_TYPE_CHECK_CLASS_TYPE ((klass), G_TYPE_LOCAL_VFS))
#define G_LOCAL_VFS_GET_CLASS(obj)		(G_TYPE_INSTANCE_GET_CLASS ((obj), G_TYPE_LOCAL_VFS, GLocalVfsClass))


typedef struct _GLocalVfs       GLocalVfs;
typedef struct _GLocalVfsClass  GLocalVfsClass;

struct _GLocalVfsClass
{
  GObjectClass parent_class;
  
};

GType   g_local_vfs_get_type  (void) G_GNUC_CONST;

GVfs *g_local_vfs_new (void);


G_END_DECLS

#endif /* __G_LOCAL_VFS_H__ */
