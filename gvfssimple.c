#include <config.h>
#include "gvfssimple.h"

static void g_vfs_simple_class_init     (GVfsSimpleClass *class);
static void g_vfs_simple_vfs_iface_init (GVfsIface       *iface);
static void g_vfs_simple_finalize       (GObject         *object);

G_DEFINE_TYPE_WITH_CODE (GVfsSimple, g_vfs_simple, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_VFS,
						g_vfs_simple_vfs_iface_init))
 
static void
g_vfs_simple_class_init (GVfsSimpleClass *class)
{
  GObjectClass *object_class;
  
  object_class = (GObjectClass *) class;

  object_class->finalize = g_vfs_simple_finalize;
}

static void
g_vfs_simple_finalize (GObject *object)
{
  /* must chain up */
  G_OBJECT_CLASS (g_vfs_simple_parent_class)->finalize (object);
}

static void
g_vfs_simple_init (GVfsSimple *vfs)
{
}

GVfsSimple *
g_vfs_simple_new (void)
{
  return g_object_new (G_TYPE_VFS_SIMPLE, NULL);
}

GFile *
g_vfs_simple_get_file_for_path  (GVfs       *vfs,
				 const char *path)
{
  return NULL;
}

GFile *
g_vfs_simple_get_file_for_uri   (GVfs       *vfs,
				 const char *uri)
{
  char *path;

  path = g_filename_from_uri (uri, NULL, NULL);

  if (path != NULL)
    return g_vfs_simple_get_file_for_path (vfs, path);
  else
    return NULL;
}
  
GFile *
g_vfs_simple_parse_name (GVfs       *vfs,
			 const char *parse_name)
{
  return NULL;
}

static void
g_vfs_simple_vfs_iface_init (GVfsIface *iface)
{
  iface->get_file_for_path = g_vfs_simple_get_file_for_path;
  iface->get_file_for_uri = g_vfs_simple_get_file_for_uri;
  iface->parse_name = g_vfs_simple_parse_name;
}
