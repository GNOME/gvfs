#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "gdaemonvolumemonitor.h"
#include "gdaemonvolume.h"
#include "gdaemonfile.h"
#include "gio/gthemedicon.h"

struct _GDaemonVolume {
  GObject     parent;

  GMountInfo *mount_info;
};

static void g_daemon_volume_volume_iface_init (GVolumeIface *iface);

G_DEFINE_TYPE_WITH_CODE (GDaemonVolume, g_daemon_volume, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME,
						g_daemon_volume_volume_iface_init))


static void
g_daemon_volume_finalize (GObject *object)
{
  GDaemonVolume *volume;
  
  volume = G_DAEMON_VOLUME (object);

  g_mount_info_free (volume->mount_info);
  
  if (G_OBJECT_CLASS (g_daemon_volume_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_daemon_volume_parent_class)->finalize) (object);
}

static void
g_daemon_volume_class_init (GDaemonVolumeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_daemon_volume_finalize;
}

static void
g_daemon_volume_init (GDaemonVolume *daemon_volume)
{
}

GDaemonVolume *
g_daemon_volume_new (GVolumeMonitor *volume_monitor,
		     GMountInfo *mount_info)
{
  GDaemonVolume *volume;

  volume = g_object_new (G_TYPE_DAEMON_VOLUME, NULL);
  volume->mount_info = mount_info;

  return volume;
}

GMountInfo *
g_daemon_volume_get_mount_info (GDaemonVolume *volume)
{
  return volume->mount_info;
}

static char *
g_daemon_volume_get_platform_id (GVolume *volume)
{
  /* TODO */

  return NULL;
}

static GFile *
g_daemon_volume_get_root (GVolume *volume)
{
  GDaemonVolume *daemon_volume = G_DAEMON_VOLUME (volume);

  return g_daemon_file_new (daemon_volume->mount_info->mount_spec, "/");
}

static GIcon *
g_daemon_volume_get_icon (GVolume *volume)
{
  GDaemonVolume *daemon_volume = G_DAEMON_VOLUME (volume);

  return g_themed_icon_new (daemon_volume->mount_info->icon);
}

static char *
g_daemon_volume_get_name (GVolume *volume)
{
  GDaemonVolume *daemon_volume = G_DAEMON_VOLUME (volume);

  return g_strdup (daemon_volume->mount_info->display_name);
}

static GDrive *
g_daemon_volume_get_drive (GVolume *volume)
{
  /* TODO */

  return NULL;
}

static gboolean
g_daemon_volume_can_unmount (GVolume *volume)
{
  /* TODO */
  return TRUE;
}

static gboolean
g_daemon_volume_can_eject (GVolume *volume)
{
  /* TODO */
  return FALSE;
}

static void
g_daemon_volume_unmount (GVolume         *volume,
		       GAsyncReadyCallback callback,
		       gpointer         user_data)
{
  /* TODO */
}

static gboolean
g_daemon_volume_unmount_finish (GVolume *volume,
			      GAsyncResult *result,
			      GError **error)
{
  return TRUE;
}

static void
g_daemon_volume_eject (GVolume         *volume,
		     GAsyncReadyCallback callback,
		     gpointer         user_data)
{
  /* TODO */
}

static gboolean
g_daemon_volume_eject_finish (GVolume *volume,
			    GAsyncResult *result,
			    GError **error)
{
  return TRUE;
}

static void
g_daemon_volume_volume_iface_init (GVolumeIface *iface)
{
  iface->get_root = g_daemon_volume_get_root;
  iface->get_name = g_daemon_volume_get_name;
  iface->get_icon = g_daemon_volume_get_icon;
  iface->get_drive = g_daemon_volume_get_drive;
  iface->can_unmount = g_daemon_volume_can_unmount;
  iface->can_eject = g_daemon_volume_can_eject;
  iface->unmount = g_daemon_volume_unmount;
  iface->unmount_finish = g_daemon_volume_unmount_finish;
  iface->eject = g_daemon_volume_eject;
  iface->eject_finish = g_daemon_volume_eject_finish;
  iface->get_platform_id = g_daemon_volume_get_platform_id;
}
