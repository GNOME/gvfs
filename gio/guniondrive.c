#include <config.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "guniondrive.h"
#include "gdrivepriv.h"

typedef struct {
  GDrive *drive;
  GVolumeMonitor *monitor;
} ChildDrive;

struct _GUnionDrive {
  GObject parent;

  GList *child_drives;
};

static void g_union_volue_drive_iface_init (GDriveIface *iface);

G_DEFINE_TYPE_WITH_CODE (GUnionDrive, g_union_drive, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_DRIVE,
						g_union_volue_drive_iface_init))
  
static void
g_union_drive_finalize (GObject *object)
{
  GUnionDrive *drive;
  ChildDrive *child;
  GList *l;
  
  drive = G_UNION_DRIVE (object);

  for (l = drive->child_drives; l != NULL; l = l->next)
    {
      child = l->data;

      g_object_unref (child->drive);
      g_object_unref (child->monitor);
      g_free (child);
    }
  g_list_free (drive->child_drives);
  
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

GUnionDrive *
g_union_drive_new (GDrive *child_drive,
		    GVolumeMonitor *child_monitor)
{
  GUnionDrive *drive;
  ChildDrive *child;
  
  drive = g_object_new (G_TYPE_UNION_DRIVE, NULL);

  child = g_new (ChildDrive, 1);
  child->drive = g_object_ref (child_drive);
  child->monitor = g_object_ref (child_monitor);

  drive->child_drives = g_list_prepend (drive->child_drives, child);

  return drive;
}

void
g_union_drive_add_drive (GUnionDrive *union_drive,
			 GDrive *child_drive,
			 GVolumeMonitor *child_monitor)
{
  ChildDrive *child;
  
  child = g_new (ChildDrive, 1);
  child->drive = g_object_ref (child_drive);
  child->monitor = g_object_ref (child_monitor);

  union_drive->child_drives = g_list_prepend (union_drive->child_drives, child);

  g_signal_emit_by_name (union_drive, "changed");
}

gboolean
g_union_drive_remove_drive (GUnionDrive *union_drive,
			    GDrive *child_drive)
{
  GList *l;
  ChildDrive *child;

  for (l = union_drive->child_drives; l != NULL; l = l->next)
    {
      child = l->data;

      if (child->drive == child_drive)
	{
	  union_drive->child_drives = g_list_delete_link (union_drive->child_drives, l);
	  g_object_unref (child->drive);
	  g_object_unref (child->monitor);
	  g_free (child);
	  
	  g_signal_emit_by_name (union_drive, "changed");
	  
	  return union_drive->child_drives == NULL;
	}
    }
  
  return FALSE;
}

GDrive *
g_union_drive_get_child_for_monitor (GUnionDrive *union_drive,
				     GVolumeMonitor *child_monitor)
{
  GList *l;
  ChildDrive *child;
  
  for (l = union_drive->child_drives; l != NULL; l = l->next)
    {
      child = l->data;

      if (child->monitor == child_monitor)
	return g_object_ref (child->drive);
    }
  
  return FALSE;
}

gboolean
g_union_drive_has_child_drive (GUnionDrive *union_drive,
			       GDrive *child_drive)
{
  GList *l;
  ChildDrive *child;

  for (l = union_drive->child_drives; l != NULL; l = l->next)
    {
      child = l->data;

      if (child->drive == child_drive)
	return TRUE;
    }
  
  return FALSE;
}

static char *
g_union_drive_get_name (GDrive *drive)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);
  ChildDrive *child;

  if (union_drive->child_drives)
    {
      child = union_drive->child_drives->data;
      return g_drive_get_name (child->drive);
    }
  return g_strdup ("drive");
}

static char *
g_union_drive_get_icon (GDrive *drive)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);
  ChildDrive *child;

  if (union_drive->child_drives)
    {
      child = union_drive->child_drives->data;
      return g_drive_get_icon (child->drive);
    }
  return NULL;
}

static gboolean
g_union_drive_is_automounted (GDrive *drive)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);
  ChildDrive *child;

  if (union_drive->child_drives)
    {
      child = union_drive->child_drives->data;
      return g_drive_is_automounted (child->drive);
    }
  return FALSE;
}

static GList *
g_union_drive_get_volumes (GDrive *drive)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);
  ChildDrive *child;

  if (union_drive->child_drives)
    {
      child = union_drive->child_drives->data;
      return g_drive_get_volumes (child->drive);
    }
  return NULL;
}

static gboolean
g_union_drive_can_mount (GDrive *drive)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);
  ChildDrive *child;

  if (union_drive->child_drives)
    {
      child = union_drive->child_drives->data;
      return g_drive_can_mount (child->drive);
    }
  return FALSE;
}

static gboolean
g_union_drive_can_eject (GDrive *drive)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);
  ChildDrive *child;

  if (union_drive->child_drives)
    {
      child = union_drive->child_drives->data;
      return g_drive_can_eject (child->drive);
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
g_union_drive_mount (GDrive         *drive,
		     GMountOperation *mount_operation,
		     GVolumeCallback callback,
		     gpointer        user_data)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);
  ChildDrive *child;
  VolData *data;

  if (union_drive->child_drives)
    {
      child = union_drive->child_drives->data;
      return g_drive_mount (child->drive,
			    mount_operation,
			    callback, user_data);
    }

  data = g_new0 (VolData, 1);
  data->callback = callback;
  data->user_data = user_data;
  g_idle_add (error_on_idle, data);
}

static void
g_union_drive_eject (GDrive         *drive,
		     GVolumeCallback callback,
		     gpointer        user_data)
{
  GUnionDrive *union_drive = G_UNION_DRIVE (drive);
  ChildDrive *child;
  VolData *data;

  if (union_drive->child_drives)
    {
      child = union_drive->child_drives->data;
      return g_drive_eject (child->drive,
			     callback, user_data);
    }
  
  data = g_new0 (VolData, 1);
  data->callback = callback;
  data->user_data = user_data;
  g_idle_add (error_on_idle, data);
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
