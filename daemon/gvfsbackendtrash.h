#ifndef __G_VFS_BACKEND_TRASH_H__
#define __G_VFS_BACKEND_TRASH_H__

#include <gvfsbackend.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_BACKEND_TRASH         (g_vfs_backend_trash_get_type ())
#define G_VFS_BACKEND_TRASH(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_BACKEND_TRASH, GVfsBackendTrash))
#define G_VFS_BACKEND_TRASH_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_BACKEND_TRASH, GVfsBackendTrashClass))
#define G_VFS_IS_BACKEND_TRASH(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_BACKEND_TRASH))
#define G_VFS_IS_BACKEND_TRASH_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_BACKEND_TRASH))
#define G_VFS_BACKEND_TRASH_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_BACKEND_TRASH, GVfsBackendTrashClass))

typedef struct _GVfsBackendTrash        GVfsBackendTrash;
typedef struct _GVfsBackendTrashClass   GVfsBackendTrashClass;

struct _GVfsBackendTrashClass
{
  GVfsBackendClass parent_class;
};

GType g_vfs_backend_trash_get_type (void) G_GNUC_CONST;

G_END_DECLS

#endif /* __G_VFS_BACKEND_TRASH_H__ */
