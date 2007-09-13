#include <config.h>
#include <gmodule.h>
#include "gvfs.h"
#include "glocalvfs.h"
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


void
g_vfs_mount_for_location (GVfs                 *vfs,
			  GFile                *location,
			  GMountOperation      *mount_operation,
			  GAsyncReadyCallback   callback,
			  gpointer              user_data)
{
  GVfsIface *iface;

  iface = G_VFS_GET_IFACE (vfs);

  return (* iface->mount_for_location) (vfs, location, mount_operation, callback, user_data);
}

gboolean
g_vfs_mount_for_location_finish (GVfs                 *vfs,
				 GAsyncResult         *result,
				 GError              **error)
{
  GVfsIface *iface;

  iface = G_VFS_GET_IFACE (vfs);

  return (* iface->mount_for_location_finish) (vfs, result, error);
}


static gpointer
get_default_vfs (gpointer arg)
{
#ifdef G_OS_UNIX
  GModule *module;
  char *path;
  GVfs *vfs;
  GVfs *(*create_vfs) (void);

  if (g_getenv ("VFS_USE_LOCAL") != NULL)
    return g_local_vfs_new ();

  if (g_module_supported ())
    {
      /* TODO: Don't hardcode module name */
      path = g_module_build_path (GIO_MODULE_DIR, "libgvfsdbus.so");
      module = g_module_open (path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
      g_free (path);

      if (module == NULL) {
	g_warning ("Cannot load module `%s' (%s)", path, g_module_error ());
      } else if (g_module_symbol (module, "create_vfs", (gpointer *)&create_vfs))
	{
	  vfs = create_vfs ();
	  if (vfs)
	    return vfs;
	}
    }
#endif

  return g_local_vfs_new ();
}

GVfs *
g_vfs_get (void)
{
  static GOnce once_init = G_ONCE_INIT;
  GVfs *vfs;

  vfs = g_once (&once_init, get_default_vfs, NULL);
  
  return vfs;
}
