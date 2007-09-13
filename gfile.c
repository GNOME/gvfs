#include <config.h>
#include "gfile.h"
#include "gvfs.h"
#include <glib/gi18n-lib.h>

static void g_file_base_init (gpointer g_class);


GType
g_file_get_type (void)
{
  static GType file_type = 0;

  if (! file_type)
    {
      static const GTypeInfo file_info =
      {
        sizeof (GFileIface), /* class_size */
	g_file_base_init,   /* base_init */
	NULL,		/* base_finalize */
	NULL,
	NULL,		/* class_finalize */
	NULL,		/* class_data */
	0,
	0,              /* n_preallocs */
	NULL
      };

      file_type =
	g_type_register_static (G_TYPE_INTERFACE, I_("GFile"),
				&file_info, 0);

      g_type_interface_add_prerequisite (file_type, G_TYPE_OBJECT);
    }

  return file_type;
}

static void
g_file_base_init (gpointer g_class)
{
}

gboolean
g_file_is_native (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->is_native) (file);
}

char *
g_file_get_path (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_path) (file);
}

char *
g_file_get_uri (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_uri) (file);
}


char *
g_file_get_parse_name (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_parse_name) (file);
}

GFile *
g_file_copy (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->copy) (file);
}


GFile *
g_file_get_parent (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_parent) (file);
}

GFile *
g_file_get_child (GFile *file,
		  const char *name)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_child) (file, name);
}

GFileEnumerator *
g_file_enumerate_children (GFile *file,
			   GFileInfoRequestFlags requested,
			   const char *attributes,
			   gboolean follow_symlinks)
			   
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->enumerate_children) (file, requested, attributes, follow_symlinks);
}

GFileInfo *
g_file_get_info (GFile *file,
		 GFileInfoRequestFlags requested,
		 const char *attributes,
		 gboolean follow_symlinks)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->get_info) (file, requested, attributes, follow_symlinks);
}

GFileInputStream *
g_file_read (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->read) (file);
}

GFileOutputStream *
g_file_append_to (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->append_to) (file);
}

GFileOutputStream *
g_file_create (GFile *file)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->create) (file);
}

GFileOutputStream *
g_file_replace (GFile                 *file,
		time_t                 mtime,
		gboolean               make_backup)
{
  GFileIface *iface;

  iface = G_FILE_GET_IFACE (file);

  return (* iface->replace) (file, mtime, make_backup);
}

/* Default vfs ops */

GFile *
g_file_get_for_path (const char *path)
{
  return g_vfs_get_file_for_path (g_vfs_get (),
				  path);
}
  
GFile *
g_file_get_for_uri (const char *uri)
{
  return g_vfs_get_file_for_uri (g_vfs_get (),
				 uri);
}
  
GFile *
g_file_parse_name (const char *parse_name)
{
  return g_vfs_parse_name (g_vfs_get (),
			   parse_name);
}
