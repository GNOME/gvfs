#ifndef __G_UNIX_VOLUME_H__
#define __G_UNIX_VOLUME_H__

#include <glib-object.h>
#include <gio/gvolume.h>
#include <gio/gunixmounts.h>
#include <gio/gvolumemonitor.h>

G_BEGIN_DECLS

#define G_TYPE_UNIX_VOLUME        (g_unix_volume_get_type ())
#define G_UNIX_VOLUME(o)          (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_UNIX_VOLUME, GUnixVolume))
#define G_UNIX_VOLUME_CLASS(k)    (G_TYPE_CHECK_CLASS_CAST((k), G_TYPE_UNIX_VOLUME, GUnixVolumeClass))
#define G_IS_UNIX_VOLUME(o)       (G_TYPE_CHECK_INSTANCE_TYPE ((o), G_TYPE_UNIX_VOLUME))
#define G_IS_UNIX_VOLUME_CLASS(k) (G_TYPE_CHECK_CLASS_TYPE ((k), G_TYPE_UNIX_VOLUME))

typedef struct _GUnixVolume GUnixVolume;
typedef struct _GUnixVolumeClass GUnixVolumeClass;

struct _GUnixVolumeClass {
   GObjectClass parent_class;
};

GType g_unix_volume_get_type (void) G_GNUC_CONST;

GUnixVolume *g_unix_volume_new            (GVolumeMonitor *volume_monitor,
					   GUnixMount  *mount);
gboolean     g_unix_volume_has_mountpoint (GUnixVolume *volume,
					   const char  *mountpoint);

G_END_DECLS

#endif /* __G_UNIX_VOLUME_H__ */
