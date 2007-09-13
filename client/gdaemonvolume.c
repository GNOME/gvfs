#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "gdaemonvolumemonitor.h"
#include "gdaemonvolume.h"
#include "gdaemondrive.h"

struct _GDaemonVolume {
  GObject     parent;

  GMountRef *mount_info;
  char       *name;
  char       *icon;
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

  g_free (volume->name);
  g_free (volume->icon);
  _g_mount_ref_unref (volume->mount_info);
  
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
		     GMountRef *mount_info)
{
  GDaemonVolume *volume;
  char *volume_name;

  volume = g_object_new (G_TYPE_DAEMON_VOLUME, NULL);
  volume->mount_info = _g_mount_ref_ref (mount_info);

  volume_name = NULL;

  if (volume_name == NULL)
    {
      /* TODO: Use volume size as name? */
      volume_name = g_strdup (_("Unknown volume"));
    }
  
  volume->name = volume_name;

  /* TODO: Figure out a better icon */
  volume->icon = g_strdup ("network");

  return volume;
}

static char *
g_daemon_volume_get_platform_id (GVolume *volume)
{
  GDaemonVolume *daemon_volume = G_DAEMON_VOLUME (volume);

  return g_strdup (daemon_volume->mount_info->spec->mount_prefix);
}

static GFile *
g_daemon_volume_get_root (GVolume *volume)
{
  GDaemonVolume *daemon_volume = G_DAEMON_VOLUME (volume);

  return g_file_get_for_path (daemon_volume->mount_info->spec->mount_prefix);
}

static char *
g_daemon_volume_get_icon (GVolume *volume)
{
  GDaemonVolume *daemon_volume = G_DAEMON_VOLUME (volume);

  return g_strdup (daemon_volume->icon);
}

static char *
g_daemon_volume_get_name (GVolume *volume)
{
  GDaemonVolume *daemon_volume = G_DAEMON_VOLUME (volume);
  
  return g_strdup (daemon_volume->name);
}

static GDrive *
g_daemon_volume_get_drive (GVolume *volume)
{
  GDaemonVolume *daemon_volume = G_DAEMON_VOLUME (volume);

  daemon_volume = NULL;

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
