#ifndef __G_APP_INFO_H__
#define __G_APP_INFO_H__

#include <glib-object.h>
#include <gio/gicon.h>

G_BEGIN_DECLS

#define G_TYPE_APP_INFO            (g_app_info_get_type ())
#define G_APP_INFO(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_APP_INFO, GAppInfo))
#define G_IS_APP_INFO(obj)	   (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_APP_INFO))
#define G_APP_INFO_GET_IFACE(obj)  (G_TYPE_INSTANCE_GET_INTERFACE ((obj), G_TYPE_APP_INFO, GAppInfoIface))

typedef struct _GAppInfo         GAppInfo; /* Dummy typedef */
typedef struct _GAppInfoIface    GAppInfoIface;

struct _GAppInfoIface
{
  GTypeInterface g_iface;

  /* Virtual Table */

  GAppInfo *          (*dup)                (GAppInfo                *appinfo);
  gboolean            (*equal)              (GAppInfo                *appinfo1,
					     GAppInfo                *appinfo2);
  char *              (*get_name)           (GAppInfo                *appinfo);
  char *              (*get_description)    (GAppInfo                *appinfo);
  GIcon *             (*get_icon)           (GAppInfo                *appinfo);
  gboolean            (*launch)             (GAppInfo                *appinfo,
					     GList                   *filenames,
					     char                   **envp,
					     GError                 **error);
  gboolean            (*supports_uris)      (GAppInfo                *appinfo);
  gboolean            (*launch_uris)        (GAppInfo                *appinfo,
					     GList                   *uris,
					     char                   **envp,
					     GError                 **error);
  gboolean            (*should_show)        (GAppInfo                *appinfo,
					     const char              *desktop_env);
  gboolean            (*supports_xdg_startup_notify) (GAppInfo       *appinfo);
  gboolean            (*set_as_default_for_type) (GAppInfo           *appinfo,
						  const char         *content_type,
						  GError            **error);
};

GType g_app_info_get_type (void) G_GNUC_CONST;

GAppInfo *g_app_info_create_from_commandline     (const char  *commandline,
						  const char  *application_name,
						  GError **error);
GAppInfo *g_app_info_dup                         (GAppInfo    *appinfo);
gboolean  g_app_info_equal                       (GAppInfo    *appinfo1,
						  GAppInfo    *appinfo2);
char *    g_app_info_get_name                    (GAppInfo    *appinfo);
char *    g_app_info_get_description             (GAppInfo    *appinfo);
GIcon *   g_app_info_get_icon                    (GAppInfo    *appinfo);
gboolean  g_app_info_launch                      (GAppInfo    *appinfo,
						  GList       *filenames,
						  char       **envp,
						  GError     **error);
gboolean  g_app_info_supports_uris               (GAppInfo    *appinfo);
gboolean  g_app_info_supports_xdg_startup_notify (GAppInfo    *appinfo);
gboolean  g_app_info_launch_uris                 (GAppInfo    *appinfo,
						  GList       *uris,
						  char       **envp,
						  GError     **error);
gboolean  g_app_info_should_show                 (GAppInfo    *appinfo,
						  const char  *desktop_env);
gboolean  g_app_info_set_as_default_for_type     (GAppInfo    *appinfo,
						  const char  *content_type,
						  GError     **error);

GList *   g_get_all_app_info                     (void);
GList *   g_get_all_app_info_for_type            (const char  *content_type);
GAppInfo *g_get_default_app_info_for_type        (const char  *content_type);

/* TODO: Possibly missing operations:
   create new content type from extension
*/


G_END_DECLS

#endif /* __G_APP_INFO_H__ */
