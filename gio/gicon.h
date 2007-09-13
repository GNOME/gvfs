#ifndef __G_ICON_H__
#define __G_ICON_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define G_TYPE_ICON            (g_icon_get_type ())
#define G_ICON(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_ICON, GIcon))
#define G_IS_ICON(obj)	       (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_ICON))
#define G_ICON_GET_IFACE(obj)  (G_TYPE_INSTANCE_GET_INTERFACE ((obj), G_TYPE_ICON, GIconIface))

typedef struct _GIcon         		GIcon; /* Dummy typedef */
typedef struct _GIconIface    		GIconIface;


struct _GIconIface
{
  GTypeInterface g_iface;

  /* Virtual Table */

  guint               (*hash)               (GIcon                *icon);
  gboolean            (*equal)              (GIcon                *icon1,
					     GIcon                *icon2);
};

GType g_icon_get_type (void) G_GNUC_CONST;

guint    g_icon_hash  (gconstpointer  icon);
gboolean g_icon_equal (GIcon         *icon1,
		       GIcon         *icon2);

G_END_DECLS

#endif /* __G_ICON_H__ */
