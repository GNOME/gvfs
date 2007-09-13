#include <config.h>

#include <string.h>

#include "gcontenttypeprivate.h"
#include "gwin32appinfo.h"
#include "gioerror.h"
#include <glib/gi18n-lib.h>
#include <glib/gstdio.h>

static void g_win32_app_info_iface_init (GAppInfoIface *iface);

struct _GWin32AppInfo
{
  GObject parent_instance;
  char *id;
  char *name;
  gboolean no_open_width;
};

G_DEFINE_TYPE_WITH_CODE (GWin32AppInfo, g_win32_app_info, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_APP_INFO,
						g_win32_app_info_iface_init))


static void
g_win32_app_info_finalize (GObject *object)
{
  GWin32AppInfo *info;

  info = G_WIN32_APP_INFO (object);

  g_free (info->id);
  g_free (info->name);
  
  if (G_OBJECT_CLASS (g_win32_app_info_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_win32_app_info_parent_class)->finalize) (object);
}

static void
g_win32_app_info_class_init (GWin32AppInfoClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_win32_app_info_finalize;
}

static void
g_win32_app_info_init (GWin32AppInfo *local)
{
}

static GAppInfo *
g_win32_app_info_dup (GAppInfo *appinfo)
{
  GWin32AppInfo *info = G_WIN32_APP_INFO (appinfo);
  GWin32AppInfo *new_info;
  
  new_info = g_object_new (G_TYPE_WIN32_APP_INFO, NULL);

  new_info->id = g_strdup (info->id);
  new_info->name = g_strdup (info->name);
  new_info->no_open_width = info->no_open_width;
  
  return G_APP_INFO (new_info);
}

static gboolean
g_win32_app_info_equal (GAppInfo *appinfo1,
			  GAppInfo *appinfo2)
{
  /*
  GWin32AppInfo *info1 = G_WIN32_APP_INFO (appinfo1);
  GWin32AppInfo *info2 = G_WIN32_APP_INFO (appinfo2);
  */
  return FALSE;
}

static char *
g_win32_app_info_get_name (GAppInfo *appinfo)
{
  GWin32AppInfo *info = G_WIN32_APP_INFO (appinfo);

  if (info->name == NULL)
    return g_strdup (_("Unnamed"));
  
  return g_strdup (info->name);
}

static char *
g_win32_app_info_get_description (GAppInfo *appinfo)
{
  return NULL;
}

static char *
g_win32_app_info_get_icon (GAppInfo *appinfo)
{
  /* GWin32AppInfo *info = G_WIN32_APP_INFO (appinfo); */

  /* TODO: How to handle icons */
  return NULL;
}

static gboolean
g_win32_app_info_launch (GAppInfo                *appinfo,
			   GList                   *filenames,
			   char                   **envp,
			   GError                 **error)
{
  return FALSE;
}

static gboolean
g_win32_app_info_supports_uris (GAppInfo *appinfo)
{
  return FALSE;
}

static gboolean
g_win32_app_info_launch_uris (GAppInfo *appinfo,
				GList *uris,
				char **envp,
				GError **error)
{
  return FALSE;
}

static gboolean
g_win32_app_info_should_show (GAppInfo *appinfo,
				const char *win32_env)
{
  GWin32AppInfo *info = G_WIN32_APP_INFO (appinfo);

  if (info->no_open_width)
    return FALSE;
  
  return TRUE;
}

static gboolean
g_win32_app_info_set_as_default_for_type (GAppInfo    *appinfo,
					    const char  *content_type,
					    GError     **error)
{
  return FALSE;
}

GAppInfo *
g_app_info_create_from_commandline (const char *commandline,
				    const char *application_name,
				    GError **error)
{
  return NULL;
}


static void
g_win32_app_info_iface_init (GAppInfoIface *iface)
{
  iface->dup = g_win32_app_info_dup;
  iface->equal = g_win32_app_info_equal;
  iface->get_name = g_win32_app_info_get_name;
  iface->get_description = g_win32_app_info_get_description;
  iface->get_icon = g_win32_app_info_get_icon;
  iface->launch = g_win32_app_info_launch;
  iface->supports_uris = g_win32_app_info_supports_uris;
  iface->launch_uris = g_win32_app_info_launch_uris;
  iface->should_show = g_win32_app_info_should_show;
  iface->set_as_default_for_type = g_win32_app_info_set_as_default_for_type;
}

GList *
g_get_all_app_info_for_type (const char *content_type)
{
  return NULL;
}

GAppInfo *
g_get_default_app_info_for_type (const char *content_type)
{
  return NULL;
}

GList *
g_get_all_app_info (void)
{
  return NULL;
}
