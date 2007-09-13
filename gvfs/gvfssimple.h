#ifndef __G_VFS_SIMPLE_H__
#define __G_VFS_SIMPLE_H__

#include <gvfs.h>

G_BEGIN_DECLS

#define G_TYPE_VFS_SIMPLE			(g_vfs_simple_get_type ())
#define G_VFS_SIMPLE(obj)			(G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_VFS_SIMPLE, GVfsSimple))
#define G_VFS_SIMPLE_CLASS(klass)		(G_TYPE_CHECK_CLASS_CAST ((klass), G_TYPE_VFS_SIMPLE, GVfsSimpleClass))
#define G_IS_VFS_SIMPLE(obj)			(G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_VFS_SIMPLE))
#define G_IS_VFS_SIMPLE_CLASS(klass)		(G_TYPE_CHECK_CLASS_TYPE ((klass), G_TYPE_VFS_SIMPLE))
#define G_VFS_SIMPLE_GET_CLASS(obj)		(G_TYPE_INSTANCE_GET_CLASS ((obj), G_TYPE_VFS_SIMPLE, GVfsSimpleClass))


typedef struct _GVfsSimple       GVfsSimple;
typedef struct _GVfsSimpleClass  GVfsSimpleClass;

struct _GVfsSimpleClass
{
  GObjectClass parent_class;
  
};

GType   g_vfs_simple_get_type  (void) G_GNUC_CONST;

GVfs *g_vfs_simple_new (void);


G_END_DECLS

#endif /* __G_VFS_SIMPLE_H__ */
