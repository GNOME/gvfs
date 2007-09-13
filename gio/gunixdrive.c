#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "gunixdrive.h"
#include "gunixvolume.h"
#include "gdrivepriv.h"
#include "gvolumemonitor.h"

struct _GUnixDrive {
  GObject parent;

  GUnixVolume *volume; /* owned by volume monitor */
  char *name;
  char *icon;
  char *mountpoint;
  GUnixMountType guessed_type;
};

static void g_unix_volume_drive_iface_init (GDriveIface *iface);

G_DEFINE_TYPE_WITH_CODE (GUnixDrive, g_unix_drive, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_DRIVE,
						g_unix_volume_drive_iface_init))

static void
g_unix_drive_finalize (GObject *object)
{
  GUnixDrive *drive;
  
  drive = G_UNIX_DRIVE (object);

  if (drive->volume)
    g_unix_volume_unset_drive (drive->volume, drive);
  
  g_free (drive->name);
  g_free (drive->icon);
  g_free (drive->mountpoint);

  if (G_OBJECT_CLASS (g_unix_drive_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_unix_drive_parent_class)->finalize) (object);
}

static void
g_unix_drive_class_init (GUnixDriveClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_unix_drive_finalize;
}

static void
g_unix_drive_init (GUnixDrive *unix_drive)
{
}

static char *
type_to_icon (GUnixMountType type)
{
  const char *icon_name = NULL;
  
  switch (type)
    {
    case G_UNIX_MOUNT_TYPE_HD:
      icon_name = "drive-harddisk";
      break;
    case G_UNIX_MOUNT_TYPE_FLOPPY:
    case G_UNIX_MOUNT_TYPE_ZIP:
    case G_UNIX_MOUNT_TYPE_JAZ:
    case G_UNIX_MOUNT_TYPE_MEMSTICK:
      icon_name = "drive-removable-media";
      break;
    case G_UNIX_MOUNT_TYPE_CDROM:
      icon_name = "drive-optical";
      break;
    case G_UNIX_MOUNT_TYPE_NFS:
      /* TODO: Would like a better icon here... */
      icon_name = "drive-removable-media";
      break;
    case G_UNIX_MOUNT_TYPE_CAMERA:
      icon_name = "camera-photo";
      break;
    case G_UNIX_MOUNT_TYPE_IPOD:
      icon_name = "multimedia-player";
      break;
    case G_UNIX_MOUNT_TYPE_UNKNOWN:
    default:
      icon_name = "drive-removable-media";
      break;
    }
  return g_strdup (icon_name);
}

GUnixDrive *
g_unix_drive_new (GVolumeMonitor *volume_monitor,
		  GUnixMountPoint *mountpoint)
{
  GUnixDrive *drive;
  
  if (!(mountpoint->is_user_mountable ||
	g_str_has_prefix (mountpoint->device_path, "/vol/")) ||
      mountpoint->is_loopback)
    return NULL;
  
  drive = g_object_new (G_TYPE_UNIX_DRIVE, NULL);

  drive->guessed_type = _g_guess_type_for_mount (mountpoint->mount_path,
						 mountpoint->device_path,
						 mountpoint->filesystem_type);
  
  /* TODO: */
  drive->mountpoint = g_strdup (mountpoint->mount_path);
  drive->icon = type_to_icon (drive->guessed_type);
  drive->name = g_strdup (_("Unknown drive"));
  
  return drive;
}

void
g_unix_drive_disconnected (GUnixDrive *drive)
{
  if (drive->volume)
    {
      g_unix_volume_unset_drive (drive->volume, drive);
      drive->volume = NULL;
    }
}

void
g_unix_drive_set_volume (GUnixDrive     *drive,
			 GUnixVolume    *volume)
{
  if (drive->volume == volume)
    return;
  
  if (drive->volume)
    g_unix_volume_unset_drive (drive->volume, drive);
  
  drive->volume = volume;
  
  /* TODO: Emit changed in idle to avoid locking issues */
  g_signal_emit_by_name (drive, "changed");
}

void
g_unix_drive_unset_volume (GUnixDrive     *drive,
			   GUnixVolume    *volume)
{
  if (drive->volume == volume)
    {
      drive->volume = NULL;
      /* TODO: Emit changed in idle to avoid locking issues */
      g_signal_emit_by_name (drive, "changed");
    }
}

static char *
g_unix_drive_get_icon (GDrive *drive)
{
  GUnixDrive *unix_drive = G_UNIX_DRIVE (drive);

  return g_strdup (unix_drive->icon);
}

static char *
g_unix_drive_get_name (GDrive *drive)
{
  GUnixDrive *unix_drive = G_UNIX_DRIVE (drive);
  
  return g_strdup (unix_drive->name);
}

static gboolean
g_unix_drive_is_automounted (GDrive *drive)
{
  /* TODO */
  return FALSE;
}

static gboolean
g_unix_drive_can_mount (GDrive *drive)
{
  /* TODO */
  return TRUE;
}

static gboolean
g_unix_drive_can_eject (GDrive *drive)
{
  /* TODO */
  return FALSE;
}

static GList *
g_unix_drive_get_volumes (GDrive *drive)
{
  GList *l;
  GUnixDrive *unix_drive = G_UNIX_DRIVE (drive);

  l = NULL;
  if (unix_drive->volume)
    l = g_list_prepend (l, g_object_ref (unix_drive->volume));

  return l;
}

gboolean
g_unix_drive_has_mountpoint (GUnixDrive *drive,
			     const char  *mountpoint)
{
  return strcmp (drive->mountpoint, mountpoint) == 0;
}

static void
g_unix_drive_mount (GDrive         *drive,
		    GMountOperation *mount_operation,
		    GAsyncReadyCallback callback,
		    gpointer        user_data)
{
  /* TODO */
}


static gboolean
g_unix_drive_mount_finish (GDrive *drive,
			   GAsyncResult *result,
			   GError **error)
{
  return TRUE;
}

static void
g_unix_drive_eject (GDrive         *drive,
		    GAsyncReadyCallback callback,
		    gpointer        user_data)
{
  /* TODO */
}

static gboolean
g_unix_drive_eject_finish (GDrive *drive,
			   GAsyncResult *result,
			   GError **error)
{
  return TRUE;
}

static void
g_unix_volume_drive_iface_init (GDriveIface *iface)
{
  iface->get_name = g_unix_drive_get_name;
  iface->get_icon = g_unix_drive_get_icon;
  iface->get_volumes = g_unix_drive_get_volumes;
  iface->is_automounted = g_unix_drive_is_automounted;
  iface->can_mount = g_unix_drive_can_mount;
  iface->can_eject = g_unix_drive_can_eject;
  iface->mount = g_unix_drive_mount;
  iface->mount_finish = g_unix_drive_mount_finish;
  iface->eject = g_unix_drive_eject;
  iface->eject_finish = g_unix_drive_eject_finish;
}
