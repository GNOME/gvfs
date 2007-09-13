#ifndef __G_VFS_IMPLEMENTATION_H__
#define __G_VFS_IMPLEMENTATION_H__

#include <glib-object.h>
#include <gio/giotypes.h>
#include <gio/gfile.h>

G_BEGIN_DECLS

#define G_TYPE_VFS         (g_vfs_get_type ())
#define G_VFS(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_VFS, GVfs))
#define G_VFS_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_VFS, GVfsClass))
#define G_VFS_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_VFS, GVfsClass))
#define G_IS_VFS(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_VFS))
#define G_IS_VFS_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_VFS))

typedef struct _GVfs         GVfs; /* Dummy typedef */
typedef struct _GVfsClass    GVfsClass;

struct _GVfs {
  GObject parent;
};

struct _GVfsClass
{
  GObjectClass parent_class;

  /* Virtual Table */

  const char *(*get_name)          (GVfs *vfs);
  int         (*get_priority)      (GVfs *vfs);
  GFile      *(*get_file_for_path) (GVfs *vfs,
				    const char *path);
  GFile      *(*get_file_for_uri)  (GVfs *vfs,
				    const char *uri);
  GFile      *(*parse_name)        (GVfs *vfs,
				    const char *parse_name);
};

GType g_vfs_get_type (void) G_GNUC_CONST;

const char *g_vfs_get_name          (GVfs       *vfs);
int         g_vfs_get_priority      (GVfs       *vfs);
GFile *     g_vfs_get_file_for_path (GVfs       *vfs,
				     const char *path);
GFile *     g_vfs_get_file_for_uri  (GVfs       *vfs,
				     const char *uri);
GFile *     g_vfs_parse_name        (GVfs       *vfs,
				     const char *parse_name);

GVfs *      g_vfs_get_default       (void);

G_END_DECLS

#endif /* __G_VFS_H__ */
