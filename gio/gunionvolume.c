#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "gunionvolume.h"
#include "gvolumepriv.h"

typedef struct {
  GVolume *volume;
  GVolumeMonitor *volume_monitor;
} ChildVolume;

struct _GUnionVolume {
  GObject parent;

  GList *child_volumes;
};

static void g_union_volue_volume_iface_init (GVolumeIface *iface);

G_DEFINE_TYPE_WITH_CODE (GUnionVolume, g_union_volume, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME,
						g_union_volue_volume_iface_init))


static void
g_union_volume_finalize (GObject *object)
{
  GUnionVolume *volume;
  
  volume = G_UNION_VOLUME (object);
  
  if (G_OBJECT_CLASS (g_union_volume_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_union_volume_parent_class)->finalize) (object);
}

static void
g_union_volume_class_init (GUnionVolumeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  gobject_class->finalize = g_union_volume_finalize;
}


static void
g_union_volume_init (GUnionVolume *union_volume)
{
}

GUnionVolume *
g_union_volume_new (GVolume        *child_volume,
		    GVolumeMonitor *child_monitor)
{
  GUnionVolume *volume;
  ChildVolume *child;
  
  volume = g_object_new (G_TYPE_UNION_VOLUME, NULL);

  child = g_new (ChildVolume, 1);
  child->volume = g_object_ref (child_volume);
  child->volume_monitor = g_object_ref (child_monitor);

  volume->child_volumes = g_list_prepend (volume->child_volumes, child);

  return volume;
}

void
g_union_volume_add_volume (GUnionVolume   *union_volume,
			   GVolume        *volume,
			   GVolumeMonitor *monitor)
{
  /* Should emit changed in idle */
}

gboolean
g_union_volume_remove_volume (GUnionVolume   *union_volume,
			      GVolume        *volume)
{
  /* Return TRUE if last volume */
  return FALSE;
}

static char *
g_union_volume_get_platform_id (GVolume *volume)
{
  GUnionVolume *union_volume = G_UNION_VOLUME (volume);
  ChildVolume *child;

  if (union_volume->child_volumes)
    {
      child = union_volume->child_volumes->data;
      return g_volume_get_platform_id (child->volume);
    }
  return NULL;
}

static void
g_union_volue_volume_iface_init (GVolumeIface *iface)
{
  iface->get_platform_id = g_union_volume_get_platform_id;
}
