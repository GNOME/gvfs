#ifndef __G_VFS_UNIX_H__
#define __G_VFS_UNIX_H__

#include <gvfs.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_UNIX			(g_vfs_unix_get_type ())
#define G_VFS_UNIX(obj)			(G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_VFS_UNIX, GVfsUnix))
#define G_VFS_UNIX_CLASS(klass)		(G_TYPE_CHECK_CLASS_CAST ((klass), G_TYPE_VFS_UNIX, GVfsUnixClass))
#define G_IS_VFS_UNIX(obj)			(G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_VFS_UNIX))
#define G_IS_VFS_UNIX_CLASS(klass)		(G_TYPE_CHECK_CLASS_TYPE ((klass), G_TYPE_VFS_UNIX))
#define G_VFS_UNIX_GET_CLASS(obj)		(G_TYPE_INSTANCE_GET_CLASS ((obj), G_TYPE_VFS_UNIX, GVfsUnixClass))


typedef struct _GVfsUnix       GVfsUnix;
typedef struct _GVfsUnixClass  GVfsUnixClass;

struct _GVfsUnixClass
{
  GObjectClass parent_class;
  
};

GType   g_vfs_unix_get_type  (void) G_GNUC_CONST;

GVfsUnix *g_vfs_unix_new (void);

G_END_DECLS

#endif /* __G_VFS_UNIX_H__ */
