#include <config.h>
#include "giotypes.h"
#include "gasyncresult.h"

static void g_async_result_base_init (gpointer g_class);
static void g_async_result_class_init (gpointer g_class,
				       gpointer class_data);

GType
g_async_result_get_type (void)
{
  static GType async_result_type = 0;

  if (! async_result_type)
    {
      static const GTypeInfo async_result_info =
      {
        sizeof (GAsyncResultIface), /* class_size */
	g_async_result_base_init,   /* base_init */
	NULL,		            /* base_finalize */
	g_async_result_class_init,
	NULL,		/* class_finalize */
	NULL,		/* class_data */
	0,
	0,              /* n_preallocs */
	NULL
      };

      async_result_type =
	g_type_register_static (G_TYPE_INTERFACE, I_("GAsyncResult"),
				&async_result_info, 0);

      g_type_interface_add_prerequisite (async_result_type, G_TYPE_OBJECT);
    }

  return async_result_type;
}

static void
g_async_result_class_init (gpointer g_class,
			   gpointer class_data)
{
}

static void
g_async_result_base_init (gpointer g_class)
{
}

gpointer
g_async_result_get_user_data (GAsyncResult *res)
{
  GAsyncResultIface *iface;

  iface = G_ASYNC_RESULT_GET_IFACE (res);

  return (* iface->get_user_data) (res);
}

GObject *
g_async_result_get_source_object (GAsyncResult *res)
{
  GAsyncResultIface *iface;

  iface = G_ASYNC_RESULT_GET_IFACE (res);

  return (* iface->get_source_object) (res);
}
