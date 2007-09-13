#ifndef __G_THEMED_ICON_H__
#define __G_THEMED_ICON_H__

#include <gio/gicon.h>

G_BEGIN_DECLS

#define G_TYPE_THEMED_ICON         (g_themed_icon_get_type ())
#define G_THEMED_ICON(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_THEMED_ICON, GThemedIcon))
#define G_THEMED_ICON_CLASS(k)     (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_THEMED_ICON, GThemedIconClass))
#define G_IS_THEMED_ICON(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_THEMED_ICON))
#define G_IS_THEMED_ICON_CLASS(k)  (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_THEMED_ICON))
#define G_THEMED_ICON_GET_CLASS(o) (G_TYPE_INSTANCE_GET_CLASS ((o), G_TYPE_THEMED_ICON, GThemedIconClass))

typedef struct _GThemedIcon        GThemedIcon;
typedef struct _GThemedIconClass   GThemedIconClass;

struct _GThemedIconClass
{
  GObjectClass parent_class;
};

GType g_themed_icon_get_type (void) G_GNUC_CONST;
  
GIcon *g_themed_icon_new (const char *iconname);
GIcon *g_themed_icon_new_from_names (char **iconnames, int len);

char **g_themed_icon_get_names (GThemedIcon *icon);

G_END_DECLS

#endif /* __G_THEMED_ICON_H__ */
