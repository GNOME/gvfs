#include <config.h>
#include "gvfslocal.h"
#include "gfilelocal.h"

static void g_vfs_local_class_init     (GVfsLocalClass *class);
static void g_vfs_local_vfs_iface_init (GVfsIface       *iface);
static void g_vfs_local_finalize       (GObject         *object);

struct _GVfsLocal
{
  GObject parent;
};

G_DEFINE_TYPE_WITH_CODE (GVfsLocal, g_vfs_local, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_VFS,
						g_vfs_local_vfs_iface_init))
 
static void
g_vfs_local_class_init (GVfsLocalClass *class)
{
  GObjectClass *object_class;
  
  object_class = (GObjectClass *) class;

  object_class->finalize = g_vfs_local_finalize;
}

static void
g_vfs_local_finalize (GObject *object)
{
  /* must chain up */
  G_OBJECT_CLASS (g_vfs_local_parent_class)->finalize (object);
}

static void
g_vfs_local_init (GVfsLocal *vfs)
{
}

GVfs *
g_vfs_local_new (void)
{
  return g_object_new (G_TYPE_VFS_LOCAL, NULL);
}

static GFile *
g_vfs_local_get_file_for_path  (GVfs       *vfs,
				const char *path)
{
  return g_file_local_new (path);
}

static GFile *
g_vfs_local_get_file_for_uri   (GVfs       *vfs,
				const char *uri)
{
  char *path;
  GFile *file;

  path = g_filename_from_uri (uri, NULL, NULL);

  if (path != NULL)
    file = g_file_local_new (path);
  else
    file = NULL;

  g_free (path);

  return file;
}

static GFile *
g_vfs_local_parse_name (GVfs       *vfs,
			const char *parse_name)
{
  GFile *file;
  char *filename;
  
  g_return_val_if_fail (G_IS_VFS (vfs), NULL);
  g_return_val_if_fail (parse_name != NULL, NULL);

  if (g_ascii_strncasecmp ("file:", parse_name, 5))
    filename = g_filename_from_uri (parse_name, NULL, NULL);
  else
    filename = g_filename_from_utf8 (parse_name, -1, NULL, NULL, NULL);
    
  file = g_file_local_new (filename);
  g_free (filename);

  return file;
}

static void
g_vfs_local_vfs_iface_init (GVfsIface *iface)
{
  iface->get_file_for_path = g_vfs_local_get_file_for_path;
  iface->get_file_for_uri = g_vfs_local_get_file_for_uri;
  iface->parse_name = g_vfs_local_parse_name;
}
