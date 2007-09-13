#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "guniondrive.h"
#include "gunionvolumemonitor.h"
#include "gdrivepriv.h"

/* In general we don't expect collisions in drives
 * between HAL and unix-mounts. Either you use HAL to
 * enumerate removable devices, or user-mountable
 * entries in fstab. So, we don't merge drives, saving
 * considerable complexity, at the cost of having double
 * drives in weird cases (and these two drives would have
 * the same volume).
 */

struct _GUnionDrive {
  GObject parent;
  GVolumeMonitor *union_monitor;

  GDrive *child_drive;
  GVolumeMonitor *child_monitor;
};

static void g_union_volue_drive_iface_init (GDriveIface *iface);

G_DEFINE_TYPE_WITH_CODE (GUnionDrive, g_union_drive, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_DRIVE,
						g_union_volue_drive_iface_init))
  
static void
g_union_drive_finalize (GObject *object)
{
  GUnionDrive *drive;
  
  drive = G_UNION_DRIVE (object);
  
  if (drive->union_monitor)
    g_object_remove_weak_pointer (G_OBJECT (drive->union_monitor), (gpointer *)&drive->union_monitor);
  
  g_object_unref (drive->child_drive);
  g_object_unref (drive->child_monitor);
  
  if (G_OBJECT_CLASS (g_union_drive_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_union_drive_parent_class)->finalize) (object);
}

static void
g_union_drive_class_init (GUnionDriveClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_union_drive_finalize;
}

static void
g_union_drive_init (GUnionDrive *union_drive)
{
}


static void
child_changed (GDrive *child_drive, GDrive *union_drive)
{
  g_signal_emit_by_name (union_drive, "changed");
}

GUnionDrive *
g_union_drive_new (GVolumeMonitor *union_monitor,
		   GDrive *child_drive,
		   GVolumeMonitor *child_monitor)
{
  GUnionDrive *drive;
  
  drive = g_object_new (G_TYPE_UNION_DRIVE, NULL);

  drive->union_monitor = union_monitor;
  g_object_add_weak_pointer (G_OBJECT (drive->union_monitor),
			     (gpointer *)&drive->union_monitor);
  
  drive->child_drive = g_object_ref (child_drive);
  drive->child_monitor = g_object_ref (child_monitor);

  g_signal_connect_object (drive->child_drive, "changed", (GCallback)child_changed, drive, 0);
  
  return drive;
}

gboolean
g_union_drive_child_is_for_monitor (GUnionDrive    *union_drive,
				    GVolumeMonitor *child_monitor)
{
  return (union_drive->child_monitor == child_monitor);
}

gboolean
g_union_drive_is_for_child_drive (GUnionDrive *union_drive,
				  GDrive *child_drive)
{
  if (union_drive->child_drive == child_drive)
    return TRUE;
  
  return FALSE;
}

static char *
g_union_drive_get_name (GDrive *drive)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);

  return g_drive_get_name (union_drive->child_drive);
}

static char *
g_union_drive_get_icon (GDrive *drive)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);

  return g_drive_get_icon (union_drive->child_drive);
}

static gboolean
g_union_drive_is_automounted (GDrive *drive)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);

  return g_drive_is_automounted (union_drive->child_drive);
}

static GList *
g_union_drive_get_volumes (GDrive *drive)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);
  GList *child_volumes, *union_volumes;

  if (union_drive->union_monitor == NULL)
    return NULL;
  
  child_volumes = g_drive_get_volumes (union_drive->child_drive);

  union_volumes =
    g_union_volume_monitor_convert_volumes (G_UNION_VOLUME_MONITOR (union_drive->union_monitor),
					    child_volumes);
  g_list_foreach (child_volumes, (GFunc)g_object_unref, NULL);
  g_list_free (child_volumes);
  return union_volumes;
}

static gboolean
g_union_drive_can_mount (GDrive *drive)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);

  return g_drive_can_mount (union_drive->child_drive);
}

static gboolean
g_union_drive_can_eject (GDrive *drive)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);

  return g_drive_can_eject (union_drive->child_drive);
}

static void
g_union_drive_mount (GDrive         *drive,
		     GMountOperation *mount_operation,
		     GVolumeCallback callback,
		     gpointer        user_data)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);

  return g_drive_mount (union_drive->child_drive,
			mount_operation,
			callback, user_data);
}

static void
g_union_drive_eject (GDrive         *drive,
		     GVolumeCallback callback,
		     gpointer        user_data)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);

  return g_drive_eject (union_drive->child_drive,
			callback, user_data);
}

static void
g_union_volue_drive_iface_init (GDriveIface *iface)
{
  iface->get_name = g_union_drive_get_name;
  iface->get_icon = g_union_drive_get_icon;
  iface->is_automounted = g_union_drive_is_automounted;
  iface->get_volumes = g_union_drive_get_volumes;
  iface->can_mount = g_union_drive_can_mount;
  iface->can_eject = g_union_drive_can_eject;
  iface->mount = g_union_drive_mount;
  iface->eject = g_union_drive_eject;
}
