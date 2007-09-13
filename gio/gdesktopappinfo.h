#ifndef __G_DESKTOP_APP_INFO_H__
#define __G_DESKTOP_APP_INFO_H__

#include <gio/gappinfo.h>

G_BEGIN_DECLS

#define G_TYPE_DESKTOP_APP_INFO         (g_desktop_app_info_get_type ())
#define G_DESKTOP_APP_INFO(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_DESKTOP_APP_INFO, GDesktopAppInfo))
#define G_DESKTOP_APP_INFO_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_DESKTOP_APP_INFO, GDesktopAppInfoClass))
#define G_IS_DESKTOP_APP_INFO(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_DESKTOP_APP_INFO))
#define G_IS_DESKTOP_APP_INFO_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_DESKTOP_APP_INFO))
#define G_DESKTOP_APP_INFO_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_DESKTOP_APP_INFO, GDesktopAppInfoClass))

typedef struct _GDesktopAppInfo        GDesktopAppInfo;
typedef struct _GDesktopAppInfoClass   GDesktopAppInfoClass;

struct _GDesktopAppInfoClass
{
  GObjectClass parent_class;
};

GType g_desktop_app_info_get_type (void) G_GNUC_CONST;
  
GAppInfo *g_desktop_app_info_new (const char *desktop_id);

G_END_DECLS


#endif /* __G_DESKTOP_APP_INFO_H__ */
