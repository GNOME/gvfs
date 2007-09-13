#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "gunionvolume.h"
#include "gunionvolumemonitor.h"
#include "gvolumepriv.h"

typedef struct {
  GVolume *volume;
  GVolumeMonitor *monitor;
  guint32 changed_tag;
} ChildVolume;

struct _GUnionVolume {
  GObject parent;
  GVolumeMonitor *union_monitor;

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
  ChildVolume *child;
  GList *l;
  
  volume = G_UNION_VOLUME (object);

  if (volume->union_monitor)
    g_object_remove_weak_pointer (G_OBJECT (volume->union_monitor), (gpointer *)&volume->union_monitor);
  
  for (l = volume->child_volumes; l != NULL; l = l->next)
    {
      child = l->data;

      g_signal_handler_disconnect (child->volume, child->changed_tag);
      g_object_unref (child->volume);
      g_object_unref (child->monitor);
      g_free (child);
    }
  g_list_free (volume->child_volumes);
  
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
g_union_volume_new (GVolumeMonitor *union_monitor,
		    GVolume        *child_volume,
		    GVolumeMonitor *child_monitor)
{
  GUnionVolume *volume;
  
  volume = g_object_new (G_TYPE_UNION_VOLUME, NULL);

  volume->union_monitor = union_monitor;
  g_object_add_weak_pointer (G_OBJECT (volume->union_monitor),
			     (gpointer *)&volume->union_monitor);
  

  g_union_volume_add_volume (volume, child_volume, child_monitor);

  return volume;
}

static void
child_changed (GDrive *child_drive, GDrive *union_drive)
{
  g_signal_emit_by_name (union_drive, "changed");
}

void
g_union_volume_add_volume (GUnionVolume   *union_volume,
			   GVolume        *child_volume,
			   GVolumeMonitor *child_monitor)
{
  ChildVolume *child;
  
  child = g_new (ChildVolume, 1);
  child->volume = g_object_ref (child_volume);
  child->monitor = g_object_ref (child_monitor);
  child->changed_tag = g_signal_connect (child->volume, "changed", (GCallback)child_changed, union_volume);

  union_volume->child_volumes = g_list_prepend (union_volume->child_volumes, child);

  g_signal_emit_by_name (union_volume, "changed");
}

gboolean
g_union_volume_remove_volume (GUnionVolume   *union_volume,
			      GVolume        *child_volume)
{
  GList *l;
  ChildVolume *child;


  for (l = union_volume->child_volumes; l != NULL; l = l->next)
    {
      child = l->data;

      if (child->volume == child_volume)
	{
	  union_volume->child_volumes = g_list_delete_link (union_volume->child_volumes, l);
	  g_signal_handler_disconnect (child->volume, child->changed_tag);
	  g_object_unref (child->volume);
	  g_object_unref (child->monitor);
	  g_free (child);
	  
	  g_signal_emit_by_name (union_volume, "changed");
	  
	  return union_volume->child_volumes == NULL;
	}
    }
  
  return FALSE;
}

GVolume *
g_union_volume_get_child_for_monitor (GUnionVolume   *union_volume,
				      GVolumeMonitor *child_monitor)
{
  GList *l;
  ChildVolume *child;

  for (l = union_volume->child_volumes; l != NULL; l = l->next)
    {
      child = l->data;

      if (child->monitor == child_monitor)
	return g_object_ref (child->volume);
    }
  
  return NULL;
}

gboolean
g_union_volume_has_child_volume (GUnionVolume   *union_volume,
				 GVolume        *child_volume)
{
  GList *l;
  ChildVolume *child;

  for (l = union_volume->child_volumes; l != NULL; l = l->next)
    {
      child = l->data;

      if (child->volume == child_volume)
	return TRUE;
    }
  
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

static char *
g_union_volume_get_name (GVolume *volume)
{
  GUnionVolume *union_volume = G_UNION_VOLUME (volume);
  ChildVolume *child;

  if (union_volume->child_volumes)
    {
      child = union_volume->child_volumes->data;
      return g_volume_get_name (child->volume);
    }
  return g_strdup ("volume");
}

static char *
g_union_volume_get_icon (GVolume *volume)
{
  GUnionVolume *union_volume = G_UNION_VOLUME (volume);
  ChildVolume *child;

  if (union_volume->child_volumes)
    {
      child = union_volume->child_volumes->data;
      return g_volume_get_icon (child->volume);
    }
  return NULL;
}

static GFile *
g_union_volume_get_root (GVolume *volume)
{
  GUnionVolume *union_volume = G_UNION_VOLUME (volume);
  ChildVolume *child;

  if (union_volume->child_volumes)
    {
      child = union_volume->child_volumes->data;
      return g_volume_get_root (child->volume);
    }
  return NULL;
}

static GDrive *
g_union_volume_get_drive (GVolume *volume)
{
  GUnionVolume *union_volume = G_UNION_VOLUME (volume);
  ChildVolume *child;
  GDrive *child_drive, *union_drive;

  if (union_volume->child_volumes)
    {
      child = union_volume->child_volumes->data;
      child_drive = g_volume_get_drive (child->volume);
      union_drive = NULL;
      if (child_drive)
	{
	  union_drive = g_union_volume_monitor_convert_drive (G_UNION_VOLUME_MONITOR (union_volume->union_monitor),
							      child_drive);
	  g_object_unref (child_drive);
	}
      return union_drive;
    }
  return NULL;
}

static gboolean
g_union_volume_can_unmount (GVolume *volume)
{
  GUnionVolume *union_volume = G_UNION_VOLUME (volume);
  ChildVolume *child;

  if (union_volume->child_volumes)
    {
      child = union_volume->child_volumes->data;
      return g_volume_can_unmount (child->volume);
    }
  return FALSE;
}

static gboolean
g_union_volume_can_eject (GVolume *volume)
{
  GUnionVolume *union_volume = G_UNION_VOLUME (volume);
  ChildVolume *child;

  if (union_volume->child_volumes)
    {
      child = union_volume->child_volumes->data;
      return g_volume_can_eject (child->volume);
    }
  return FALSE;
}

typedef struct {
  GVolumeCallback callback;
  gpointer user_data;
} VolData;

static gboolean
error_on_idle (gpointer _data)
{
  VolData *data = _data;
  GError *error = NULL;

  g_set_error (&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("Operation not supported"));

  if (data->callback)
    data->callback (error, data->user_data);

  g_free (data);
  return FALSE;
}

static void
g_union_volume_unmount (GVolume         *volume,
			GVolumeCallback  callback,
			gpointer         user_data)
{
  GUnionVolume *union_volume = G_UNION_VOLUME (volume);
  ChildVolume *child;
  VolData *data;

  if (union_volume->child_volumes)
    {
      child = union_volume->child_volumes->data;
      return g_volume_unmount (child->volume,
			       callback, user_data);
    }

  data = g_new0 (VolData, 1);
  data->callback = callback;
  data->user_data = user_data;
  g_idle_add (error_on_idle, data);
}

static void
g_union_volume_eject (GVolume         *volume,
		      GVolumeCallback  callback,
		      gpointer         user_data)
{
  GUnionVolume *union_volume = G_UNION_VOLUME (volume);
  ChildVolume *child;
  VolData *data;

  if (union_volume->child_volumes)
    {
      child = union_volume->child_volumes->data;
      return g_volume_eject (child->volume,
			     callback, user_data);
    }
  
  data = g_new0 (VolData, 1);
  data->callback = callback;
  data->user_data = user_data;
  g_idle_add (error_on_idle, data);
}

static void
g_union_volue_volume_iface_init (GVolumeIface *iface)
{
  iface->get_platform_id = g_union_volume_get_platform_id;
  iface->get_name = g_union_volume_get_name;
  iface->get_icon = g_union_volume_get_icon;
  iface->get_root = g_union_volume_get_root;
  iface->get_drive = g_union_volume_get_drive;
  iface->can_unmount = g_union_volume_can_unmount;
  iface->can_eject = g_union_volume_can_eject;
  iface->unmount = g_union_volume_unmount;
  iface->eject = g_union_volume_eject;
}
