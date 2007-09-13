#include <config.h>
#include "gdrive.h"
#include <glib/gi18n-lib.h>

static void g_drive_base_init (gpointer g_class);
static void g_drive_class_init (gpointer g_class,
				 gpointer class_data);

GType
g_drive_get_type (void)
{
  static GType drive_type = 0;

  if (! drive_type)
    {
      static const GTypeInfo drive_info =
      {
        sizeof (GDriveIface), /* class_size */
	g_drive_base_init,   /* base_init */
	NULL,		/* base_finalize */
	g_drive_class_init,
	NULL,		/* class_finalize */
	NULL,		/* class_data */
	0,
	0,              /* n_preallocs */
	NULL
      };

      drive_type =
	g_type_register_static (G_TYPE_INTERFACE, I_("GDrive"),
				&drive_info, 0);

      g_type_interface_add_prerequisite (drive_type, G_TYPE_OBJECT);
    }

  return drive_type;
}

static void
g_drive_class_init (gpointer g_class,
		   gpointer class_data)
{
}

static void
g_drive_base_init (gpointer g_class)
{
  static gboolean initialized = FALSE;

  if (! initialized)
    {
      g_signal_new (I_("changed"),
                    G_TYPE_DRIVE,
                    G_SIGNAL_RUN_LAST,
                    G_STRUCT_OFFSET (GDriveIface, changed),
                    NULL, NULL,
                    g_cclosure_marshal_VOID__VOID,
                    G_TYPE_NONE, 0);

      initialized = TRUE;
    }
}

char *
g_drive_get_name (GDrive *drive)
{
  GDriveIface *iface;

  iface = G_DRIVE_GET_IFACE (drive);

  return (* iface->get_name) (drive);
}

char *
g_drive_get_icon (GDrive *drive)
{
  GDriveIface *iface;

  iface = G_DRIVE_GET_IFACE (drive);

  return (* iface->get_icon) (drive);
}
  
GList *
g_drive_get_volumes (GDrive *drive)
{
  GDriveIface *iface;

  iface = G_DRIVE_GET_IFACE (drive);

  return (* iface->get_volumes) (drive);
}

gboolean
g_drive_is_automounted (GDrive *drive)
{
  GDriveIface *iface;

  iface = G_DRIVE_GET_IFACE (drive);

  return (* iface->is_automounted) (drive);
}

gboolean
g_drive_can_mount (GDrive *drive)
{
  GDriveIface *iface;

  iface = G_DRIVE_GET_IFACE (drive);

  return (* iface->can_mount) (drive);
}

gboolean
g_drive_can_eject (GDrive *drive)
{
  GDriveIface *iface;

  iface = G_DRIVE_GET_IFACE (drive);

  return (* iface->can_eject) (drive);
}
  
void
g_drive_mount (GDrive         *drive,
	       GMountOperation *mount_operation,
	       GVolumeCallback  callback,
	       gpointer         user_data)
{
  GDriveIface *iface;

  iface = G_DRIVE_GET_IFACE (drive);

  return (* iface->mount) (drive, mount_operation, callback, user_data);
}
  
void
g_drive_eject (GDrive         *drive,
		GVolumeCallback  callback,
		gpointer         user_data)
{
  GDriveIface *iface;

  iface = G_DRIVE_GET_IFACE (drive);

  return (* iface->eject) (drive, callback, user_data);
}
