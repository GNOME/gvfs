#include <config.h>
#include <string.h>
#include <gmodule.h>
#include "gvfs.h"
#include "glocalvfs.h"
#include "giomodule.h"
#include <glib/gi18n-lib.h>

G_DEFINE_TYPE (GVfs, g_vfs, G_TYPE_OBJECT);

static void
g_vfs_class_init (GVfsClass *klass)
{
}

static void
g_vfs_init (GVfs *vfs)
{
}

const char *
g_vfs_get_name (GVfs *vfs)
{
  GVfsClass *class;

  class = G_VFS_GET_CLASS (vfs);

  return (* class->get_name) (vfs);
}

int
g_vfs_get_priority (GVfs *vfs)
{
  GVfsClass *class;

  class = G_VFS_GET_CLASS (vfs);

  return (* class->get_priority) (vfs);
}

GFile *
g_vfs_get_file_for_path (GVfs *vfs,
			 const char *path)
{
  GVfsClass *class;

  class = G_VFS_GET_CLASS (vfs);

  return (* class->get_file_for_path) (vfs, path);
}

GFile *
g_vfs_get_file_for_uri  (GVfs *vfs,
			 const char *uri)
{
  GVfsClass *class;

  class = G_VFS_GET_CLASS (vfs);

  return (* class->get_file_for_uri) (vfs, uri);
}

GFile *
g_vfs_parse_name (GVfs *vfs,
		  const char *parse_name)
{
  GVfsClass *class;

  class = G_VFS_GET_CLASS (vfs);

  return (* class->parse_name) (vfs, parse_name);
}

static gpointer
get_default_vfs (gpointer arg)
{
  GType local_type;
  GType *vfs_impls;
  int i;
  guint n_vfs_impls;
  const char *use_this;
  GVfs *vfs, *max_prio_vfs;
  int max_prio;
  GType (*casted_get_type)(void);

  use_this = g_getenv ("GIO_USE_VFS");
  
  /* Ensure GLocalVfs type is availible
     the cast is required to avoid any G_GNUC_CONST optimizations */
  casted_get_type = g_local_vfs_get_type;
  local_type = casted_get_type ();
  
  /* Ensure vfs in modules loaded */
  g_io_modules_ensure_loaded (GIO_MODULE_DIR);

  vfs_impls = g_type_children (G_TYPE_VFS, &n_vfs_impls);

  max_prio = G_MININT;
  max_prio_vfs = NULL;
  for (i = 0; i < n_vfs_impls; i++)
    {
      vfs = g_object_new (vfs_impls[i], NULL);

      if (use_this && strcmp (g_vfs_get_name (vfs), use_this) == 0)
	{
	  max_prio = G_MAXINT;
	  if (max_prio_vfs)
	    g_object_unref (max_prio_vfs);
	  max_prio_vfs = g_object_ref (vfs);
	}

      if (max_prio < g_vfs_get_priority (vfs))
	{
	  max_prio = g_vfs_get_priority (vfs);
	  if (max_prio_vfs)
	    g_object_unref (max_prio_vfs);
	  max_prio_vfs = g_object_ref (vfs);
	}

      g_object_unref (vfs);
    }
  
  g_free (vfs_impls);

  /* We should at least have gotten the local implementation */
  g_assert (max_prio_vfs != NULL);

  return max_prio_vfs;
}

GVfs *
g_vfs_get_default (void)
{
  static GOnce once_init = G_ONCE_INIT;
  
  return g_once (&once_init, get_default_vfs, NULL);
}
