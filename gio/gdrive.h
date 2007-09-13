#ifndef __G_DRIVE_H__
#define __G_DRIVE_H__

#include <glib-object.h>
#include <gio/gvolume.h>
#include <gio/gmountoperation.h>

G_BEGIN_DECLS

#define G_TYPE_DRIVE           (g_drive_get_type ())
#define G_DRIVE(obj)           (G_TYPE_CHECK_INSTANCE_CAST ((obj), G_TYPE_DRIVE, GDrive))
#define G_IS_DRIVE(obj)	       (G_TYPE_CHECK_INSTANCE_TYPE ((obj), G_TYPE_DRIVE))
#define G_DRIVE_GET_IFACE(obj) (G_TYPE_INSTANCE_GET_INTERFACE ((obj), G_TYPE_DRIVE, GDriveIface))

typedef struct _GDriveIface    GDriveIface;

struct _GDriveIface
{
  GTypeInterface g_iface;

  /* signals */
  void (*changed)            (GVolume *volume);
  
  /* Virtual Table */
  
  char *   (*get_name)    (GDrive         *drive);
  GIcon *  (*get_icon)    (GDrive         *drive);
  GList *  (*get_volumes) (GDrive         *drive);
  gboolean (*is_automounted)(GDrive       *drive);
  gboolean (*can_mount)   (GDrive         *drive);
  gboolean (*can_eject)   (GDrive         *drive);
  void     (*mount)       (GDrive         *drive,
			   GMountOperation *mount_operation,
			   GAsyncReadyCallback callback,
			   gpointer        user_data);
  gboolean (*mount_finish)(GDrive         *drive,
			   GAsyncResult   *result,
			   GError        **error);
  void     (*eject)       (GDrive         *drive,
			   GAsyncReadyCallback callback,
			   gpointer        user_data);
  gboolean (*eject_finish)(GDrive         *drive,
			   GAsyncResult   *result,
			   GError        **error);
};

GType g_drive_get_type (void) G_GNUC_CONST;

char *   g_drive_get_name       (GDrive               *drive);
GIcon *  g_drive_get_icon       (GDrive               *drive);
GList  * g_drive_get_volumes    (GDrive               *drive);
gboolean g_drive_is_automounted (GDrive               *drive);
gboolean g_drive_can_mount      (GDrive               *drive);
gboolean g_drive_can_eject      (GDrive               *drive);
void     g_drive_mount          (GDrive               *drive,
				 GMountOperation      *mount_operation,
				 GAsyncReadyCallback   callback,
				 gpointer              user_data);
gboolean g_drive_mount_finish   (GDrive               *drive,
				 GAsyncResult         *result,
				 GError              **error);
void     g_drive_eject          (GDrive               *drive,
				 GAsyncReadyCallback   callback,
				 gpointer              user_data);
gboolean g_drive_eject_finish   (GDrive               *drive,
				 GAsyncResult         *result,
				 GError              **error);

G_END_DECLS

#endif /* __G_DRIVE_H__ */
