#include <config.h>
#include "gvolume.h"
#include "gvolumepriv.h"
#include "gsimpleasyncresult.h"
#include <glib/gi18n-lib.h>

static void g_volume_base_init (gpointer g_class);
static void g_volume_class_init (gpointer g_class,
				 gpointer class_data);

GType
g_volume_get_type (void)
{
  static GType volume_type = 0;

  if (! volume_type)
    {
      static const GTypeInfo volume_info =
      {
        sizeof (GVolumeIface), /* class_size */
	g_volume_base_init,   /* base_init */
	NULL,		/* base_finalize */
	g_volume_class_init,
	NULL,		/* class_finalize */
	NULL,		/* class_data */
	0,
	0,              /* n_preallocs */
	NULL
      };

      volume_type =
	g_type_register_static (G_TYPE_INTERFACE, I_("GVolume"),
				&volume_info, 0);

      g_type_interface_add_prerequisite (volume_type, G_TYPE_OBJECT);
    }

  return volume_type;
}

static void
g_volume_class_init (gpointer g_class,
		   gpointer class_data)
{
}

static void
g_volume_base_init (gpointer g_class)
{
  static gboolean initialized = FALSE;

  if (! initialized)
    {
      g_signal_new (I_("changed"),
                    G_TYPE_VOLUME,
                    G_SIGNAL_RUN_LAST,
                    G_STRUCT_OFFSET (GVolumeIface, changed),
                    NULL, NULL,
                    g_cclosure_marshal_VOID__VOID,
                    G_TYPE_NONE, 0);

      initialized = TRUE;
    }
}

GFile *
g_volume_get_root (GVolume *volume)
{
  GVolumeIface *iface;

  iface = G_VOLUME_GET_IFACE (volume);

  return (* iface->get_root) (volume);
}
  
char *
g_volume_get_name (GVolume *volume)
{
  GVolumeIface *iface;

  iface = G_VOLUME_GET_IFACE (volume);

  return (* iface->get_name) (volume);
}

GIcon *
g_volume_get_icon (GVolume *volume)
{
  GVolumeIface *iface;

  iface = G_VOLUME_GET_IFACE (volume);

  return (* iface->get_icon) (volume);
}
  
GDrive *
g_volume_get_drive (GVolume *volume)
{
  GVolumeIface *iface;

  iface = G_VOLUME_GET_IFACE (volume);

  return (* iface->get_drive) (volume);
}

gboolean
g_volume_can_unmount (GVolume *volume)
{
  GVolumeIface *iface;

  iface = G_VOLUME_GET_IFACE (volume);

  return (* iface->can_unmount) (volume);
}

gboolean
g_volume_can_eject (GVolume *volume)
{
  GVolumeIface *iface;

  iface = G_VOLUME_GET_IFACE (volume);

  return (* iface->can_eject) (volume);
}

void
g_volume_unmount (GVolume *volume,
		  GAsyncReadyCallback callback,
		  gpointer user_data)
{
  GVolumeIface *iface;
  
  iface = G_VOLUME_GET_IFACE (volume);

  if (iface->unmount == NULL)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (volume),
					   callback, user_data,
					   G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
					   _("volume doesn't implement unmount"));
      
      return;
    }
  
  return (* iface->unmount) (volume, callback, user_data);
}

gboolean
g_volume_unmount_finish (GVolume              *volume,
			 GAsyncResult         *result,
			 GError              **error)
{
  GVolumeIface *iface;

  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return FALSE;
    }
  
  iface = G_VOLUME_GET_IFACE (volume);
  return (* iface->unmount_finish) (volume, result, error);
}

void
g_volume_eject (GVolume         *volume,
		GAsyncReadyCallback  callback,
		gpointer         user_data)
{
  GVolumeIface *iface;

  iface = G_VOLUME_GET_IFACE (volume);

  if (iface->eject == NULL)
    {
      g_simple_async_report_error_in_idle (G_OBJECT (volume),
					   callback, user_data,
					   G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
					   _("volume doesn't implement eject"));
      
      return;
    }
  
  return (* iface->eject) (volume, callback, user_data);
}

gboolean
g_volume_eject_finish (GVolume              *volume,
		       GAsyncResult         *result,
		       GError              **error)
{
  GVolumeIface *iface;

  if (G_IS_SIMPLE_ASYNC_RESULT (result))
    {
      GSimpleAsyncResult *simple = G_SIMPLE_ASYNC_RESULT (result);
      if (g_simple_async_result_propagate_error (simple, error))
	return FALSE;
    }
  
  iface = G_VOLUME_GET_IFACE (volume);
  return (* iface->eject_finish) (volume, result, error);
}

char *
g_volume_get_platform_id (GVolume *volume)
{
  GVolumeIface *iface;

  iface = G_VOLUME_GET_IFACE (volume);

  return (* iface->get_platform_id) (volume);
}
