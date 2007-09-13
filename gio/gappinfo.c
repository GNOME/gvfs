#include <config.h>
#include "gappinfo.h"
#include <glib/gi18n-lib.h>

#include <giotypes.h>

static void g_app_info_base_init (gpointer g_class);
static void g_app_info_class_init (gpointer g_class,
				   gpointer class_data);


GType
g_app_info_get_type (void)
{
  static GType app_info_type = 0;

  if (! app_info_type)
    {
      static const GTypeInfo app_info_info =
      {
        sizeof (GAppInfoIface), /* class_size */
	g_app_info_base_init,   /* base_init */
	NULL,		/* base_finalize */
	g_app_info_class_init,
	NULL,		/* class_finalize */
	NULL,		/* class_data */
	0,
	0,              /* n_preallocs */
	NULL
      };

      app_info_type =
	g_type_register_static (G_TYPE_INTERFACE, I_("GAppInfo"),
				&app_info_info, 0);

      g_type_interface_add_prerequisite (app_info_type, G_TYPE_OBJECT);
    }

  return app_info_type;
}

static void
g_app_info_class_init (gpointer g_class,
		       gpointer class_data)
{
}

static void
g_app_info_base_init (gpointer g_class)
{
}

GAppInfo *
g_app_info_dup (GAppInfo *appinfo)
{
  GAppInfoIface *iface;

  iface = G_APP_INFO_GET_IFACE (appinfo);

  return (* iface->dup) (appinfo);
}
  
gboolean
g_app_info_equal (GAppInfo    *appinfo1,
		  GAppInfo    *appinfo2)
{
  GAppInfoIface *iface;

  if (G_TYPE_FROM_INSTANCE (appinfo1) != G_TYPE_FROM_INSTANCE (appinfo2))
    return FALSE;
  
  iface = G_APP_INFO_GET_IFACE (appinfo1);

  return (* iface->equal) (appinfo1, appinfo2);
}

char *
g_app_info_get_name (GAppInfo *appinfo)
{
  GAppInfoIface *iface;

  iface = G_APP_INFO_GET_IFACE (appinfo);

  return (* iface->get_name) (appinfo);
}

char *
g_app_info_get_description (GAppInfo *appinfo)
{
  GAppInfoIface *iface;

  iface = G_APP_INFO_GET_IFACE (appinfo);

  return (* iface->get_description) (appinfo);
}

gboolean
g_app_info_set_as_default_for_type (GAppInfo    *appinfo,
				    const char  *content_type,
				    GError     **error)
{
  GAppInfoIface *iface;

  iface = G_APP_INFO_GET_IFACE (appinfo);

  return (* iface->set_as_default_for_type) (appinfo, content_type, error);
}

GIcon *
g_app_info_get_icon (GAppInfo *appinfo)
{
  GAppInfoIface *iface;

  iface = G_APP_INFO_GET_IFACE (appinfo);

  return (* iface->get_icon) (appinfo);
}

gboolean
g_app_info_launch (GAppInfo    *appinfo,
		   GList       *filenames,
		   char       **envp,
		   GError     **error)
{
  GAppInfoIface *iface;

  iface = G_APP_INFO_GET_IFACE (appinfo);

  return (* iface->launch) (appinfo, filenames, envp, error);
}

gboolean
g_app_info_supports_uris (GAppInfo *appinfo)
{
  GAppInfoIface *iface;

  iface = G_APP_INFO_GET_IFACE (appinfo);

  return (* iface->supports_uris) (appinfo);
}

gboolean
g_app_info_supports_xdg_startup_notify (GAppInfo *appinfo)
{
  GAppInfoIface *iface;

  iface = G_APP_INFO_GET_IFACE (appinfo);

  return (* iface->supports_xdg_startup_notify) (appinfo);
}

gboolean
g_app_info_launch_uris (GAppInfo    *appinfo,
			GList       *uris,
			char       **envp,
			GError     **error)
{
  GAppInfoIface *iface;

  iface = G_APP_INFO_GET_IFACE (appinfo);

  return (* iface->launch) (appinfo, uris, envp, error);
}

gboolean
g_app_info_should_show (GAppInfo    *appinfo,
			const char  *desktop_env)
{
  GAppInfoIface *iface;

  iface = G_APP_INFO_GET_IFACE (appinfo);

  return (* iface->should_show) (appinfo, desktop_env);
}

