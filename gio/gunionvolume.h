#ifndef __G_UNION_VOLUME_H__
#define __G_UNION_VOLUME_H__

#include <glib-object.h>
#include <gio/gvolume.h>
#include <gio/gvolumemonitor.h>

G_BEGIN_DECLS

#define G_TYPE_UNION_VOLUME        (g_union_volume_get_type ())
#define G_UNION_VOLUME(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_UNION_VOLUME, GUnionVolume))
#define G_UNION_VOLUME_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_UNION_VOLUME, GUnionVolumeClass))
#define G_IS_UNION_VOLUME(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_UNION_VOLUME))
#define G_IS_UNION_VOLUME_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_UNION_VOLUME))

typedef struct _GUnionVolume GUnionVolume;
typedef struct _GUnionVolumeClass GUnionVolumeClass;

struct _GUnionVolumeClass {
   GObjectClass parent_class;
};

GType g_union_volume_get_type (void) G_GNUC_CONST;

GUnionVolume *g_union_volume_new           (GVolume        *volume,
					    GVolumeMonitor *monitor);
void          g_union_volume_add_volume    (GUnionVolume   *union_volume,
					    GVolume        *volume,
					    GVolumeMonitor *monitor);
gboolean      g_union_volume_remove_volume (GUnionVolume   *union_volume,
					    GVolume        *volume);

G_END_DECLS

#endif /* __G_UNION_VOLUME_H__ */
