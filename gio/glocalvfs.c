#include <config.h>
#include "glocalvfs.h"
#include "glocalfile.h"

struct _GLocalVfs
{
  GVfs parent;
};

struct _GLocalVfsClass
{
  GVfsClass parent_class;
  
};

G_DEFINE_TYPE (GLocalVfs, g_local_vfs, G_TYPE_VFS)
 
static void
g_local_vfs_finalize (GObject *object)
{
  /* must chain up */
  G_OBJECT_CLASS (g_local_vfs_parent_class)->finalize (object);
}

static void
g_local_vfs_init (GLocalVfs *vfs)
{
}

GVfs *
g_local_vfs_new (void)
{
  return g_object_new (G_TYPE_LOCAL_VFS, NULL);
}

static GFile *
g_local_vfs_get_file_for_path  (GVfs       *vfs,
				const char *path)
{
  return g_local_file_new (path);
}

static GFile *
g_local_vfs_get_file_for_uri   (GVfs       *vfs,
				const char *uri)
{
  char *path;
  GFile *file;

  path = g_filename_from_uri (uri, NULL, NULL);

  if (path != NULL)
    file = g_local_file_new (path);
  else
    file = NULL;

  g_free (path);

  return file;
}

static GFile *
g_local_vfs_parse_name (GVfs       *vfs,
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
    
  file = g_local_file_new (filename);
  g_free (filename);

  return file;
}

static const char *
g_local_vfs_get_name (GVfs *vfs)
{
  return "local";
}

static int
g_local_vfs_get_priority (GVfs *vfs)
{
  return 0;
}

static void
g_local_vfs_class_init (GLocalVfsClass *class)
{
  GObjectClass *object_class;
  GVfsClass *vfs_class;
  
  object_class = (GObjectClass *) class;

  object_class->finalize = g_local_vfs_finalize;

  vfs_class = G_VFS_CLASS (class);
  
  vfs_class->get_name = g_local_vfs_get_name;
  vfs_class->get_priority = g_local_vfs_get_priority;
  vfs_class->get_file_for_path = g_local_vfs_get_file_for_path;
  vfs_class->get_file_for_uri = g_local_vfs_get_file_for_uri;
  vfs_class->parse_name = g_local_vfs_parse_name;
}
