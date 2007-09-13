#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "gunixdrive.h"
#include "gdrivepriv.h"
#include "gvolumemonitor.h"

struct _GUnixDrive {
  GObject parent;
  GVolumeMonitor *monitor;

  GVolume *volume;
  char *name;
  char *icon;
  char *mountpoint;
};

static void g_unix_volue_drive_iface_init (GDriveIface *iface);

G_DEFINE_TYPE_WITH_CODE (GUnixDrive, g_unix_drive, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_DRIVE,
						g_unix_volue_drive_iface_init))


static void
g_unix_drive_finalize (GObject *object)
{
  GUnixDrive *drive;
  
  drive = G_UNIX_DRIVE (object);

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

GUnixDrive *
g_unix_drive_new (GVolumeMonitor *volume_monitor,
		  GUnixMountPoint *mountpoint)
{
  GUnixDrive *drive;
  GUnixMountType type;
  
  if (!(mountpoint->is_user_mountable ||
	g_str_has_prefix (mountpoint->device_path, "/vol/")) ||
      mountpoint->is_loopback)
    return NULL;
  
  drive = g_object_new (G_TYPE_UNIX_DRIVE, NULL);
  drive->monitor = volume_monitor;

  type = _g_guess_type_for_mount (mountpoint->mount_path,
				  mountpoint->device_path,
				  mountpoint->filesystem_type);
  
  /* TODO: */
  drive->mountpoint = g_strdup (mountpoint->mount_path);
  drive->icon = g_strdup_printf ("drive type %d", type);
  drive->name = g_strdup (_("Unknown drive"));
  
  return drive;
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

gboolean
g_unix_drive_has_mountpoint (GUnixDrive *drive,
			     const char  *mountpoint)
{
  return strcmp (drive->mountpoint, mountpoint) == 0;
}

static void
g_unix_volue_drive_iface_init (GDriveIface *iface)
{
  iface->get_name = g_unix_drive_get_name;
  iface->get_icon = g_unix_drive_get_icon;
}
