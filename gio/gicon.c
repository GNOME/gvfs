#include <config.h>
#include "gicon.h"

#include "giotypes.h"
#include <glib/gi18n-lib.h>

static void g_icon_base_init (gpointer g_class);
static void g_icon_class_init (gpointer g_class,
			       gpointer class_data);

GType
g_icon_get_type (void)
{
  static GType icon_type = 0;

  if (! icon_type)
    {
      static const GTypeInfo icon_info =
      {
        sizeof (GIconIface), /* class_size */
	g_icon_base_init,   /* base_init */
	NULL,		/* base_finalize */
	g_icon_class_init,
	NULL,		/* class_finalize */
	NULL,		/* class_data */
	0,
	0,              /* n_preallocs */
	NULL
      };

      icon_type =
	g_type_register_static (G_TYPE_INTERFACE, I_("GIcon"),
				&icon_info, 0);

      g_type_interface_add_prerequisite (icon_type, G_TYPE_OBJECT);
    }

  return icon_type;
}

static void
g_icon_class_init (gpointer g_class,
		   gpointer class_data)
{
}

static void
g_icon_base_init (gpointer g_class)
{
}

guint
g_icon_hash (gconstpointer icon)
{
  GIconIface *iface;

  iface = G_ICON_GET_IFACE (icon);

  return (* iface->hash) ((GIcon *)icon);
}

gboolean
g_icon_equal (GIcon *icon1,
	      GIcon *icon2)
{
  GIconIface *iface;
  
  if (G_TYPE_FROM_INSTANCE (icon1) != G_TYPE_FROM_INSTANCE (icon2))
    return FALSE;

  iface = G_ICON_GET_IFACE (icon1);
  
  return (* iface->equal) (icon1, icon2);
}

