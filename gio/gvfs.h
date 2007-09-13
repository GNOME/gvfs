#ifndef __G_VFS_IMPLEMENTATION_H__
#define __G_VFS_IMPLEMENTATION_H__

#include <glib-object.h>
#include <gio/giotypes.h>
#include <gio/gfile.h>

G_BEGIN_DECLS

#define G_TYPE_VFS           (g_vfs_get_type ())
#define G_VFS(obj)           (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_VFS, GVfs))
#define G_IS_VFS(obj)	     (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_VFS))
#define G_VFS_GET_IFACE(obj) (G_TYPE_INSTANCE_GET_INTERFACE ((obj), G_TYPE_VFS, GVfsIface))

typedef struct _GVfs         GVfs; /* Dummy typedef */
typedef struct _GVfsIface    GVfsIface;

struct _GVfsIface
{
  GTypeInterface g_iface;

  /* Virtual Table */

  GFile *(*get_file_for_path) (GVfs *vfs,
			       const char *path);
  GFile *(*get_file_for_uri)  (GVfs *vfs,
			       const char *uri);
  GFile *(*parse_name)        (GVfs *vfs,
			       const char *parse_name);
};

GType g_vfs_get_type (void) G_GNUC_CONST;

GFile *g_vfs_get_file_for_path (GVfs       *vfs,
				const char *path);
GFile *g_vfs_get_file_for_uri  (GVfs       *vfs,
				const char *uri);
GFile *g_vfs_parse_name        (GVfs       *vfs,
				const char *parse_name);

GVfs *g_vfs_get (void);

G_END_DECLS

#endif /* __G_VFS_H__ */
