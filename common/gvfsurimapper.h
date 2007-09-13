#ifndef __G_VFS_URI_MAPPER_H__
#define __G_VFS_URI_MAPPER_H__

#include <glib-object.h>
#include <gvfsuriutils.h>
#include <gmountspec.h>

G_BEGIN_DECLS

#define G_VFS_TYPE_URI_MAPPER         (g_vfs_uri_mapper_get_type ())
#define G_VFS_URI_MAPPER(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_VFS_TYPE_URI_MAPPER, GVfsUriMapper))
#define G_VFS_URI_MAPPER_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_VFS_TYPE_URI_MAPPER, GVfsUriMapperClass))
#define G_VFS_URI_MAPPER_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_VFS_TYPE_URI_MAPPER, GVfsUriMapperClass))
#define G_IS_VFS_URI_MAPPER(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_VFS_TYPE_URI_MAPPER))
#define G_IS_VFS_URI_MAPPER_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_VFS_TYPE_URI_MAPPER))

typedef struct _GVfsUriMapper         GVfsUriMapper; /* Dummy typedef */
typedef struct _GVfsUriMapperClass    GVfsUriMapperClass;

struct _GVfsUriMapper {
  GObject parent;
};

struct _GVfsUriMapperClass
{
  GObjectClass parent_class;

  /* Virtual Table */

  const char ** (*get_handled_schemes)     (GVfsUriMapper *mapper);
  gboolean      (*from_uri)                (GVfsUriMapper *mapper,
					    const char *uri,
					    GMountSpec **spec_out,
					    char **path_out);
  
  const char ** (*get_handled_mount_types) (GVfsUriMapper *mapper);
  char *        (*to_uri)                  (GVfsUriMapper *mapper,
					    GMountSpec *spec,
					    char *path,
					    gboolean allow_utf8);
};

GType g_vfs_uri_mapper_get_type (void) G_GNUC_CONST;

const char **g_vfs_uri_mapper_get_handled_schemes     (GVfsUriMapper  *mapper);
gboolean     g_vfs_uri_mapper_from_uri                (GVfsUriMapper  *mapper,
						       const char     *uri,
						       GMountSpec    **spec_out,
						       char          **path_out);

const char **g_vfs_uri_mapper_get_handled_mount_types (GVfsUriMapper  *mapper);
char *       g_vfs_uri_mapper_to_uri                  (GVfsUriMapper  *mapper,
						       GMountSpec     *spec,
						       char           *path,
						       gboolean        allow_utf8);

G_END_DECLS

#endif /* __G_VFS_URI_MAPPER_H__ */
