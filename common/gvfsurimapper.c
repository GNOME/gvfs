#include <config.h>
#include <string.h>
#include <gmodule.h>
#include "gvfsurimapper.h"

G_DEFINE_TYPE (GVfsUriMapper, g_vfs_uri_mapper, G_TYPE_OBJECT);

static void
g_vfs_uri_mapper_class_init (GVfsUriMapperClass *klass)
{
}

static void
g_vfs_uri_mapper_init (GVfsUriMapper *vfs)
{
}

const char **
g_vfs_uri_mapper_get_handled_schemes (GVfsUriMapper  *mapper)
{
  GVfsUriMapperClass *class;

  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);

  return (* class->get_handled_schemes) (mapper);
}

  
gboolean
g_vfs_uri_mapper_from_uri (GVfsUriMapper  *mapper,
			   const char     *uri,
			   GMountSpec    **spec_out,
			   char          **path_out)
{
  GVfsUriMapperClass *class;

  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);

  return (* class->from_uri) (mapper, uri, spec_out, path_out);
}

const char **
g_vfs_uri_mapper_get_handled_mount_types (GVfsUriMapper  *mapper)
{
  GVfsUriMapperClass *class;
  
  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);

  return (* class->get_handled_mount_types) (mapper);
}

char *
g_vfs_uri_mapper_to_uri (GVfsUriMapper  *mapper,
			 GMountSpec     *spec,
			 char           *path,
			 gboolean        allow_utf8)
{
  GVfsUriMapperClass *class;
  
  class = G_VFS_URI_MAPPER_GET_CLASS (mapper);
  
  return (* class->to_uri) (mapper, spec, path, allow_utf8);
}
