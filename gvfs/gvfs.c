#include <config.h>
#include "gvfs.h"
#include "gvfslocal.h"
#include "gvfsdaemon.h"
#include <glib/gi18n-lib.h>

static void g_vfs_base_init (gpointer g_class);

GType
g_vfs_get_type (void)
{
  static GType vfs_type = 0;

  if (! vfs_type)
    {
      static const GTypeInfo vfs_info =
      {
        sizeof (GVfsIface), /* class_size */
	g_vfs_base_init,   /* base_init */
	NULL,		/* base_finalize */
	NULL,
	NULL,		/* class_finalize */
	NULL,		/* class_data */
	0,
	0,              /* n_preallocs */
	NULL
      };

      vfs_type =
	g_type_register_static (G_TYPE_INTERFACE, I_("GVfs"),
				&vfs_info, 0);

      g_type_interface_add_prerequisite (vfs_type, G_TYPE_OBJECT);
    }

  return vfs_type;
}

static void
g_vfs_base_init (gpointer g_class)
{
}

GFile *
g_vfs_get_file_for_path (GVfs *vfs,
			 const char *path)
{
  GVfsIface *iface;

  iface = G_VFS_GET_IFACE (vfs);

  return (* iface->get_file_for_path) (vfs, path);
}

GFile *
g_vfs_get_file_for_uri  (GVfs *vfs,
			 const char *uri)
{
  GVfsIface *iface;

  iface = G_VFS_GET_IFACE (vfs);

  return (* iface->get_file_for_uri) (vfs, uri);
}

GFile *
g_vfs_parse_name (GVfs *vfs,
		  const char *parse_name)
{
  GVfsIface *iface;

  iface = G_VFS_GET_IFACE (vfs);

  return (* iface->parse_name) (vfs, parse_name);
}

static gpointer
get_default_vfs (gpointer arg)
{
  if (g_getenv ("VFS_USE_LOCAL") != NULL)
    return g_vfs_local_new ();
  else
    return g_vfs_daemon_new ();
}

GVfs *
g_vfs_get (void)
{
  static GOnce once_init = G_ONCE_INIT;
  GVfs *vfs;

  vfs = g_once (&once_init, get_default_vfs, NULL);
  
  return vfs;
}
